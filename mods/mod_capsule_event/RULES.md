# Capsule Event Rules

## Slot Order (UI Display)
- Non-R units first, sorted by grade descending: A, B, C, C, C
- R-rank units last, sorted by grade descending: AR, BR, CR
- Example 6-slot machine: A, B, C, C, C, CR
- Original reference (machine 78): Slot 0=A/AR, Slot 1=B/BR, Slot 2-4=C/CR, Slot 5=CR/BR/AR
- SQL ordering: `ORDER BY MachineID, IsR ASC, GN DESC`

## Grade Matching
- Replacements must strictly match: same UnitGradeNumber AND same R-rank status
- R-rank = UnitGradeNumber2 = 4 EXACTLY (not >= 4)
- U-rank (GN2=5) is NOT R-rank, displays normally (no "?" name)
- If BR, replace with BR. If CR, replace with CR. Strict.

## Grade System (UnitGradeNumber2 suffixes)
- GN2=1: Training units (rare, only 2 units) — NOT in capsules
- GN2=2: Normal (C, B, A, S — no suffix)
- GN2=3: S-variant (CS, BS, AS — "S" suffix) — NOT in capsules
- GN2=4: R-rank (CR, BR, AR, SR — "R" suffix) — shows "?" name + silhouette
- GN2=5: U-rank (CU, BU — "U" suffix) — normal display, NOT R-rank

## Excluded from Capsule Machines
- S-grade base (UnitGradeNumber = 4): never in capsules
- GN2=3 "S-suffix" variants (AS, BS, CS): never in capsules
- GN2=1 training units: never in capsules
- ONLY units in tbl_CapsuleEventPool (612 units with capsule SPRs) are eligible

## Unit Pool
- Pick ONLY from tbl_CapsuleEventPool (612 units)
- 341 original capsule SPRs (from GDT data, atlas-based)
- 271 generated capsule SPRs (composite: starfield bg + unit portrait crop + grade template)
- Generated files: sprs/30000-30541.spr + Txrs/30000-30541.txr in game client dir
- ZOA engine loads loose files from filesystem BEFORE ZPK archive (native override)
- Pool breakdown: C=90, CR=43, CU=30, B=118, BR=97, BU=4, A=111, AR=118, AU=1
- Generator: SDGO-tools/generate_capsule_sprs.py

## Draw Weight Distribution (per unit, 1000 slots total)
- C: 300, CR: 150, B: 80, BR: 30, A: 20, AR: 8
- Derived from original tbl_CapsuleList1 data analysis
- Preserves original per-machine draw probability distribution

## R-rank "?" Display
- Capsule GDT entry offset 0x13: 8 bytes, one per slot
- byte=0 -> show real unit name; byte!=0 -> show "?" + silhouette
- Only GN2=4 units (CR, BR, AR, SR) get the "?" treatment

## JSON Format (server -> client mod)
```json
{
  "machineId": {
    "units": [uid1, uid2, ...],
    "sprs": [["spr0.spr", "spr1.spr"], ...],
    "rrank": [0, 1, ...]
  },
  ...
}
```
- Per-machine dict, keyed by machine ID string
- Units ordered by: non-R grade DESC, then R grade DESC
- Served via HTTP on port 8099 by command_processor.py

## Event Schedule
- Daily 18:00-24:00: all capsule machines randomized
- Outside window: machines restored to original content
- Managed by command_processor.py on VPS
