# Hook Inventory — Random Treasure Drop

## Plugin metadata

| Field | Value |
|---|---|
| Plugin name | Random Treasure Drop |
| Plugin ID | random-treasure-drop |
| Release version | 0.1.42 |
| Inventory updated | 2026-11-09 |
| Source revision | v0.1.42 release source |
| D2R build(s) audited | D2R 3.3.93847; D2R 3.4.95032 confirmed working by user |
| D2RLoader SDK / Plugin ABI | PluginSDK 0.4.0, commit 6eb8f8b6192868214706bd6d528c5294f2f551b7 / Plugin ABI 4 |
| Audit status | Partial |

## Scope and method

Inspected src/plugin.cpp, src/native_drop_signatures.hpp, README.md, and CMakeLists.txt. The plugin installs one guarded native inline hook. Supporting native helper functions are validated and called directly; they are not hook sites. The user's D2R 3.4.95032 log reports the hook installed and a successful bonus drop.

## Hook sites

| Site ID | Type | Target | Address / identifier | Operation | Guard / validation | Source reference | Notes |
|---|---|---|---|---|---|---|---|
| H-01 | Inline | DropTreasureClass | RVA 0x4404F0 | InstallInlineHook intercepts Treasure Class drops and applies configured bonus-drop rules | Executable-region check, expected hook bytes, and helper/layout signature checks; no explicit game-version gate | src/plugin.cpp — InstallDropHook | No hook-site changes from v0.1.41. User confirmed operation on D2R 3.4.95032; runtime signatures and helper calls remain byte-validated. |

## Known collision overlaps

| Other plugin | Shared target | Relationship | Evidence / notes |
|---|---|---|---|

None identified in the current inventory; this is not a compatibility guarantee.

## Audit notes

The expected native signatures and helper RVAs were originally validated against D2R 3.3.93847. D2R 3.4.95032 runtime logs confirm the byte-validated hook installed and produced a bonus drop. Version 0.1.42 removes the explicit build-name gate; RVAs, signatures, and hook sites are unchanged from v0.1.41.

