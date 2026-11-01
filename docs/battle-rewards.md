# Battle contribution rewards (Beta)

[完整中文安装教程](battle-rewards.zh-CN.md) · [Approved rule tables](battle-reward-design.md)

This server Mod is wired to native final settlement, effective HP-loss statistics,
the original G delivery path and an idempotent native MB wallet worker. Owners can
change the base multiplier from the assistant; players retain their normal client
and launcher. Authenticated gameplay, HUD and reconnect acceptance remain manual.

## Owner installation

Use `rewards/` in the full framework package or the standalone owner kit. Python
3.10+, Windows PowerShell and the owner's existing SQL Server access are required:

```powershell
python -m pip install -r requirements-rewards.txt
python tools/install_battle_rewards.py --package-root . --battle-dir "D:/SDGO/Server/BattleServer1" --workspace "D:/SDGO-Owner/Rewards" --server-ini "D:/SDGO/Server/WorldServer/WorldServer.ini"
```

This first invocation is read-only. Close the selected BattleServer yourself,
review the plan, then repeat with `--apply`. The INI must contain both `DB Game`
and `DB StoreBilling`; secrets stay on the owner computer. Install each relevant
BattleServer with the same owner workspace.

The installer updates the bundled loader/core/Mod, retains other mods and settings,
merges symbol definitions, exports the owner's mission Boss identities and installs
only versioned reward-ledger objects. It does not edit native reward tables, replace
the native wallet procedure or grant test money. Receipts/backups describe changes
and failures. Runtime global category/feature/mod switches retain their authority.

## Start the owner control plane

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Start-Battle-Rewards.ps1 -Workspace "D:/SDGO-Owner/Rewards" -StoreServerIni "D:/SDGO/Server/WorldServer/WorldServer.ini"
```

The entry publishes policy, starts its wallet worker hidden and prints the local
assistant URL. Start the game server normally. Keep the owner entry running;
closing only the browser tab does not stop delivery. The panel reports connected
only after fresh matching native policy, hook/network readiness and wallet health.

The base multiplier reloads for later settlement snapshots without restarting the
BattleServer. A match uses one frozen policy/event/human-count snapshot. AI matches
use the PvP tier; extra humans compound `1.1^(humans-1)`. The loaded `daily_rewards`
component supplies its configured one/two event factor once. Pilot/unit experience
remains owned by that component. With no loaded daily component the factor is one.

The native G field is limited to 65,535. Under the default policy and worst-case
twelve humans/double time, the safe base ceiling is **10.440894**. Unsafe rules are
rejected with an unhealthy status, never clipped or wrapped. The panel exposes the
safe limit for the currently configured policy.

For a shared port, supply `-AssistantRoot` and `-AssistantWorkspace`, plus the
installed factory's workspace inputs (`-AssistantClient`, `-AssistantSnapshot`,
`-AssistantModuleBinary`, `-AssistantServerIni` or `-AssistantOption`). The existing
factory must export `Workspace` and `make_server(workspace, port=0)`, or the explicit
factory options can select alternatives. This starts a composed instance; it does
not patch or kill an already running assistant. An occupied port is rejected.

## Delivery and recovery

G is replaced at the verified native final amount and continues through the normal
World/wallet path. MB uses durable jobs and a database receipt keyed by match/account
and bound to the plan. A lost post-commit response can be retried without another
credit. The worker queries fresh current MB and sends the normal `0x064B` absolute
balance update through the existing Agent route. Recycled/offline sessions are not
notified; normal login/store cash queries refresh their authoritative balance.

Keep queued jobs and wallet receipts during recovery. Stop only the owned worker:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Stop-Battle-Rewards.ps1 -Workspace "D:/SDGO-Owner/Rewards"
```

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
