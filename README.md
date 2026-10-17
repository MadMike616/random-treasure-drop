# Random Treasure Drop

A D2RLoader server plugin that chooses one configured Treasure Class selector for each numbered group when a game starts. Ordinary selectors match one exact row; `actboss` and `superunique` are shorthand pools of rows associated with those monster categories, from which the plugin selects one row per group. D2R runs its normal drop routine unchanged. Separately, each selected group makes one weighted bonus-drop roll whenever its selected row is evaluated and, on success, creates one additional item at the source unit's ground coordinates. The plugin does not modify compiled Treasure Class rows or use the row's `Picks` value to repeat bonus rolls.

> **0.1.32.** Game-join logs report that a droptable was selected without printing its name or row. Successful bonus drops continue to be logged.

## Configuration

The config file is `random-treasure-drop.toml` in the plugin's D2RLoader config folder.

The shipped config contains one blank group. Fill in its empty item code and Treasure Class row name; copy the group and increment the numeric suffix to add more. The untouched blank group is ignored.

```toml
item1 = "www"
weight1 = 10
droptables_item1 = ["aaa", "bbb", "ccc"]

item2 = "type:xxx"
weight2 = 5
droptables_item2 = ["ddd", "eee", "fff"]
```

- `itemN` accepts a three- or four-character item code, or `type:xxx` for an item-type code. A type chooses a spawnable base item from that type or one of its equivalent child types.
- `weightN` is the extra item's relative drop weight in its selected row. It defaults to `1`; valid values are `1` through `1000000`.
- `droptables_itemN` is a list of exact row names from `treasureclassex.txt` or special selectors. One list entry is chosen uniformly at game join; an ordinary name resolves to one row, while a special selector is a pool from which one matching row is chosen uniformly.
- `actboss` matches the standard five act bosses in all difficulties. Add `:normal`, `:nightmare`, or `:hell` to limit it to one difficulty, for example `"actboss:normal"`. Rows whose names contain `Desecrated` are excluded.
- `superunique` matches the Treasure Class rows referenced by `superuniques.txt` across all difficulties. Add `:normal`, `:nightmare`, or `:hell` to restrict the rows to one difficulty, for example `"superunique:hell"`. Rows whose names contain `Desecrated` are excluded.
- `desecrated` matches every row in `treasureclassex.txt` whose Treasure Class name contains `Desecrated`. Add `:normal`, `:nightmare`, or `:hell` to choose one difficulty. Difficulty is identified by row-name markers: unmarked rows are Normal, `(N)` rows are Nightmare, and `(H)` rows are Hell.
- Add more numbered groups as needed. Every `itemN` must have a matching `droptables_itemN`.

At `GameJoined`, each group selects one selector uniformly from its list. If it is a special selector, the plugin then selects one matching source row uniformly from that selector's pool; an ordinary row name resolves directly to its single row. The selection is cleared at `GameLeft`. Game data can share a Treasure Class row between monster types, so a selected row may also match another unit that uses that row.

## Drop behavior

Each numbered group gets one independent bonus roll whenever its single selected row is evaluated, regardless of `Picks`. For each group, the success chance is `weightN / (source-row weight total + weightN)`. If several groups selected the same row, each gets its own roll and each may create an additional item. The native game drop and each bonus roll are separate calculations; a bonus success never replaces a native drop. The source TXT row's static `NoDrop` and `Prob1` through `Prob10` values set the baseline. D2R's runtime `/players` NoDrop adjustment is not included.

After the game's original drop routine finishes, each successful bonus roll resolves its packed item code to the compiled Items table's row index (the native class ID), then calls `ITEMS_CreateItemUnit` and `D2GAME_ReturnItemToGround` synchronously from the drop hook. It uses the source unit's dynamic-path coordinates only when both are positive and within range; otherwise, it uses the responsible player's location and room. It skips the bonus when no player or valid location exists. It does not modify the compiled Treasure Class table or the game's native drop result.

The created item has Normal quality and uses the level passed to the native drop routine, clamped to 1–99. Item-type codes choose a base item using the `rarity` weight from the active mod's `weapons.txt`, `armor.txt`, and `misc.txt` data. The active mod's plain-text Excel files must be accessible to the plugin.

## Building

Build on Windows with CMake 3.29 or newer and the Visual Studio C++ and Windows SDK components:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

CMake fetches the pinned D2RLoader PluginSDK revision. To build offline, set `RANDOM_TREASURE_DROP_LOCAL_SDK_DIR` to a local PluginSDK checkout when configuring.

## Compatibility

The native hook, path and room accessors, complete item-creation body (302 bytes), and complete ground-placement body (271 bytes) are gated to D2R build `3.3.93847` and require their captured byte signatures to match. Treasure Classes row size `0x150` is checked, and the plugin accepts a larger compiled runtime table only when it finds exactly one contiguous row range whose `Picks` values align with the configured source table. `Picks` is used only to align runtime rows, not to control the bonus roll. The game and loader must provide D2RLoader PluginSDK ABI 4 lifecycle and data-table services, including item lookup by code.

Runtime drop behavior was confirmed in GrailerRL with D2R `3.3.93847` and D2RLoader `1.3.1-beta` using PluginSDK ABI 4. Version 0.1.32's logging-only change was build-verified but was not separately play-tested.

Versions 0.1.8 through 0.1.10 triggered D2Common room assertions and a D2RLoader access violation while placing items through SDK automatic placement. Versions 0.1.11 through 0.1.19 were read-only diagnostics. Version 0.1.20 also crashed after SDK exact ground placement. Version 0.1.21 completed a chest drop without crashing, but its native creator received the packed item code instead of the required class ID and did not produce the bonus item. Version 0.1.22 resolved the class ID; its first reported crash followed a successful native helper call at `(4199, 0)`. Version 0.1.23 used the player's position when either source coordinate was zero and was later confirmed in-game to drop bonus items by monster corpses, including multiple kills. A 0.1.24 diagnostic that forced the coordinates to `(0,0)` logged a successful native placement and then crashed D2RLoader. Version 0.1.25 restores the 0.1.23 coordinate fallback and adds validated category selectors; version 0.1.26 adds difficulty-qualified superunique selectors and the plural alias; version 0.1.27 removes that alias; version 0.1.28 makes special selectors choose one row from their candidate pools; version 0.1.29 adds `desecrated` and excludes those rows from the actboss and superunique pools; version 0.1.30 adds desecrated difficulty filters; version 0.1.31 synchronizes the shipped default TOML with the current template; version 0.1.32 removes selected row names from game-join logs.
