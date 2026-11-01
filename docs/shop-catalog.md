# Owner shop catalog manager (Beta)

Double-click `SdgoShopManager.exe` to manage native shop offers in a Windows owner window.
Select the WorldServer folder and connect using its existing host INI. Add an existing item,
set its price, GB/MB currency and term, then save the draft. Use **Apply to shop** to publish it.
Search, editing, reversible listing/unlisting, retirement, and portable JSON import/export are built in.

The WorldServer module starts disabled until configured. It updates only its owned single-item offers
and keeps native listing, billing and delivery paths. Existing native offers, accounts, balances,
inventory and purchase history are preserved. IDs are stable; retirement hides offers without reuse.
The supported World build is `548A86E3-0010C000`. Renamed copies are supported by the standalone installer.

The Full framework package includes the module and helper, with the window under `shop/`.
Base fixes include only the independent label patch. Separate incremental packages are also available
through `tools/package_shop.py`. Configuration exports contain no credentials or local host paths.
The owner window uses Windows .NET Framework 4.x and needs no Python during normal use.

See the [complete Chinese guide](shop-catalog.zh-CN.md), [button-based manual checks](shop-testing.zh-CN.md),
and [verification scope](shop-validation.zh-CN.md). Native gameplay and actual delivery are user acceptance steps.
