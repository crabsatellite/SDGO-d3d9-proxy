# Packet logger (development)

Logs the client's outgoing packets, for mapping the network protocol while developing mods.
Off by default. 中文：开发用的发包日志，默认关闭。

| mod | host | part |
|---|---|---|
| `net_probe` | client | the whole feature |

## Use

Set `enabled=1` in `sdgo\mods\net_probe\mod.ini` and start the game. The packets go to
`sdgo\logs\sdgo.log`. Turn it off again with `enabled=0` (or `net_probe=0` under `[mods]` in
`sdgo\sdgo.ini`).

## Settings

| key | default | meaning |
|---|---|---|
| `MaxPerOpcode` | 60 | packets logged per opcode |
| `DumpBytes` | 64 | bytes dumped per packet |

## How it works

Hooks the client's TCP send (`net.send_packet`) and UDP `sendto` and logs the first
`MaxPerOpcode` packets of every opcode with a hex dump of `DumpBytes` bytes.
