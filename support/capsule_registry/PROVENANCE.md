# Capsule registry support provenance

The three native sources in this directory began as a working-tree snapshot
copied from the repository's `investigate/capsule-machines` worktree, whose
checked-out base commit was `5838080`. That worktree had uncommitted capsule
runtime work, so the commit alone does not identify the imported bytes. The
first hashes below identify that input.

| Imported file | SHA-256 |
|---|---|
| `capsule_catalog.cpp` | `498b1a44f89fa16df153abd709beafa9e43f3df547b5165e81ea5ae7c7299cb0` |
| `capsule_runtime.cpp` | `59bc5afc16c98d89a92eeffdd8b7e574811867f3f8c48cbace7771661e252acf` |
| `capsule_runtime.h` | `95a3c76fb55126599ed46a47a2bf8a02785c9f1f2d8a69f654169ba8b425adb4` |

The collection activity update extends that provider to compose `0031.gdt`
unit-item categories and `0099.gdt` activity machines through the same
`CZoaPack::OpenFile` hook. It adds activity-count readback but deliberately
does not count item-category records, so unrelated dynamic type-`0x22`
registrations remain valid.

The six-table provider ships as component version `1.1.0`. Its startup log
references the stable ASCII marker `SDGO_CAPSULE_REGISTRY_6TABLE_V2`, which
allows packagers to reject an older four-table DLL before publishing a
registry containing activity machines or unit-item categories.

| Current file | SHA-256 |
|---|---|
| `capsule_catalog.cpp` | `c7c1a08e7cd39aa4df5a194774fd8b84b6b6e0bd981dcbc7b8d95e0b5d0ea2f3` |
| `capsule_runtime.cpp` | `bf398278544b37f4089aebcab0bacb39da7287b4fdd4fa367b2cd720b6df12be` |
| `capsule_runtime.h` | `bf07903aae9dc88bc1df48c0b29eab273fc4361059fa648615fecbacdef6af58` |

The source repository is MIT licensed. The project root `LICENSE` applies to
these sources. `Install-Client.ps1` is the matching standalone installer
implementation and is maintained here as packaging support, outside the three
source-hash assertions above.
