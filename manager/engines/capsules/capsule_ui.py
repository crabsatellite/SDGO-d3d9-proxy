"""Loopback-only capsule catalog editor.

The server edits one catalog selected on the command line. Browser requests
cannot choose filesystem paths, run SQL, or reach a non-loopback interface.
"""
from __future__ import annotations

import copy
import hashlib
import hmac
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import tempfile
import threading

from .capsule_catalog import (
    capsule_display,
    custom_candidate_allowed,
    digest,
    export_bundle,
    initialize,
    mutate,
    patterns as catalog_patterns,
    read_json,
    validate,
    write_json,
)


MAX_REQUEST_BYTES = 16 * 1024 * 1024
MACHINE_KINDS = ("fixed_machines", "custom_machines")
ASSET_RE = re.compile(r"^/(?:api/)?assets/unit/([1-9][0-9]*)\.png$")


class RequestError(Exception):
    status = 400


class ConflictError(RequestError):
    status = 409


class MissingError(RequestError):
    status = 404


def _json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")


def _load_catalog_bytes(raw):
    value = json.loads(raw.decode("utf-8-sig"))
    validate(value)
    return value


def _machine_positions(catalog):
    return {
        (kind, machine["id"]): index
        for kind in MACHINE_KINDS
        for index, machine in enumerate(catalog[kind], 1)
    }


def _native_positions(catalog):
    result = {}
    for kind in MACHINE_KINDS:
        machines = [machine for machine in catalog[kind]
                    if machine.get("enabled", False) and not machine.get("retired", False)]
        # Native 0x007A6070 reverses the ascending 0033 grid records before
        # 0x0079E620 paginates them; the last configured fixed machine is first.
        if kind == "fixed_machines":
            machines.reverse()
        for position, machine in enumerate(machines, 1):
            result[(kind, machine["id"])] = position
    return result


def _display_location(kind, position):
    if position is None:
        return None
    if kind == "fixed_machines":
        return {
            "position": position,
            "page": (position - 1) // 8 + 1,
            "slot": (position - 1) % 8 + 1,
            "slots_per_page": 8,
        }
    return {"position": position}


def _position_changes(before, after):
    old_positions = _machine_positions(before)
    native_positions = _native_positions(after)
    result = []
    for (kind, machine_id), position in _machine_positions(after).items():
        old = old_positions.get((kind, machine_id))
        if old != position:
            native_location = _display_location(
                kind, native_positions.get((kind, machine_id))
            )
            result.append(
                {
                    "kind": kind,
                    "id": machine_id,
                    "old_position": old,
                    "position": position,
                    "location": native_location,
                    "native_location": native_location,
                }
            )
    return result


def _kind(value):
    if value not in MACHINE_KINDS:
        raise RequestError("Unknown machine kind")
    return value


def _positive_int(value, name):
    if type(value) is not int or value <= 0:
        raise RequestError(f"{name} must be a positive integer")
    return value


def _reposition(catalog, kind, machine_id, position):
    if position is None:
        return
    if type(position) is not int:
        raise RequestError("Position must be an integer")
    items = catalog[kind]
    if not 1 <= position <= len(items):
        raise RequestError(f"Position must be between 1 and {len(items)}")
    index = next((i for i, item in enumerate(items) if item["id"] == machine_id), None)
    if index is None:
        raise MissingError("Machine not found")
    item = items.pop(index)
    items.insert(position - 1, item)
    validate(catalog)


def _validate_editor_scope(catalog):
    for machine in catalog["fixed_machines"]:
        if machine.get("currency", "GP") != "GP":
            raise ValueError(
                "This runtime supports new fixed capsule machines in GP only"
            )
    for machine in catalog["custom_machines"]:
        if machine.get("currency_type", 0) not in (0, 1):
            raise ValueError("Custom capsule currency_type must be 0 (GP) or 1 (coin)")


def _compile_catalog(catalog, client, lookup, output):
    # Imported lazily so catalog-only authoring does not require the native
    # compiler's image dependencies.
    from .capsule_client_compile import compile_catalog

    return compile_catalog(catalog, client, lookup, output)


