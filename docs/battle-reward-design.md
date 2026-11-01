# Battle rewards: approved rules and native settlement

[中文完整规则](battle-reward-design.zh-CN.md)

The Beta runtime Mod, durable wallet worker and owner controls are integrated.
See [deployment and recovery](battle-rewards.md). The calculator remains a source
utility; authenticated gameplay and HUD acceptance remain manual.

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
effective damage, assists and, for PvP, an additional MVP award.
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

## Unified control surface

The reward preview, policy editor and worker controls are registered providers of
[SDGO Mod Manager](mod-manager.md). They share its local API, session and configuration
checks. The retired standalone reward HTTP server is not part of the package.

## Settlement integration contract

The trusted native server collector supplies outcomes, combat stats, MVP and
unique participating accounts. All recipients use one policy/event snapshot;
`preview_match` derives qualifying human count, rejects duplicate accounts and
checks aggregate Boss credit. The preview accepts owner-authored scenarios only;
HTTP input must never become an authoritative payout source.

The double-time component supplies a factor of one or two, consumed once. New
one-times policy values must replace, not additionally multiply, existing boosted
currency rewards. The native adapter and owner worker coordinate GB/MB delivery receipts,
duplicate settlement prevention, client display and verified packet field widths.
Experience, login gifts, compensation and purchases remain outside this currency
policy. Native integration is implemented; authenticated real match acceptance remains manual.
The reserved objective-event coefficients are zero because the current native adapter
does not supply that statistic; unverified Stars are never treated as objective counts.
