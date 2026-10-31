# FRP Beta build and verification

Run these commands in a source checkout on Windows with Visual Studio C++ tools,
CMake and Python. No game, Sakura account, server, database or credentials are
required for the automated suites.

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release --parallel
python -m unittest discover -s tests -p "test_frp*.py" -v
powershell -NoProfile -ExecutionPolicy Bypass -File tests/loader_test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests/findwindow_fix_test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests/install_test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests/net_route_test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests/net_login_test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests/agent_udp_pool_test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tests/frp_install_test.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/package.ps1
```

The workflow in `.github/workflows/frp-tests.yml` runs the same suites. A workflow
definition is not evidence that a remote run passed; check its result on the PR.

The route tests cover TCP aliases, independent services, UDP reverse translation,
peek/empty/truncated packets, disabled mode, invalid rules and hook rollback.
The login fixture verifies the actual supplied argument string, including quoted,
empty and trailing-backslash arguments. The Agent fixture checks pool exhaustion,
reuse, call-site/type gates and separate configuration. Installer checks include
in-place profiles, UTF-16 symbols through the Windows INI API, checksums, unlisted
framework files, configuration preservation and uninstall scope.

`tools/package_frp.py --out <new-directory>` creates the standalone kit; the normal
framework package embeds the same kit in `frp/`. Packaging validates DLL versions,
architecture, archive contents and local documentation links.

For an optional local acceptance check, `tests/frp_package_audit.py --help` describes
installing into a minimal copy of a supplied client. It never executes the game or
writes to the reference client. Its reference core must predate the candidate kit
for the downgrade check. Tests and game files are not shipped in the Mod kit.

Real launcher behavior, account login, room creation, complete multiplayer matches
and reconnects remain manual acceptance. The network mods are in Beta and
disabled in the general framework package until the owner configures them through
the incremental installer. See [the distribution tutorial](frp-distribution.zh-CN.md).
