# Activity capsule currency label

`capsule_currency_fix` corrects the known activity machine 9301 label from 99 GP
to 99 coins, matching the server's native currency type. After native item loading,
it corrects only the currency byte in both the validated UI copy and its exact
client-memory GridData record. The confirmation dialog independently reads that
registry record to choose its wallet and balance preview. Disk GDTs, overlays,
prices, SQL, wallet values, charging, refunds and prize pools remain unchanged.

The hook requires the supported executable and named symbol. It checks vector
bounds, record size, page shape, a unique 9301 row, price 99 and the known currency
enum before changing anything. Grid ID 145093 must resolve as type 0x3C through
the verified native API, and all 179 bytes must match the UI copy before either
write is allowed. Unknown or mismatched input is left untouched. See
[implementation checks](../mods/capsule_currency_fix/README.md).

This independent client fix can coexist with the optional capsule catalog.
Manage it in Mod Manager's **Fix archive**, then restart that client.

The November 1 screenshot report established that the user's primary client did
not contain this Mod, despite having the optional capsule registry. Its live server
configuration still says Type 2 / price 99 for machine 9301. A source build or
record-level test was therefore insufficient deployment evidence. Static tracing
also established that the old display-copy-only patch could not fix confirmation.
Version 1.0.1 adds the guarded client-memory registry correction,
an actual DLL detour replay for the current 94-record / 96-slot registry and
a separate script-free Manager import package. Existing registry resources are preserved.
Installation is recorded separately from process loading, per-page correction and
visual/manual purchase acceptance. See the [Chinese checks](capsule-currency-fix.zh-CN.md).
