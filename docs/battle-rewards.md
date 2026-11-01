# Battle contribution rewards (Beta)

[完整中文安装教程](battle-rewards.zh-CN.md) · [Approved rule tables](battle-reward-design.md)

This server Mod is wired to native final settlement, effective HP-loss statistics,
the original G delivery path and an idempotent native MB wallet worker. Owners can
change the base multiplier from the assistant; players retain their normal client
and launcher. Authenticated gameplay, HUD and reconnect acceptance remain manual.

## Unified owner interface

Open **SDGO Mod Manager.exe** and choose Battle rewards. Select the existing reward
workspace, native server INI, stopped BattleServer target and extracted framework/
reward kit. Review the installation plan before applying it. The installer keeps
configuration, symbols and recovery receipts, and does not issue test money to real accounts.

Start the Manager-owned wallet service in the same panel, then start the game server
normally. Edit multipliers, policy and previews there. Python, a separate reward page
and per-feature launch scripts are no longer user requirements. The Manager reports
real native/worker health; saving a file does not imply a live connection. See the
[Manager guide](mod-manager.md).

## Delivery and recovery

G is replaced at the verified native final amount and continues through the normal
World/wallet path. The native result animation adds its base and bonus rows, while
skipping uses the total directly. Both paths now receive the complete policy GB
in the base row, with superseded legacy GB bonuses cleared. This base row includes
contribution, MVP, multipliers and time scaling; it is not the old table's outcome
minimum. Pilot/unit EXP details and separately awarded system rewards are preserved.
MB uses durable jobs and a database receipt keyed by match/account
and bound to the plan. A lost post-commit response can be retried without another
credit. The worker queries fresh current MB and sends the normal `0x064B` absolute
balance update through the existing Agent route. Recycled/offline sessions are not
notified; normal login/store cash queries refresh their authoritative balance.

Keep queued jobs and wallet receipts during recovery. Stop only the owned worker:

Use Stop Manager-owned wallet service in the reward panel.

After closing the selected BattleServer, disable `battle_rewards=0` under `[mods]`
or plan a scoped uninstall with `tools/install_battle_rewards.py --battle-dir ...
--workspace ... --uninstall`; add `--apply` to archive only this Mod. The framework,
other mods, queue, configuration and wallet history are retained.

## Verification boundary

Automated checks cover exact arithmetic, all native frame families, real core/DLL
hooks, PvE Boss identity, policy freezing, the durable worker and native balance
notification. A copied native wallet procedure was tested in new isolated SQL
Databases. A copied real Agent forwarded the exact ten-byte cash packet from its
BATTLE connection to a synthetic client connection twice. Installation, owner
entry lifecycle and Windows file-sharing races have explicit regression tests.
A copied real BattleServer also starts with all eleven hooks owned by the current
reward DLL; its probe blocks outbound connections and does not start a real match.

From a Windows source checkout with the Visual Studio C++ tools installed:

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Default runtime checks use the real core/Mod DLLs, synthetic native frames and an
isolated ledger. Optional native SQL checks require a local server INI and scratch
database create/drop privileges; original account wallets are not credited.
The Agent and native-host probes also require locally owned game executables,
which are not distributed with the source or owner toolkit.

These are not authenticated real-client combat/HUD proof. See the Chinese tutorial
for manual match, event-window, multiplier, party-size and reconnect acceptance.
