# Activity capsule currency label

`capsule_currency_fix` corrects the known activity machine 9301 label from 99 GP
to 99 coins, matching the server's native currency type. It changes one byte in
the client's validated display copy, after native item loading. It does not
change prices, GDTs, SQL, charging, refunds or prize pools.

The hook requires the supported executable and named symbol. It checks vector
bounds, record size, page shape, a unique 9301 row, price 99 and the known currency
enum before changing anything. Unknown input is left untouched. See
[implementation checks](../mods/capsule_currency_fix/README.md).

This independent client fix can coexist with the optional capsule catalog.
Disable it using `[mods] capsule_currency_fix=0` in `sdgo/sdgo.ini`.
