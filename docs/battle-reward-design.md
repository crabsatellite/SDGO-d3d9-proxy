# Battle rewards: approved rules and calculation preview

[中文完整规则](battle-reward-design.zh-CN.md)

This source-only design preview contains the approved initial policy, an exact
calculator and an assistant module. Native match settlement, wallet delivery and
mounting into the live assistant remain unconnected. Saving preview settings does
not change current game rewards. This is not an installable runtime mod.

## Policy

```text
final GB/MB = floor(
    one-times reward * base multiplier * event multiplier
    * 1.1^(qualifying human count - 1) * participation duration factor
)
```

Standard PvP with AI teammates/opponents uses the same reward tier as human-only
PvP. AI and observers do not increase the human multiplier or receive wallets.
The human multiplier compounds: 1, 1.1, 1.21, 1.331 for one through four humans.
A base multiplier of 10, double time and four humans therefore produce 26.62.

The one-times amount combines outcome/clear participation rewards with kills,
effective damage, assists, objectives and, for PvP, an additional MVP award.
Missions reward minions and proportional Boss contribution, rather than only the
last hit. Currency is rounded once after all factors using exact rational math.

The initial PvP defaults include 500 GB/2 MB for a win, 300 GB/1 MB for a loss,
60 GB/0.2 MB per kill, 15 GB/0.1 MB per 1,000 effective damage and an additional
200 GB/6 MB for MVP. Contribution caps and participation-time scaling apply.
Full amounts and limits are in `owner/rewards/policy.example.json` and the Chinese
rule guide; they remain configurable starting values.

For the same three-minute, four-human, base-ten and double-time conditions:

| Example | GB | MB |
|---|---:|---:|
| Normal mission: 15 minions, one Boss credit, 8,000 damage | 4,871 | 13 |
| PvP loss: two kills, 6,000 damage | 13,576 | 53 |
| PvP win: three kills, 8,000 damage | 21,296 | 90 |
| Same PvP win with MVP | 26,620 | 250 |

## Run the preview

Python 3.10+ from a source checkout; no SQL, game, account or server is required:

```powershell
python -m unittest discover -s tests -p "test_battle_reward_model.py" -v
python tools/reward_preview.py --scenario owner/rewards/examples/pvp-mvp.json
python tools/reward_preview.py --workspace build/reward-preview --port 0
```

The last command prints an available loopback URL ending in `/rewards`. The page
can preview payouts and persist a multiplier/rule revision in its own workspace.
Its status explicitly says game delivery is not connected. Changing that preview
does not write to native reward tables or wallets.

An assistant maintaining the existing `json_response` module-list convention can
mount `assistant_modules.rewards.attach(server, workspace)` on its current port.
It preserves other routes and requires its own session token, same-origin requests
and expected policy revision for configuration changes. Live entry-point mounting
is not part of this design-preview merge.

## Settlement integration contract

One trusted server collector must supply native outcomes, combat stats, MVP and
unique participating accounts. All recipients use one policy/event snapshot;
`preview_match` derives qualifying human count, rejects duplicate accounts and
checks aggregate Boss credit. The preview accepts owner-authored scenarios only;
HTTP input must never become an authoritative payout source.

The double-time component supplies a factor of one or two, consumed once. New
one-times policy values must replace, not additionally multiply, existing boosted
currency rewards. A future adapter must coordinate GB/MB transaction receipts,
duplicate settlement prevention, client display and verified packet field widths.
Experience, login gifts, compensation and purchases remain outside this currency
policy. Native integration and real match acceptance remain outstanding.