class CatalogStore:
    """Serializes catalog writes and refuses to overwrite external changes."""

    def __init__(
        self,
        catalog_path,
        *,
        patterns_path=None,
        assets_path=None,
        bundle_path=None,
        snapshot_path=None,
        client_path=None,
        lookup_path=None,
        report_path=None,
        apply_callback=None,
    ):
        self.catalog_path = Path(catalog_path).resolve()
        self.patterns_path = Path(patterns_path).resolve() if patterns_path else None
        self.assets_path = Path(assets_path).resolve() if assets_path else None
        self.bundle_path = (
            Path(bundle_path).resolve()
            if bundle_path
            else (self.catalog_path.parent / "bundle").resolve()
        )
        self.snapshot_path = Path(snapshot_path).resolve() if snapshot_path else None
        self.client_path = Path(client_path).resolve() if client_path else None
        self.lookup_path = Path(lookup_path).resolve() if lookup_path else None
        self.report_path = (
            Path(report_path).resolve()
            if report_path
            else (self.catalog_path.parent / "initialization-report.json").resolve()
        )
        self.apply_callback = apply_callback
        if self.apply_callback is not None and not callable(self.apply_callback):
            raise ValueError("apply_callback must be callable")
        if self.snapshot_path is not None and self.client_path is None:
            raise ValueError("--snapshot requires --client")
        if self.lookup_path is not None and self.client_path is None:
            raise ValueError("--lookup requires --client")
        if self.apply_callback is not None and (
            self.client_path is None or self.lookup_path is None
        ):
            raise ValueError("Apply requires client compilation configuration")
        for label, path, expected in (
            ("--patterns", self.patterns_path, "file"),
            ("--assets", self.assets_path, "directory"),
            ("--snapshot", self.snapshot_path, "directory"),
            ("--client", self.client_path, "directory"),
            ("--lookup", self.lookup_path, "file"),
        ):
            if path is None:
                continue
            valid = path.is_file() if expected == "file" else path.is_dir()
            if not valid:
                raise ValueError(f"{label} must name an existing {expected}: {path}")
        self.lock = threading.RLock()
        self.read()

    @property
    def can_initialize(self):
        return self.snapshot_path is not None and self.client_path is not None

    @property
    def can_compile(self):
        return self.client_path is not None and self.lookup_path is not None

    @property
    def can_apply(self):
        return self.apply_callback is not None and self.can_compile

    def read(self):
        with self.lock:
            raw = self.catalog_path.read_bytes()
            return _load_catalog_bytes(raw), raw

    def _expect(self, catalog, expected_revision):
        current = digest(catalog)
        if not isinstance(expected_revision, str) or not hmac.compare_digest(
            current, expected_revision
        ):
            raise ConflictError(
                "Catalog changed since it was loaded. Reload before saving."
            )
        return current

    def _backup(self, raw):
        suffix = hashlib.sha256(raw).hexdigest()[:12]
        path = self.catalog_path.with_name(f"{self.catalog_path.name}.{suffix}.bak")
        try:
            with path.open("xb") as handle:
                handle.write(raw)
                handle.flush()
                os.fsync(handle.fileno())
        except FileExistsError:
            pass
        return path

    def _save(self, new_catalog, previous_raw):
        validate(new_catalog)
        _validate_editor_scope(new_catalog)
        data = _json_bytes(new_catalog)
        self.catalog_path.parent.mkdir(parents=True, exist_ok=True)
        backup = self._backup(previous_raw)
        descriptor, temporary = tempfile.mkstemp(
            prefix=f".{self.catalog_path.name}.",
            suffix=".pending",
            dir=self.catalog_path.parent,
        )
        temporary_path = Path(temporary)
        try:
            with os.fdopen(descriptor, "wb") as handle:
                handle.write(data)
                handle.flush()
                os.fsync(handle.fileno())
            if self.catalog_path.read_bytes() != previous_raw:
                raise ConflictError(
                    "Catalog was modified by another process while saving."
                )
            os.replace(temporary_path, self.catalog_path)
        finally:
            if temporary_path.exists():
                temporary_path.unlink()
        return backup

    def patterns(self, catalog):
        if self.patterns_path and self.patterns_path.exists():
            value = read_json(self.patterns_path)
            if not isinstance(value, list):
                raise ValueError("Patterns file must contain an array")
            return value
        native = {
            **catalog,
            "fixed_machines": [
                machine
                for machine in catalog["fixed_machines"]
                if machine.get("owner") == "native"
            ],
        }
        return catalog_patterns(native)

    def has_asset(self, unit_id):
        return bool(
            self.assets_path
            and (self.assets_path / "units" / f"{int(unit_id)}.png").is_file()
        )

    def asset(self, unit_id):
        if not self.assets_path:
            raise MissingError("Unit assets are not configured")
        path = self.assets_path / "units" / f"{unit_id}.png"
        if not path.is_file():
            raise MissingError("Unit preview is unavailable")
        return path

    def _result(self, catalog, machine=None, backup=None):
        result = {
            "catalog_revision": digest(catalog),
            "counts": {
                "fixed_machines": len(catalog["fixed_machines"]),
                "custom_machines": len(catalog["custom_machines"]),
            },
        }
        if backup is not None:
            result["backup_path"] = str(backup)
        if machine is not None:
            kind, machine_id = machine
            position = _machine_positions(catalog).get((kind, machine_id))
            native_position = _native_positions(catalog).get((kind, machine_id))
            native_location = _display_location(kind, native_position)
            item = next(
                (entry for entry in catalog[kind] if entry["id"] == machine_id),
                None,
            )
            result["machine"] = {
                "kind": kind,
                "id": machine_id,
                "position": position,
                "location": native_location,
                "native_location": native_location,
                "value": item,
            }
        return result

    def mutate_machine(
        self, action, kind, machine_id, expected_revision, value=None, position=None
    ):
        kind = _kind(kind)
        machine_id = _positive_int(machine_id, "Machine ID")
        with self.lock:
            catalog, raw = self.read()
            self._expect(catalog, expected_revision)
            if action == "restore":
                result = mutate(
                    catalog,
                    kind,
                    "update",
                    machine_id,
                    {"enabled": True, "retired": False},
                )
            elif action in ("create", "update", "delete"):
                result = mutate(catalog, kind, action, machine_id, value)
            else:
                raise RequestError("Unknown machine action")
            _reposition(result, kind, machine_id, position)
            backup = self._save(result, raw)
            return self._result(result, (kind, machine_id), backup)

    def replace(self, incoming, expected_revision):
        if not isinstance(incoming, dict):
            raise RequestError("Imported catalog must be a JSON object")
        incoming = copy.deepcopy(incoming)
        with self.lock:
            catalog, raw = self.read()
            self._expect(catalog, expected_revision)
            if "custom_candidate_policy" in catalog:
                if incoming.get("source_identity", {}) != catalog.get("source_identity", {}):
                    raise RequestError("Import requires the same source catalog; use a separate workspace for another source")
                # Older exported JSON has no policy fields. Rebind it to this
                # workspace's source facts before validation, rather than
                # silently restoring the old unrestricted S/SS fallback.
                incoming["custom_candidate_policy"] = copy.deepcopy(catalog["custom_candidate_policy"])
                source_units = {unit["id"]: unit for unit in catalog["units"]}
                for unit in incoming.get("units", []):
                    source = source_units.get(unit.get("id"))
                    if source is None:
                        raise RequestError("Imported unit is absent from this workspace's source")
                    for key in ("rank", "subgrade", "grade", "has_blueprint_recipe"):
                        if key in source:
                            unit[key] = copy.deepcopy(source[key])
            validate(incoming)
            changes = _position_changes(catalog, incoming)
            backup = self._save(incoming, raw)
            result = self._result(incoming, backup=backup)
            result["position_changes"] = changes
            return result

    def export(self, expected_revision, selected=None):
        with self.lock:
            catalog, _ = self.read()
            self._expect(catalog, expected_revision)
            export_bundle(catalog, self.bundle_path)
            manifest_path = self.bundle_path / "manifest.json"
            manifest = read_json(manifest_path)
            result = self._result(catalog)
            result.update(
                {
                    "bundle_path": str(self.bundle_path),
                    "manifest_path": str(manifest_path),
                    "manifest": manifest,
                    "files": ["client.json", "server.json", "manifest.json"],
                }
            )
            if isinstance(selected, dict):
                kind = selected.get("kind")
                machine_id = selected.get("id")
                if kind in MACHINE_KINDS and type(machine_id) is int:
                    position = _machine_positions(catalog).get((kind, machine_id))
                    native_position = _native_positions(catalog).get(
                        (kind, machine_id)
                    )
                    if position is not None:
                        native_location = _display_location(kind, native_position)
                        result["selected_position"] = {
                            "kind": kind,
                            "id": machine_id,
                            "position": position,
                            "location": native_location,
                            "native_location": native_location,
                        }
            return result

    @staticmethod
    def _validate_compiled_output(output, revision):
        from .capsule_client_compile import COMPILER_VERSION
        output = Path(output).resolve()
        manifest_path = output / "manifest.json"
        placements_path = output / "placements.json"
        if not manifest_path.is_file() or not placements_path.is_file():
            raise ValueError("Existing runtime output is incomplete")
        manifest = read_json(manifest_path)
        if manifest.get("compiler_version") != COMPILER_VERSION:
            raise ValueError("Runtime output was made by a different compiler version")
        placements = read_json(placements_path)
        if (
            manifest.get("catalog_revision") != revision
            or placements.get("catalog_revision") != revision
        ):
            raise ValueError("Existing runtime output has a different catalog revision")
        for relative, expected_hash in manifest.get("files", {}).items():
            relative_path = Path(relative)
            if relative_path.is_absolute() or ".." in relative_path.parts:
                raise ValueError("Runtime manifest contains an unsafe file path")
            path = (output / relative_path).resolve()
            try:
                path.relative_to(output)
            except ValueError:
                raise ValueError("Runtime manifest file escapes the output directory")
            if (
                not path.is_file()
                or hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash
            ):
                raise ValueError(f"Runtime output hash mismatch: {relative}")
        if not isinstance(placements.get("machines"), list):
            raise ValueError("Runtime placements are missing")
        return manifest, placements

    def compile(self, expected_revision, selected=None):
        if not self.can_compile:
            raise RequestError(
                "Client compilation requires --client and --lookup at server startup"
            )
        with self.lock:
            catalog, _ = self.read()
            revision = self._expect(catalog, expected_revision)
            runtime_root = (self.bundle_path / "runtime" / revision[:16]).resolve()
            from .capsule_client_compile import COMPILER_VERSION, extension_fingerprint
            extension_signature = extension_fingerprint(self.client_path)
            output = runtime_root / f"client-v{COMPILER_VERSION}-{extension_signature[:12]}"
            reused = False
            generated_cards = None
            if output.exists():
                manifest, placements = self._validate_compiled_output(output, revision)
                reused = True
            else:
                runtime_root.mkdir(parents=True, exist_ok=True)
                temporary = runtime_root / f".client.{secrets.token_hex(8)}.pending"
                try:
                    result = _compile_catalog(
                        catalog, self.client_path, self.lookup_path, temporary
                    )
                    manifest, placements = self._validate_compiled_output(
                        temporary, revision
                    )
                    if manifest.get('preserved_extensions_signature') != extension_signature:
                        raise ConflictError('Shared capsule resources changed while compiling; refresh and retry')
                    if output.exists():
                        raise ConflictError(
                            "Another process created this runtime revision while compiling"
                        )
                    temporary.replace(output)
                    generated_cards = result.get("generated_cards")
                finally:
                    if temporary.exists():
                        expected_parent = runtime_root.resolve()
                        resolved = temporary.resolve()
                        if resolved.parent != expected_parent or not resolved.name.startswith(
                            ".client."
                        ):
                            raise RuntimeError(
                                "Refusing to clean an unexpected compiler path"
                            )
                        shutil.rmtree(resolved)
            result = {
                "catalog_revision": revision,
                "output": str(output),
                "manifest_path": str(output / "manifest.json"),
                "placements_path": str(output / "placements.json"),
                "counts": manifest.get("counts", {}),
                "generated_cards": generated_cards,
                "reused": reused,
                "placements": placements["machines"],
            }
            if isinstance(selected, dict):
                match = next(
                    (
                        item
                        for item in placements["machines"]
                        if item.get("kind") == selected.get("kind")
                        and item.get("id") == selected.get("id")
                    ),
                    None,
                )
                if match is not None:
                    result["selected_placement"] = match
            return result

    def apply(self, expected_revision, selected=None):
        if not self.can_apply:
            raise RequestError(
                "Game import requires a deployment target configured at server startup"
            )
        with self.lock:
            catalog, _ = self.read()
            revision = self._expect(catalog, expected_revision)
            compiled = self.compile(revision, selected)
            current, _ = self.read()
            self._expect(current, revision)
            try:
                deployment = self.apply_callback(
                    copy.deepcopy(catalog),
                    Path(compiled["output"]).resolve(),
                    self.bundle_path,
                )
            except (RequestError, ValueError, OSError):
                raise
            except Exception as error:
                raise RequestError(str(error)) from error
            if not isinstance(deployment, dict):
                raise RequestError("Deployment callback must return a result object")
            result = dict(deployment)
            result.update(
                {
                    "applied": deployment.get("applied") is True,
                    "catalog_revision": revision,
                    "runtime_output": compiled["output"],
                    "manifest_path": compiled["manifest_path"],
                    "placements_path": compiled["placements_path"],
                    "counts": compiled["counts"],
                    "placements": compiled["placements"],
                    "compiled_reused": compiled["reused"],
                }
            )
            if "selected_placement" in compiled:
                result["selected_placement"] = compiled["selected_placement"]
            return result

    def _verify_initialization_sources(self, catalog):
        if not self.can_initialize:
            raise RequestError(
                "Initialization requires --snapshot and --client at server startup"
            )
        source = catalog.get("source_identity", {})
        _, client_identity = capsule_display(self.client_path)
        for key, value in client_identity.items():
            if source.get(key) != value:
                raise ConflictError(
                    "Configured client differs from the catalog source."
                )
        meta_path = self.snapshot_path / "_meta.json"
        if meta_path.exists():
            meta = read_json(meta_path)
            if "files" in meta and source.get("snapshot_fingerprint") != digest(
                meta["files"]
            ):
                raise ConflictError(
                    "Configured snapshot differs from the catalog source."
                )

    def initialize(self, expected_revision, price):
        if type(price) is not int or price < 0:
            raise RequestError("Initialization price must be a nonnegative integer")
        with self.lock:
            catalog, raw = self.read()
            self._expect(catalog, expected_revision)
            self._verify_initialization_sources(catalog)
            result, report = initialize(self.snapshot_path, catalog, price)
            backup = self._save(result, raw)
            if report["seeds"] or not self.report_path.exists():
                write_json(self.report_path, report)
            response = self._result(result, backup=backup)
            response.update(
                {
                    "report_path": str(self.report_path),
                    "report": report,
                    "new_machine_positions": [
                        {
                            "kind": "fixed_machines",
                            "id": machine_id,
                            "position": _machine_positions(result)[
                                ("fixed_machines", machine_id)
                            ],
                            "location": _display_location(
                                "fixed_machines",
                                _native_positions(result)[
                                    ("fixed_machines", machine_id)
                                ],
                            ),
                            "native_location": _display_location(
                                "fixed_machines",
                                _native_positions(result)[
                                    ("fixed_machines", machine_id)
                                ],
                            ),
                        }
                        for machine_id in report["new_fixed_machine_ids"]
                    ],
                }
            )
            return response


