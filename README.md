# Random Treasure Drop

Random Treasure Drop is a D2RLoader plugin that creates independently rolled bonus items from configured Treasure Class rows. The game's normal drop routine runs unchanged; a successful plugin roll creates one additional item at the source unit's location when available.

## Install

1. Download `d2rl-random-treasure-drop.dll` and `random-treasure-drop.toml` from the [latest release](https://github.com/MadMike616/random-treasure-drop/releases/latest).
2. Place the DLL in D2RLoader's `plugins` folder and the TOML file in its `config` folder.
3. Edit the TOML with item codes and Treasure Class row names available in the game's data files.

The included TOML has one blank example group. It is ignored until configured. Copy its three settings and increment the number to add more groups.

## Configuration

Each numbered group has one or more item choices, an optional weight, and a non-empty list of Treasure Class rows or special selectors:

```toml
item1 = ["pk1", "pk2", "type:armo, unique"]
weight1 = 100
droptables_item1 = ["Example Treasure Class"]

item2 = ["type:swor, magic"]
weight2 = 10
droptables_item2 = ["actboss:normal", "Example Treasure Class"]
```

Replace the example item codes and row name with values available in the game's data. Every `itemN` needs a matching `droptables_itemN`. For compatibility, a single item can also use the older string form, such as `item1 = "pk1"`.

Use numbered groups from `1` through `10000`; the plugin supports up to 10000 groups.

`droptables_itemN` accepts either one quoted row name or special selector, or an array of them. Use an array when you want the plugin to choose among multiple configured entries.

- `itemN`: a non-empty array of item choices. Each choice is a three- or four-character item code present in the active mod's item tables, such as `"pk1"`, or an item type prefixed with `type:`, such as `"type:swor"`. The plugin does not filter item rows using the `spawnable` column. Add `, quality` inside the string to request a quality, for example `"type:armo, unique"`. Without a quality suffix, the item is normal. After a group's bonus-drop roll succeeds, the plugin chooses one array entry uniformly. If the chosen entry is a type, it can match items in equivalent child types and the plugin chooses a matching base item using its `rarity` value.
- Supported qualities are `lowquality` (also `inferior`), `normal`, `superior`, `magic`, `set`, `rare`, and `unique`.
- `weightN`: the extra item's weight in the bonus-drop calculation. It defaults to `1` and must be from `1` through `1000000`.
- `droptables_itemN`: one exact Treasure Class row name or special selector, or an array of them. Row names are matched without regard to letter case. For an array, the plugin first chooses one list entry uniformly. If that entry is a special selector, it then chooses one matching row uniformly from that selector's pool.

The special selectors are:

| Selector | Rows included |
| --- | --- |
| `actboss` | Treasure Class rows referenced by the five act bosses. Rows containing `Desecrated` are excluded. |
| `superunique` | Treasure Class rows referenced by `superuniques.txt`. Rows containing `Desecrated` are excluded. |
| `desecrated` | Rows whose Treasure Class name contains `Desecrated`. |
| `any` | Every row in `TreasureClassEx.txt`, selected uniformly. |

Every special selector accepts `:normal`, `:nightmare`, or `:hell` to limit its pool to one difficulty. For `any`, the difficulty is inferred from the Treasure Class row name: `(N)` rows are Nightmare, `(H)` rows are Hell, and rows without either suffix are Normal. A selector without a difficulty suffix includes all difficulties. For example:

```toml
droptables_item1 = ["actboss:normal", "Example Treasure Class"]
droptables_item2 = ["any:hell"]
```

For each group, every entry in its list has an equal chance of selection. If `actboss:normal` is selected, one row is then selected from the Normal act-boss pool; `any:hell` selects one Hell row uniformly from all Treasure Class rows.

## Drop behavior

When a character enters a game, the plugin selects one row for each configured item group. It clears those selections when the character leaves the game. Each distinct evaluation of a selected row gets one independent bonus-drop roll per matching group; the row's `Picks` value does not repeat that roll. Groups that select the same row roll independently. To avoid duplicate rewards when D2R repeats the same source callback, evaluations for the same source unit or exact source location and row within 250 milliseconds are treated as one event.

The bonus chance for a group is:

```text
weightN / (NoDrop + Prob1 + Prob2 + ... + Prob10 + weightN)
```

The plugin runs this calculation separately from the game's native drop calculation. A successful roll adds one item and never replaces or changes a native drop. If `itemN` contains multiple entries, one is chosen uniformly for each successful bonus roll. The selected entry's quality is passed to D2R when creating the item. For a selected item type, the plugin chooses among matching base items using their `rarity` weights.

The baseline uses the source row's static `NoDrop` and `Prob1` through `Prob10` values. D2R's runtime `/players` adjustment to `NoDrop` is not included in the bonus-drop calculation.

The bonus item is created at the source unit's ground location when its location data is valid. If that location cannot be read, the plugin tries the responsible player's location. Its quality defaults to Normal unless the selected item entry specifies another quality, and its level comes from the native drop routine, clamped to 1–99.

Logs report group selections, bonus-roll outcomes, and created drops. They do not identify the selected row.

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

