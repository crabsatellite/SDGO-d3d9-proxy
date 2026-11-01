# Capsule catalog editor (Beta)

Owners manage fixed and custom capsule machines through a local browser UI.
Create, inspect, edit, retire, restore and reorder machines; choose native six-slot
templates; set prices and weights; add or remove custom candidates; preview unit
portraits; export and compile configuration; apply matching client and server data.
Players keep the game's native capsule screens and normal launcher.

`capsule_catalog` ships disabled until the owner supplies a complete generated
runtime. No game assets, player data, database credentials or machine-specific
paths are checked into this repository. Owners provide their own supported client
and server configuration. The owner tools require Python 3.10+, Pillow and, for
SQL operations, pyodbc plus an installed SQL Server ODBC driver. Player installation
uses PowerShell and does not require Python.

See the [owner walkthrough](capsule-authoring.zh-CN.md) for commands, source
inspection, initialization and the portable editor package. The editor binds to
`127.0.0.1`; it is an owner tool rather than an account-facing remote website.

The configuration is shared by two consumers:

| Consumer | Output and behavior |
|---|---|
| Client runtime | Verified overlays for four capsule GDTs, card sprites/textures and the exact capsule list price UI resource |
| Server adapter | Configuration SQL and Mod-owned prize selection; native charging, delivery and refund routes remain in use |

The same `catalog_revision` must reach both sides. An exported JSON alone is not
an installation. Applying checks the target, creates backups, installs client
files and commits server configuration; failures restore client files and roll
back SQL. Existing Mods and player history are preserved. Close the target game
before installation. The tool does not restart unrelated servers.

Give players the complete generated `player` directory, including its manifest
and `Install-Client.cmd`. Check first, then install:

```powershell
.\Install-Client.cmd -GameDir "D:\Game" -CheckOnly
.\Install-Client.cmd -GameDir "D:\Game"
```

New fixed pools use independent weighted selection with six positive integer
weights totaling 1000. Existing unedited pools retain native sequence behavior.
Native templates carry both their grade order and price; an all-A machine is not
priced as a cheap mixed pool merely because it was newly generated.

Custom candidates preserve the source-bound native whitelist. Additional high-rank
candidates may be SR, or SS with no blueprint recipe record. Ordinary S and
recipe-bearing SS are excluded. The six base slots and two extension slots keep
their native selection constraints; adding candidates does not add SR slots.

Other Mods should use the [integration contract](capsule-integration.md). Source
and synthetic runtime tests do not establish account charging or prize delivery;
those checks are performed manually in the game. Recent accepted display repairs
are described in the [display notes](capsule-display-fix.zh-CN.md).