class CapsuleApplication:
    def __init__(self, store):
        self.store = store

    @staticmethod
    def _machine_summary(machine, position, native_position, kind):
        native_location = _display_location(kind, native_position)
        return {
            "kind": kind,
            "id": machine["id"],
            "name": machine.get("name", f"Machine {machine['id']}"),
            "position": position,
            "location": native_location,
            "native_location": native_location,
            "enabled": machine.get("enabled", False),
            "retired": machine.get("retired", False),
            "owner": machine.get("owner", "unknown"),
            "price": machine.get("price", 0),
            "currency": (
                machine.get("currency", "GP")
                if kind == "fixed_machines"
                else machine.get("currency_type", 0)
            ),
            "draw_policy": machine.get("draw_policy", ""),
        }

    def bootstrap(self):
        catalog, _ = self.store.read()
        native_positions = _native_positions(catalog)
        machine_summaries = {
            kind: [
                self._machine_summary(
                    machine,
                    index,
                    native_positions.get((kind, machine["id"])),
                    kind,
                )
                for index, machine in enumerate(catalog[kind], 1)
            ]
            for kind in MACHINE_KINDS
        }
        native_custom_ids = set(catalog.get("custom_candidate_policy", {}).get("native_candidate_unit_ids", []))
        units = [
            {
                "id": unit["id"],
                "name": unit.get("name", str(unit["id"])),
                "grade": unit.get("grade", ""),
                "rank": unit.get("rank"),
                "subgrade": unit.get("subgrade"),
                "eligible": unit.get("eligible", True),
                "has_blueprint_recipe": unit.get("has_blueprint_recipe"),
                "can_add_custom": unit["id"] in native_custom_ids or custom_candidate_allowed(catalog, unit),
                "capsule_sprites": unit.get("capsule_sprites", []),
                "has_asset": self.store.has_asset(unit["id"]),
                "asset_url": f"/assets/unit/{unit['id']}.png",
            }
            for unit in catalog["units"]
        ]
        return {
            "catalog_revision": digest(catalog),
            "catalog_path": str(self.store.catalog_path),
            "bundle_path": str(self.store.bundle_path),
            "counts": {
                "units": len(units),
                **{kind: len(machine_summaries[kind]) for kind in MACHINE_KINDS},
            },
            "capabilities": {
                "assets": self.store.assets_path is not None,
                "initialize": self.store.can_initialize,
                "compile": self.store.can_compile,
                "apply": self.store.can_apply,
                "export": True,
            },
            "machines": machine_summaries,
            "units": units,
            "patterns": self.store.patterns(catalog),
        }

    def machine(self, kind, machine_id):
        kind = _kind(kind)
        machine_id = _positive_int(machine_id, "Machine ID")
        catalog, _ = self.store.read()
        native_positions = _native_positions(catalog)
        for position, machine in enumerate(catalog[kind], 1):
            if machine["id"] == machine_id:
                native_location = _display_location(
                    kind, native_positions.get((kind, machine_id))
                )
                return {
                    "catalog_revision": digest(catalog),
                    "kind": kind,
                    "position": position,
                    "location": native_location,
                    "native_location": native_location,
                    "machine": machine,
                }
        raise MissingError("Machine not found")

    def units(self, query, limit):
        catalog, _ = self.store.read()
        native_custom_ids = set(catalog.get("custom_candidate_policy", {}).get("native_candidate_unit_ids", []))
        normalized = query.casefold().strip()
        if type(limit) is not int or not 1 <= limit <= 500:
            raise RequestError("Unit result limit must be between 1 and 500")
        result = []
        for unit in catalog["units"]:
            haystack = f"{unit['id']} {unit.get('name', '')} {unit.get('grade', '')}".casefold()
            if normalized and normalized not in haystack:
                continue
            result.append(
                {
                    "id": unit["id"],
                    "name": unit.get("name", str(unit["id"])),
                    "grade": unit.get("grade", ""),
                    "rank": unit.get("rank"),
                    "subgrade": unit.get("subgrade"),
                    "eligible": unit.get("eligible", True),
                    "has_blueprint_recipe": unit.get("has_blueprint_recipe"),
                    "can_add_custom": unit["id"] in native_custom_ids or custom_candidate_allowed(catalog, unit),
                    "capsule_sprites": unit.get("capsule_sprites", []),
                    "has_asset": self.store.has_asset(unit["id"]),
                    "asset_url": f"/assets/unit/{unit['id']}.png",
                }
            )
            if len(result) >= limit:
                break
        return {"units": result, "query": query, "limit": limit}


    def dispatch_post(self, path, payload):
        expected = payload.get("expected_revision")
        if path == "/api/machines/create":
            machine = payload.get("machine")
            if not isinstance(machine, dict):
                raise RequestError("Create requires a machine object")
            return self.store.mutate_machine(
                "create",
                payload.get("kind"),
                machine.get("id"),
                expected,
                machine,
                payload.get("position"),
            )
        if path == "/api/machines/update":
            machine_id = payload.get("id")
            value = payload.get("machine")
            if not isinstance(value, dict):
                raise RequestError("Update requires a machine object")
            return self.store.mutate_machine(
                "update",
                payload.get("kind"),
                machine_id,
                expected,
                value,
                payload.get("position"),
            )
        if path == "/api/machines/retire":
            return self.store.mutate_machine(
                "delete",
                payload.get("kind"),
                payload.get("id"),
                expected,
            )
        if path == "/api/machines/restore":
            return self.store.mutate_machine(
                "restore",
                payload.get("kind"),
                payload.get("id"),
                expected,
            )
        if path == "/api/catalog/import":
            return self.store.replace(payload.get("catalog"), expected)
        if path == "/api/export":
            return self.store.export(expected, payload.get("selected"))
        if path == "/api/compile":
            return self.store.compile(expected, payload.get("selected"))
        if path == "/api/apply":
            unexpected = set(payload) - {"expected_revision", "selected"}
            if unexpected:
                raise RequestError(
                    "Deployment targets are configured at server startup, not by the browser"
                )
            return self.store.apply(expected, payload.get("selected"))
        if path == "/api/initialize":
            return self.store.initialize(expected, payload.get("price", 1200))
        raise MissingError("API endpoint not found")


