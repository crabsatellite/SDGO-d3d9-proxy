# Shop rental and Unicode labels

`shop_labels` is a client fix for native GOnline build `54BE1568-00C97000`.
It displays numeric rental terms and permanent unit status in hover, purchase and gift options,
and supports a host-generated UTF-16LE display profile. Price, currency and purchase data are unchanged.

The fix is included with base fixes and also has its own incremental package. Its installer preserves
other mods and existing framework configuration. Unicode overrides are used only when the native
option ID, category, price, currency and validity match. A custom caption cannot conceal a rental.

See the [Chinese installation guide](shop-labels.zh-CN.md) and [manual checks](shop-testing.zh-CN.md).
This is a shop option fix, not a global font/chat or product-title converter. Activity capsule currency
display is a separate feature. Visual gameplay acceptance and real purchases remain manual checks.
