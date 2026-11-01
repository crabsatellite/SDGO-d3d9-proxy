# Daily double battle rewards (Beta)

`daily_rewards` runs on BattleServer. Its enabled default is daily **18:00 inclusive
to 22:00 exclusive in the server's local time**. It doubles the normal final G-point,
pilot EXP/rank-star and unit EXP rewards. Clients need no additional Mod or launcher.
See the [Chinese setup tutorial](daily-rewards.zh-CN.md) for installation and switches.

Settings are `StartTime=18:00`, `EndTime=22:00`, `Trace=0` in the Mod's `config.ini`.
The existing manifest settings remain the fallback when that file is absent. A present
configuration is authoritative. Times use strict `HH:MM`; overnight windows are supported;
equal endpoints and invalid values reject the Mod before any hook is installed.
Restart the affected BattleServer after editing configuration. Daily transitions use the
actual settlement clock, not server-start time or the client's clock.

Native bonuses are calculated first. The Mod doubles each final reward once and adds the
difference to its event-bonus bucket, keeping base/other bonuses plus event rewards equal
to the same totals consumed by the native unit update, database request and client packet.
Native caps still apply; the 16-bit per-settlement words saturate at 65535. Energy, repair
costs, match statistics/rating, random drops, separate quests/achievements, purchases,
top-ups, transfers and GM grants are outside this battle-settlement feature.

## Native boundary

Supported host: `BattleServer-548A891D-0046C000`, SHA-256
`f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4`.
The shipped legacy 2011 MAP does not describe this 2014 binary and was not used for hooks.
Current-file Ghidra/REA pseudocode and raw x86 decoding establish the dispatcher at
`0x0047E4A0` and ten final-settlement branch joins. Every join is after native bonus
aggregation and before the unit EXP application and DB/client record construction.

The Mod uses the framework's appended `HookSite` API (core 1.1.4), with verified `site`
symbols and separate signatures that pin the six EBP-relative argument locations.
All ten sites/layouts must verify. Hooks are queued and a failed registration rolls
back the Mod as a whole. The site detours preserve GPRs, arithmetic/direction flags,
x87/SSE state, the native stack and Win32 last error, then jump into the original
trampoline. Clock/log callbacks receive a normal FP and direction-flag environment.
No raw EXE file is patched and no global native event rate is modified.

REA evidence for the current dispatcher: `ev_17e0f1164d256b782a9855b5f6869810c4811150c23688e211081329f6a35f68`.
The complete personal settlement pipeline is recorded as
`ev_ec1037dced2caa31196002a189c2cddbd6e5f3b09554a9d657afa810008eb981`;
the native event calculation as `ev_156609a6758b93b985404833e5328c26394defddf0f3709286a41feff5dd0f96`.
These are static decompiler observations, not original source or gameplay execution.

## Automated verification

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release --parallel
build/Release/daily_rewards_policy_test.exe
powershell -NoProfile -ExecutionPolicy Bypass -File tests/daily_rewards_runtime_test.ps1
python tools/verify_symbols.py D:/SDGO/Server/BattleServer1/BattleServer.exe
```

Policy checks include the 18:00/22:00 edges, minute precision, overnight schedules,
invalid settings, zero rewards and saturation. Real-core/Mod cases cover active,
inactive, saturated, disabled and invalid configurations, rejected site bytes/layouts
and partial-registration rollback. The fixture uses a separately captured native
frame contract and replays the actual first MOV instruction for each trampoline.
It checks native registers, FP/SIMD state, last error, guard bytes and payout breakdown.

The feature is **Beta**. Real account balances, client presentation, match completion
and reconnect persistence remain manual acceptance. No automatic test executes a game
or mutates a live account/database. Disable `[features] daily-rewards=0` or
`[mods] daily_rewards=0`; remove only this Mod directory with the server closed to
uninstall it. Other framework components remain installed.
