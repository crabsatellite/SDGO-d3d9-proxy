# FRP connectivity (Beta)

Optional networking tools, configured by the server owner. Use your normal game
launcher after installing the owner's client profile.
中文与完整命令：[FRP 联机分发指南](frp-distribution.zh-CN.md)

| Part | Host |
|---|---|
| `net_route` | Every participating client |
| `agent_udp_pool` | Agent servers |
| `agent_relay_log_fix` | Recommended core fix on Agent servers |

The owner supplies public endpoints and the matching UDP tunnels, generates the
owner/player configuration, and distributes only the client-side profile to
players. Service addresses and tunnel ports are not built into the mod.

The standalone `frp/` kit includes installers and the owner's profile generator.
The feature is disabled until configured. Runtime category `network` and feature
`frp-distribution` switches control the networking mods together. The core relay
log fix remains in `fixes` and is managed separately.
