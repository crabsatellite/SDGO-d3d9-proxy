# Capsule list price probe (Experimental)

`capsule_price_probe` is a developer diagnostic, disabled by default. It observes
the eight capsule list price labels during the native page refresh. It logs a
bounded number of numeric label events to the normal framework log; it does not
change text, currency, SQL, resource files or account state.

The production price fix belongs to `capsule_catalog`: its compiler clears only
the eight known default price strings in `uirs/1161.uir`, forcing the native text
cache to build the first label. Do not enable the probe for normal play. See the
[probe contract](../mods/capsule_price_probe/README.md) for scope and symbols.
