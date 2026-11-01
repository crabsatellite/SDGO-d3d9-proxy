# Battle reward result display

The contribution Mod originally replaced the final GB field and cleared its old
event bucket, while keeping the old table's base and three other GB bonus fields.
This made the wallet amount and result presentation disagree. It does not prove
the contents of a particular player's unrecorded result packet.

The supported native result record is `0x67` bytes. BattleServer writes final GB
at `+0x18`, the base display row at `+0x1E`, and old bonuses at `+0x24`, `+0x2A`,
`+0x30` and `+0x36`. These are separate fields, even when the original tables
normally make their sum equal the total.

In the exact supported client, the result event constructor at `0x004D0630`
copies these fields into event `0x427B`. The GB dialog's data reader at
`0x004E3D60` initializes its displayed number from the base row and separately
retains the total. The normal animation at `0x004EC8B0` adds the four display
bonuses. Skipping at `0x004EC640` uses the retained total. Correcting only total
GB therefore cannot correct the normal animation.

The repair publishes the complete policy GB as the native base row and clears
all four superseded GB bonuses at every settlement site. Both presentation paths
then agree with the existing wallet amount. Native labels do not have separate
rows for the contribution policy, so no false mapping of kills or MVP to an old
bonus category is introduced. XP fields and the separate native operator reward
field at packet `+0x59` are left intact. MB delivery and policy calculation are
unchanged.

## Local native verification

The optional probe requires explicitly supplied local game images. It checks
their complete SHA-256 before extracting any executable slice:

- GOnline: `e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`
- BattleServer: `f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4`

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release --target battle_reward_native_result_probe
build\Release\battle_reward_native_result_probe.exe "D:\SDGO\GOnline.exe" "D:\SDGO\BattleServer.exe"
```

The probe executes all ten original server GB-emitter slices and the original
client event constructor, bonus reader and GB data reader in an isolated test
process. The constructor/number-control/wallet-query dependencies are stubs;
neither game process starts and no database or network is used. A synthetic
base of `16` with total `22,000` demonstrates the old mismatch. The production
repair is checked at zero, mission-sized amounts, `10,655`, `22,000` and `65,535`,
along with preservation of every unrelated input-frame byte.

The regular real-core reward runtime fixture additionally checks the repaired
rows for all ten settlement entries, daily double coexistence and duplicate
settlement. The old deployed DLL must fail these additional assertions, while
the repaired DLL passes. This is packet/data-reader verification, not a rendered
HUD or authenticated gameplay recording. Game images are not bundled in source,
CI or distribution packages.
