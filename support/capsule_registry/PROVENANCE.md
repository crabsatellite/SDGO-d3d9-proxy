# Capsule registry support snapshot

The three native sources in this directory are an exact working-tree snapshot
copied from the repository's `investigate/capsule-machines` worktree, whose
checked-out base commit was `5838080`. That worktree had uncommitted capsule
runtime work, so the commit alone does not identify these bytes; the hashes
below are the authoritative provenance receipt. The sources implement the
shared `capsule_catalog` resource provider, so the collection feature does not
install a second competing `CZoaPack::OpenFile` hook.

| File | SHA-256 |
|---|---|
| `capsule_catalog.cpp` | `498b1a44f89fa16df153abd709beafa9e43f3df547b5165e81ea5ae7c7299cb0` |
| `capsule_runtime.cpp` | `59bc5afc16c98d89a92eeffdd8b7e574811867f3f8c48cbace7771661e252acf` |
| `capsule_runtime.h` | `95a3c76fb55126599ed46a47a2bf8a02785c9f1f2d8a69f654169ba8b425adb4` |

The source repository is MIT licensed. The project root `LICENSE` applies to
this snapshot. `Install-Client.ps1` is the matching standalone installer
implementation and is maintained here as packaging support, outside the three
source-hash assertions above.
