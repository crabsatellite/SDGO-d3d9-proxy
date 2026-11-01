# Unit Support Info startup mismatch

The production BattleServer1 process displayed `Unit Support Info AddData Not Match` during startup.
The active channel table and the original native launcher's XML both enable only ports 9302-9307.
The historical server backups also contain no 9301 row. This is an intentionally unregistered host.

Read-only native PE/x86 inspection located the error string at VA `0x0078F994` and
its reference at `0x004506DF`. At `0x004506D3`, the handler compares the incoming
16-bit count at packet+2 to eight. It processes 23-byte records only when the
count matches. The original check remains intact.

The native SQL procedure `sp_GetOwnServerInfo` joins `tbl_ServerChannel` to
`tbl_ChannelInfo` by ServerID and filters the local listen port. Port 9301 returns
zero rows. `sp_GetDataUnitSupporterInfo 0` returns zero records, while every
ServerID used by the six configured ports returns the eight expected records.

The local repair closes only the unregistered BattleServer1 startup instance.
The private server repository's `start_all.ps1`, `sdgo_watchdog.ps1` and monitor
instructions are aligned with the existing XML and channel table. Registered
game services preserve their PID/start-time identity and listen ports; isolated
test copies remain outside this repair. No player data or native executable is
modified. Future deployment helpers must consult the configured list rather
than enumerate every BattleServer directory.
