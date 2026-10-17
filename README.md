# Random Treasure Drop

Random Treasure Drop is a D2RLoader plugin that creates independently rolled bonus items from configured Treasure Class rows. The game's normal drop routine runs unchanged; a successful plugin roll creates one additional item at the source unit's location when available.

## Install

1. Download `d2rl-random-treasure-drop.dll` and `random-treasure-drop.toml` from the [latest release](https://github.com/MadMike616/random-treasure-drop/releases/latest).
2. Place the DLL in D2RLoader's `plugins` folder and the TOML file in its `config` folder.
3. Edit the TOML with item codes and Treasure Class row names available in the game's data files.

The included TOML has one blank example group. It is ignored until configured. Copy its three settings and increment the number to add more groups.

## Configuration

Each numbered group has an item, an optional weight, and a non-empty list of Treasure Class rows or special selectors:

```toml
item1 = "abc"
weight1 = 100
droptables_item1 = ["Example Treasure Class"]

item2 = "type:swor"
weight2 = 10
droptables_item2 = ["actboss:normal", "Example Treasure Class"]
```

Replace the example item codes and row name with values available in the game's data. Every `itemN` needs a matching `droptables_itemN`.

- `itemN`: a spawnable three- or four-character item code, such as `"abc"`, or an item type prefixed with `type:`, such as `"type:swor"`. A type can match items in equivalent child types; the plugin chooses a matching base item using its `rarity` value.
- `weightN`: the extra item's weight in the bonus-drop calculation. It defaults to `1` and must be from `1` through `1000000`.
- `droptables_itemN`: a list of exact Treasure Class row names or special selectors. Row names are matched without regard to letter case. The plugin first chooses one list entry uniformly. If that entry is a special selector, it then chooses one matching row uniformly from that selector's pool.

The special selectors are:

| Selector | Rows included |
| --- | --- |
| `actboss` | Treasure Class rows referenced by the five act bosses. Rows containing `Desecrated` are excluded. |
| `superunique` | Treasure Class rows referenced by `superuniques.txt`. Rows containing `Desecrated` are excluded. |
| `desecrated` | Rows whose Treasure Class name contains `Desecrated`. |

Every special selector accepts `:normal`, `:nightmare`, or `:hell` to limit its pool to one difficulty. A selector without a difficulty suffix includes all difficulties. For example:

```toml
droptables_item1 = ["actboss:normal", "Act 1 Chest A"]
```

This gives each of the two list entries an equal chance of selection. If `actboss:normal` is selected, one row is then selected from the Normal act-boss pool.

## Drop behavior

When a character enters a game, the plugin selects one row for each configured item group. It clears those selections when the character leaves the game. Each time a selected row is evaluated, its group gets one independent bonus-drop roll; the row's `Picks` value does not repeat that roll. Groups that select the same row roll independently.

The bonus chance for a group is:

```text
weightN / (NoDrop + Prob1 + Prob2 + ... + Prob10 + weightN)
```

The plugin runs this calculation separately from the game's native drop calculation. A successful roll adds one item and never replaces or changes a native drop. For an item type, the plugin chooses among matching spawnable base items using their `rarity` weights.

The baseline uses the source row's static `NoDrop` and `Prob1` through `Prob10` values. D2R's runtime `/players` adjustment to `NoDrop` is not included in the bonus-drop calculation.

The bonus item is created at the source unit's ground location when its location data is valid. If that location cannot be read, the plugin tries the responsible player's location. The item uses Normal quality and the level supplied to the native drop routine, clamped to 1–99.

Logs report that a group selected a droptable and whether a bonus item was created. They do not identify the selected row.

## Compatibility

The native drop hook and supporting game functions are validated for D2R build `3.3.93847`. The plugin requires D2RLoader PluginSDK ABI 4, including its lifecycle and data-table services. It checks the expected game-code signatures and Treasure Class table layout, and leaves the hook uninstalled if validation fails.

## Build

Build on Windows with CMake 3.29 or newer and the Visual Studio C++ and Windows SDK components:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

The build fetches a pinned D2RLoader PluginSDK revision. To build offline, configure with `RANDOM_TREASURE_DROP_LOCAL_SDK_DIR` set to a local PluginSDK checkout.

## License

This project is distributed under the MIT License. See [LICENSE](LICENSE).

