# Hook Inventory — Random Treasure Drop

## Plugin metadata

| Field | Value |
|---|---|
| Plugin name | Random Treasure Drop |
| Plugin ID | random-treasure-drop |
| Release version | 0.1.41 |
| Inventory updated | 2026-11-08 |
| Source revision | v0.1.41 release source |
| D2R build(s) audited | D2R 3.3.93847 is the documented native-hook target; not revalidated in-game for this inventory |
| D2RLoader SDK / Plugin ABI | PluginSDK 0.4.0, commit 6eb8f8b6192868214706bd6d528c5294f2f551b7 / Plugin ABI 4 |
| Audit status | Partial |

## Scope and method

Inspected src/plugin.cpp, src/native_drop_signatures.hpp, README.md, and CMakeLists.txt. The plugin installs one guarded native inline hook. Supporting native helper functions are validated and called directly; they are not hook sites.

## Hook sites

| Site ID | Type | Target | Address / identifier | Operation | Guard / validation | Source reference | Notes |
|---|---|---|---|---|---|---|---|
| H-01 | Inline | DropTreasureClass | RVA 0x4404F0 | InstallInlineHook intercepts Treasure Class drops and applies configured bonus-drop rules | Supported-build gate, executable-region check, expected hook bytes, and helper/layout signature checks | src/plugin.cpp — InstallDropHook | No hook-site changes from v0.1.40. Hook is not installed if the target build or required signatures do not match. |

## Known collision overlaps

| Other plugin | Shared target | Relationship | Evidence / notes |
|---|---|---|---|

None identified in the current inventory; this is not a compatibility guarantee.

## Audit notes

The README documents native hook and helper validation for D2R 3.3.93847. Source-only review; the runtime layout was not revalidated for this rerelease. No hook-site changes from v0.1.40.
