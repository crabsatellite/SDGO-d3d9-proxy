"""Inspect, initialize, and CRUD capsule configuration without game/SQL writes."""
import argparse
import json
from pathlib import Path
from capsule_catalog import (acquisition, capsule_display, digest, export_bundle, get_machine,
                             import_catalog, initialize, mutate, patterns, read_json,
                             validate, write_json)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for name in ("inspect", "initialize"):
        p = sub.add_parser(name)
        p.add_argument("--snapshot", type=Path, required=True)
        p.add_argument("--client", type=Path, required=True)
        p.add_argument("--output", type=Path, required=True)
        if name == "initialize":
            p.add_argument(
                "--price", type=int, default=None,
                help=("override every newly initialized fixed-machine price; "
                      "default inherits each selected native template price"))
    p = sub.add_parser("catalog")
    p.add_argument("action", choices=["list", "show", "create", "update", "delete", "validate", "export"])
    p.add_argument("--file", type=Path, required=True)
    p.add_argument("--kind", choices=["fixed_machines", "custom_machines"], default="fixed_machines")
    p.add_argument("--id", type=int)
    p.add_argument("--payload", type=Path)
    p.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.command in ("inspect", "initialize"):
        path = args.output / "catalog.json"
        catalog = read_json(path) if path.exists() else import_catalog(args.snapshot, args.client)
        validate(catalog)
        _, current_identity = capsule_display(args.client)
        if any(catalog["source_identity"].get(k) != v for k, v in current_identity.items()):
            parser.error("Client data differs from the catalog source; use a new output directory")
        meta = args.snapshot / "_meta.json"
        if meta.exists() and "files" in read_json(meta):
            if catalog["source_identity"].get("snapshot_fingerprint") != digest(read_json(meta)["files"]):
                parser.error("Snapshot differs from the catalog source; use a new output directory")
        native_catalog = {**catalog, "fixed_machines": [m for m in catalog["fixed_machines"]
                                                       if m["owner"] == "native"]}
        write_json(args.output / "patterns.json", patterns(native_catalog))
        if args.command == "initialize":
            if args.price is not None and args.price < 0:
                parser.error("Price cannot be negative")
            catalog, report = initialize(args.snapshot, catalog, args.price)
            # An idempotent rerun keeps the original seed receipt.
            if report["seeds"] or not (args.output / "initialization-report.json").exists():
                write_json(args.output / "initialization-report.json", report)
            export_bundle(catalog, args.output / "bundle")
        else:
            report = acquisition(args.snapshot, catalog)
            write_json(args.output / "acquisition-report.json", report)
        write_json(path, catalog)
        print(json.dumps({"catalog": str(path), "fixed": len(catalog["fixed_machines"]),
                          "custom": len(catalog["custom_machines"]),
                          "new_roots": len(report.get("seeds", [])),
                          "runtime_installed": False}))
        return
    catalog = read_json(args.file)
    validate(catalog)
    if args.action == "list":
        print(json.dumps([{k: m[k] for k in ("id", "name", "enabled", "price")}
                          for m in catalog[args.kind]], ensure_ascii=False, indent=2))
    elif args.action == "show":
        item = get_machine(catalog, args.kind, args.id)
        if item is None:
            parser.error("Machine not found")
        print(json.dumps(item, ensure_ascii=False, indent=2))
    elif args.action == "export":
        if args.output is None:
            parser.error("Export requires --output")
        export_bundle(catalog, args.output)
    elif args.action != "validate":
        if args.id is None or (args.action != "delete" and args.payload is None):
            parser.error("Mutation requires --id and create/update requires --payload")
        value = read_json(args.payload) if args.payload else None
        result = mutate(catalog, args.kind, args.action, args.id, value)
        # Keep a content-addressed previous configuration, including native entries.
        import hashlib
        backup = args.file.with_name(args.file.name + "." + hashlib.sha256(args.file.read_bytes()).hexdigest()[:12] + ".bak")
        if not backup.exists():
            backup.write_bytes(args.file.read_bytes())
        write_json(args.file, result)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError) as error:
        raise SystemExit(str(error))
