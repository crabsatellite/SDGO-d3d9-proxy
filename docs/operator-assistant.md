# Operator assistant (preview)

The host UI configures operator names, native/uploaded portraits, weighted item
drops and either permanent free access for every player or item registration.
Store listing, pricing and sales remain the storefront module's responsibility.

Run `tools/Start-Assistant.ps1` from a full framework package, select the game
client and the host's existing `StoreServer.ini`, then use the browser UI. The
host needs Python, Pillow and pyodbc. Players receive only the client Mod/data
update and use their normal launcher; they need neither Python nor SQL access.

The `operator_catalog` Mod uses the existing loader/core ABI. The host adapter
keeps native procedure backups and detects concurrent changes before applying a
transaction. Existing personal nicknames and unrelated modules are preserved.
Free entitlements are issued during normal operator login/page reads; product
registration exports stable item descriptors without store prices or listings.

Read [the detailed workflow and storefront handoff contract](operator-assistant.zh-CN.md).

Validation includes 14 Python tests, 3 DLL/native fixtures, actual UI operation,
and a fresh SQL fixture database using synthetic accounts. Live gameplay and
in-game presentation remain manual acceptance gates; no production installation
or live-game acceptance is claimed.
