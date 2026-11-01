# Unified Mod Manager

[简体中文](mod-manager.zh-CN.md)

Open **SDGO Mod Manager.exe** from the framework package. Client/server selection,
mod configuration, shop, operators, capsule machines, rewards, FRP and AI catalogs
share one local interface. Users do not install Python or launch per-feature UIs.

Daily management includes hosting servers, adding content and editing rules.
Automatic bug fixes appear in the archive. The developer view only expands internal
identifiers, low-level settings and diagnostics; it is not a server-owner permission gate.

Choose existing workspaces to preserve authored data. Configuration writes retain
unknown keys and comments, validate registered values and reject stale revisions.
Games continue using their normal launchers. Saving configuration does not prove
that a running game loaded it; follow the reported restart/runtime status.

Every feature can copy a credential-free AI configuration/debugging handoff or a
GitHub bug-report prompt. The repair path helps willing users set up a development
environment and submit a tested PR. PR text must disclose actual AI model names and
versions, their scope, human verification and remaining uncertainty. Use explicit
Unknown when the tool did not disclose a model, and None when no AI was used.

See the [full guide](mod-manager.zh-CN.md), [contribution guidance](contributing.zh-CN.md)
and [registration design](mod-manager-design.zh-CN.md).
