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
transaction. Personal nicknames of other operators and unrelated modules are preserved.
Free entitlements are issued during normal operator login/page reads; product
registration exports stable item descriptors without store prices or listings.

Read [the detailed workflow and storefront handoff contract](operator-assistant.zh-CN.md).

Fresh full imports include five permanent free support officers for blueprints,
experience packs, cosmetics, upgrade kits and capsule coins. The historical
branch probabilities and quantity ranges are preserved; each native drop now
samples independently. Typed server references and matching client registrations
replace name searches. Blueprint aliases are deduplicated by recipe, invalid
outputs/materials are excluded. These five use fixed Unicode names and skip the
native naming prompt. Existing names are backed up before replacing them with
short ASCII identity tokens and a completed-name flag; the matching client Mod
renders their configured names without changing other operators' naming flow.
Existing holdings of the five defaults are backed up and upgraded in place to
permanent use; user-operator IDs, slots and equipped references stay.

Validation includes 22 Python tests, 4 DLL/native fixtures, actual UI operation,
and a fresh SQL fixture database using synthetic accounts. Live gameplay and
in-game presentation remain manual acceptance gates. The default pack also has
isolated SQL tests of permanent grants, legacy alias migration, typed pools and
all 289 current deduplicated blueprints through the native inventory procedure.
