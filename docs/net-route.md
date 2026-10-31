# Configured network endpoint routes

`net_route` is an experimental client mod for the supported GOnline build. It
translates configured TCP/UDP destinations and restores the logical source of
UDP replies. It does not change packet payloads or provide a new battle relay.
Native relay integration and external multiplayer acceptance remain separate.

Use the [incremental distribution kit](frp-distribution.zh-CN.md) to update the
framework while preserving other mods and user settings. Owners supply their own endpoints; players
receive the public profile and use the existing launcher. No Sakura account,
Python, .NET, or special launch script is needed on players' computers.

Connection settings live in `sdgo/mods/net_route/config.ini`:

```ini
[settings]
LoginIPv4=192.0.2.10
LoginPort=15001
RouteCount=2
Trace=0
Route1=tcp,192.0.2.10,15001,your-node.example.invalid,25001
Route2=udp,192.0.2.10,10001,your-node.example.invalid,26017
```

`mod.ini` remains the developer manifest and enabled switch. If `config.ini` is
absent, legacy `[settings]` in the manifest still works. A present config is
authoritative, with no per-key mixing. Leave both login fields empty for legacy
route-only behavior. Otherwise the numeric login tuple must have an exact TCP
route. The verified native `CommandLineToArgvW` call sees this configured login;
all other argument parsing and trailing launcher arguments remain unchanged.
The existing launcher's own event/permission requirements still apply.

The host forwards TCP to its actual login listener and every UDP pool port to
its actual Agent socket. Add independent TCP rules only for observed client
connections. Server database and inter-service ports are not player routes.
Friends receive public connection settings, never the owner's FRP credentials.

Each `RouteN` has five comma-separated fields:

| field | meaning |
|---|---|
| `tcp` / `udp` | Lower-case transport name. TCP and UDP rules are distinct. |
| logical address | Exact numeric IPv4 address used by the game. No wildcard. |
| logical port | 1–65535, used by the game. |
| external host | IPv4 or DNS hostname, resolved to one IPv4 at startup. |
| external port | 1–65535, allocated to the Sakura tunnel. |

`RouteCount` is 1–32. Each logical key must be unique per transport.
UDP rules cannot resolve to the same external IP/port, because their replies
would be ambiguous. Several TCP logical aliases may share one external gateway.
The profile generator keeps `login` and `login.aliases` on their shared gateway;
optional top-level `tcp` entries each provide an independent external endpoint.
TCP logical keys are unique across all three groups, while different TCP rules
may still use the same physical endpoint. All TCP and UDP rules together must
fit the 32-route limit. Invalid configuration rejects the mod before hooks are
installed. Restart the client after changing configuration or DNS records.

`Trace=1` logs route definitions, the first eight outgoing/incoming operations per
route and the first 32 unmatched destinations. Payloads and login fields are not
logged. Unknown destinations keep the original behavior. `Trace=0` keeps startup
diagnostics without per-operation logging.

The mod owns the EXE's `connect`, `sendto` and `recvfrom` imports. Disable the
diagnostic `net_probe` for routing sessions, or leave it at its default disabled
setting. If another mod owns an import, the core rolls back the entire failed
`net_route` load. No partial translation is accepted. DLL callers and dynamically
looked-up APIs are outside this IAT hook's coverage.

The current transport preserves return values, flags, nonblocking behavior and
Winsock errors. UDP reply translation includes zero-length messages, `MSG_PEEK`
and the sender returned with a truncated `WSAEMSGSIZE` datagram. Local bind ports
are left to the game. The main observed network path uses these imports; actual
game session and native relay behavior still require runtime evidence.

For Sakura background behavior, the mod does not start, stop, reconfigure or
embed frpc. The host continues to use the official Sakura launcher/service.

For the host's bounded native Agent UDP pool and configuration generator, see
[the Sakura setup guide](sakura-frp-setup.zh-CN.md).
Human login, room creation and full gameplay remain manual acceptance gates;
the distribution guide distinguishes those from the automated transport checks.
