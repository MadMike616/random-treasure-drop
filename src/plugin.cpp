#include <D2RLPlugin/api.h>

#include "default_config.hpp"
#include "native_drop_signatures.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr char PluginId[] = "random-treasure-drop";
constexpr char SupportedBuildToken[] = "93847";
constexpr std::uint64_t DropTreasureClassRva = 0x4404F0;
constexpr std::uint64_t GetDynamicPathRva = 0x34AE80;
constexpr std::uint64_t PathGetXRva = 0x341A20;
constexpr std::uint64_t PathGetYRva = 0x341A30;
constexpr std::uint64_t GetUnitRoomRva = 0x34B440;
constexpr std::uint64_t CreateItemUnitRva = 0x43D530;
constexpr std::uint64_t ReturnItemToGroundRva = 0x43DD00;
constexpr std::uint32_t CompiledTreasureClassTableId = 76;
constexpr std::uint32_t CompiledTreasureClassRowSize = 0x150;
constexpr std::uint32_t TreasureClassPicksOffset = 0x18;
constexpr std::uint32_t TreasureClassItemListOffset = 0x38;
constexpr std::uint32_t TreasureClassItemListCountOffset = 0x40;
constexpr std::uint32_t TreasureClassItemListCapacityOffset = 0x48;
constexpr std::uint32_t TreasureClassItemEntryDiagnosticSize = 32;
constexpr std::uint32_t TreasureClassMaxItems = 10;
constexpr std::uint32_t MaximumNativeProbeCalls = 32;
constexpr std::uint64_t DuplicateDropSuppressionWindowMs = 250;
constexpr std::size_t RecentDropEventCapacity = 128;
constexpr bool EnableBonusDrops = true;
constexpr bool EnableProbeLogging = false;
constexpr bool EnableDropOutcomeDiagnostics = true;
constexpr std::array<std::uint8_t, 16> HookExpectedBytes{
    0x4C, 0x8B, 0xDC, 0x55, 0x53, 0x49, 0x8D, 0xAB,
    0x08, 0xF7, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xE8,
};
constexpr std::array<std::uint8_t, 68> GetDynamicPathExpectedBytes{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0x48, 0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C,
    0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
    0xD4, 0xB9, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01,
    0xCC, 0x83, 0x3B, 0x05, 0x75, 0x14, 0x48, 0x8D,
    0x4C, 0x24, 0x30, 0xC6, 0x44, 0x24, 0x30, 0x00,
    0xE8, 0xEB, 0xA4, 0xFF, 0xFF, 0x84, 0xC0, 0x74,
    0x01, 0xCC, 0x48, 0x8B, 0x43, 0x38, 0x48, 0x83,
    0xC4, 0x20, 0x5B, 0xC3,
};
constexpr std::array<std::uint8_t, 16> PathGetXExpectedBytes{
    0x0F, 0xB7, 0x41, 0x02, 0xC3, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};
constexpr std::array<std::uint8_t, 16> PathGetYExpectedBytes{
    0x0F, 0xB7, 0x41, 0x06, 0xC3, 0xCC, 0xCC, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
};
constexpr std::array<std::uint8_t, 32> GetUnitRoomExpectedBytes{
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
    0xD9, 0x48, 0x85, 0xC9, 0x75, 0x13, 0x88, 0x4C,
    0x24, 0x30, 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8,
    0x54, 0xA7, 0xFF, 0xFF, 0x84, 0xC0, 0x74, 0x01,
};

using DropTreasureClassFn = void(__fastcall*)(
    void* game,
    void* monster,
    void* player,
    void* treasureClass,
    std::int32_t quality,
    std::int32_t level,
    std::int32_t arg7,
    void** items,
    std::int32_t* itemsDropped,
    std::int32_t maxItems);
using GetDynamicPathFn = void*(__fastcall*)(void*) noexcept;
using PathGetCoordinateFn = std::int32_t(__fastcall*)(void*) noexcept;
using GetUnitRoomFn = void*(__fastcall*)(void*) noexcept;
using CreateItemUnitFn = void*(__fastcall*)(
    void* player,
    std::uint32_t itemClassId,
    void* game,
    std::int32_t spawnTarget,
    std::int32_t quality,
    std::int32_t noSockets,
    std::int32_t noEthereal,
    std::int32_t itemLevel,
    std::int32_t seed1,
    std::int32_t seed2,
    std::int32_t seed3);
using ReturnItemToGroundFn = void(__fastcall*)(
    void* game,
    void* optionalPlayer,
    void* item,
    void* room,
    std::int32_t x,
    std::int32_t y);

enum class ItemQuality : std::int32_t {
    Low = 1,
    Normal = 2,
    Superior = 3,
    Magic = 4,
    Set = 5,
    Rare = 6,
    Unique = 7,
};

struct ItemCandidate {
    std::uint32_t packedCode{};
    std::uint32_t classId{std::numeric_limits<std::uint32_t>::max()};
    std::uint32_t rarityWeight{1};
    ItemQuality quality{ItemQuality::Normal};
};

struct DropTableSelector {
    std::string spec;
    std::vector<std::uint32_t> sourceRowIndices;
};

struct ItemOption {
    ItemQuality quality{ItemQuality::Normal};
    std::vector<ItemCandidate> candidates;
};

struct ItemSpec {
    std::string itemCode;
    ItemQuality quality{ItemQuality::Normal};
};

struct Rule {
    std::uint32_t number{};
    std::vector<ItemSpec> itemSpecs;
    std::uint32_t itemWeight{1};
    std::vector<DropTableSelector> dropTableSelectors;
    std::vector<ItemOption> itemOptions;
};

struct TreasureClassInfo {
    std::string name;
    std::int32_t picks{};
    std::uint64_t nativeWeight{};
};

struct TableText {
    std::vector<std::string> headers;
    std::vector<std::vector<std::string>> rows;
};

void __fastcall DropTreasureClassHook(
    void* game,
    void* monster,
    void* player,
    void* treasureClass,
    std::int32_t quality,
    std::int32_t level,
    std::int32_t arg7,
    void** items,
    std::int32_t* itemsDropped,
    std::int32_t maxItems);

struct BonusDrop {
    std::uint32_t itemCode{};
    std::uint32_t itemClassId{};
    std::uint32_t itemLevel{};
    ItemQuality quality{ItemQuality::Normal};
    std::uint32_t x{};
    std::uint32_t y{};
    void* room{};
};

struct RecentDropEvent {
    void* game{};
    void* sourceUnit{};
    void* room{};
    std::uint32_t rowIndex{};
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint64_t tick{};
};

enum class NativeDropResult : std::uint8_t {
    Success,
    InvalidContext,
    ItemCreationReturnedNull,
    ItemCreationException,
    GroundPlacementException,
};

auto NativeDropResultName(NativeDropResult result) noexcept -> const char* {
    switch (result) {
    case NativeDropResult::Success: return "success";
    case NativeDropResult::InvalidContext: return "invalid-context";
    case NativeDropResult::ItemCreationReturnedNull: return "create-null";
    case NativeDropResult::ItemCreationException: return "create-exception";
    case NativeDropResult::GroundPlacementException: return "placement-exception";
    }
    return "unknown";
}

constexpr D2RL::PluginInfo Info{
    .infoSize = D2RL::PluginInfoSize,
    .apiVersion = D2RL_PLUGIN_API_VERSION,
    .id = PluginId,
    .name = "Random Treasure Drop",
    .version = "0.1.40",
    .author = "MadMike",
    .description = "Adds independently rolled bonus drops at selected Treasure Class sources.",
    .flags = D2RL::PluginFlags::Server | D2RL::PluginFlags::NativeHooks,
};

const D2RL::PluginContext* Context{};
const D2RL::LifecycleServiceV1* LifecycleService{};
const D2RL::DataTableServiceV1* DataTableService{};
D2RL::Lifecycle::ListenerHandle DataTablesListenerHandle{};
D2RL::Lifecycle::ListenerHandle GameJoinedListenerHandle{};
D2RL::Lifecycle::ListenerHandle GameLeftListenerHandle{};
DropTreasureClassFn OriginalDropTreasureClass{};
GetDynamicPathFn GetDynamicPath{};
PathGetCoordinateFn PathGetX{};
PathGetCoordinateFn PathGetY{};
GetUnitRoomFn GetUnitRoom{};
CreateItemUnitFn CreateItemUnit{};
ReturnItemToGroundFn ReturnItemToGround{};

std::vector<Rule> Rules;
std::vector<TreasureClassInfo> TreasureClasses;
std::unordered_map<std::string, std::uint32_t> TreasureClassByName;
std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> TreasureClassByHash;
std::vector<std::pair<std::uint32_t, std::size_t>> ActiveRules;
std::mutex StateMutex;
std::mt19937_64 RandomEngine;
std::atomic<bool> TableReady{};
std::atomic<bool> GameActive{};
std::atomic<std::uint32_t> RuntimeTableRowCount{};
std::atomic<std::uint32_t> RuntimeSourceRowOffset{};
std::atomic<std::uint64_t> RuntimeTableRevision{};
std::atomic<std::uint32_t> ObservedDropHookCount{};
std::atomic<std::uint32_t> MappedDropHookCount{};
std::atomic<std::uint32_t> MatchedDropCallbackCount{};
std::atomic<std::uint32_t> SuppressedDuplicateDropCount{};
std::array<RecentDropEvent, RecentDropEventCapacity> RecentDropEvents{};
std::size_t RecentDropEventNext{};
std::size_t RecentDropEventCount{};

std::atomic<bool> NativeDropProbeLogged{};
std::atomic<std::uint32_t> NativeDropProbeCallCount{};

auto Trim(std::string_view value) -> std::string {
    std::size_t first{};
    while (first < value.size()
            && std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first
            && std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

auto Lower(std::string_view value) -> std::string {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return result;
}

auto NormalizeHeader(std::string_view value) -> std::string {
    std::string result;
    result.reserve(value.size());
    for (const char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch))) {
            result.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch))));
        }
    }
    return result;
}

auto Utf8Path(std::string_view value) -> std::filesystem::path {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char ch : value) {
        encoded.push_back(static_cast<char8_t>(
            static_cast<unsigned char>(ch)));
    }
    return std::filesystem::path(encoded);
}

auto StripComment(std::string_view line) -> std::string_view {
    bool quoted{};
    bool escaped{};
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char ch = line[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (quoted && ch == '\\') {
            escaped = true;
            continue;
        }
        if (ch == '"') {
            quoted = !quoted;
        } else if (!quoted && ch == '#') {
            return line.substr(0, index);
        }
    }
    return line;
}

auto ParseQuotedString(std::string_view text, std::string& value) -> bool {
    text = std::string_view(text.data(), text.size());
    while (!text.empty()
            && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    if (text.size() < 2 || text.front() != '"') return false;
    value.clear();
    bool escaped{};
    bool closed{};
    std::size_t index = 1;
    for (; index < text.size(); ++index) {
        const char ch = text[index];
        if (escaped) {
            if (ch != '\\' && ch != '"') return false;
            value.push_back(ch);
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
        } else if (ch == '"') {
            ++index;
            closed = true;
            break;
        } else {
            value.push_back(ch);
        }
    }
    if (escaped || !closed || index > text.size()) return false;
    return Trim(text.substr(index)).empty();
}

auto ParseStringArray(std::string_view text, std::vector<std::string>& values)
        -> bool {
    values.clear();
    text = std::string_view(text.data(), text.size());
    while (!text.empty()
            && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    if (text.empty() || text.front() != '[') return false;
    text.remove_prefix(1);
    while (true) {
        while (!text.empty()
                && std::isspace(static_cast<unsigned char>(text.front()))) {
            text.remove_prefix(1);
        }
        if (text.empty()) return false;
        if (text.front() == ']') {
            text.remove_prefix(1);
            return Trim(text).empty();
        }
        std::string item;
        std::size_t consumed{};
        bool quoted = text.front() == '"';
        if (!quoted) return false;
        bool escaped{};
        std::size_t index = 1;
        for (; index < text.size(); ++index) {
            const char ch = text[index];
            if (escaped) {
                if (ch != '\\' && ch != '"') return false;
                item.push_back(ch);
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                consumed = index + 1;
                break;
            } else {
                item.push_back(ch);
            }
        }
        if (escaped || consumed == 0) return false;
        values.push_back(std::move(item));
        text.remove_prefix(consumed);
        while (!text.empty()
                && std::isspace(static_cast<unsigned char>(text.front()))) {
            text.remove_prefix(1);
        }
        if (text.empty()) return false;
        if (text.front() == ',') {
            text.remove_prefix(1);
            continue;
        }
        if (text.front() == ']') {
            text.remove_prefix(1);
            return Trim(text).empty();
        }
        return false;
    }
}

auto ParseInteger(std::string_view text, std::int64_t& value) -> bool {
    const std::string trimmed = Trim(text);
    if (trimmed.empty()) return false;
    const char* first = trimmed.data();
    const char* last = first + trimmed.size();
    const auto result = std::from_chars(first, last, value);
    return result.ec == std::errc{} && result.ptr == last;
}

auto ParseNumberedKey(std::string_view key, std::string_view prefix,
        std::uint32_t& number) -> bool {
    if (!key.starts_with(prefix) || key.size() == prefix.size()) return false;
    const auto suffix = key.substr(prefix.size());
    std::uint32_t parsed{};
    const auto result = std::from_chars(
        suffix.data(), suffix.data() + suffix.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != suffix.data() + suffix.size()
            || parsed == 0 || parsed > 10000) {
        return false;
    }
    number = parsed;
    return true;
}

struct PartialRule {
    Rule rule;
    bool hasItem{};
    bool hasItemWeight{};
    bool hasRows{};
};

auto ValidCode(std::string_view code) noexcept -> bool {
    if (code.size() < 3 || code.size() > 4) return false;
    for (const char ch : code) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
            return false;
        }
    }
    return true;
}

auto ParseItemSpec(std::string_view text, ItemSpec& item,
        std::string& error) -> bool {
    const std::string value = Trim(text);
    const auto comma = value.find(',');
    item.itemCode = Trim(std::string_view(value).substr(0, comma));
    item.quality = ItemQuality::Normal;
    if (comma == std::string::npos) return true;
    if (value.find(',', comma + 1) != std::string::npos) {
        error = "item entries may contain only one comma between the item code and quality";
        return false;
    }

    const std::string quality = Lower(Trim(
        std::string_view(value).substr(comma + 1)));
    if (quality == "lowquality" || quality == "inferior") {
        item.quality = ItemQuality::Low;
    } else if (quality == "normal") {
        item.quality = ItemQuality::Normal;
    } else if (quality == "superior") {
        item.quality = ItemQuality::Superior;
    } else if (quality == "magic") {
        item.quality = ItemQuality::Magic;
    } else if (quality == "set") {
        item.quality = ItemQuality::Set;
    } else if (quality == "rare") {
        item.quality = ItemQuality::Rare;
    } else if (quality == "unique") {
        item.quality = ItemQuality::Unique;
    } else {
        error = "item quality must be lowquality, normal, superior, magic, set, rare, or unique";
        return false;
    }
    return true;
}

auto ParseConfig(std::string_view text, std::vector<Rule>& parsed,
        std::string& error) -> bool {
    try {
        std::map<std::uint32_t, PartialRule> groups;
        std::size_t lineNumber{};
        while (!text.empty()) {
            ++lineNumber;
            const auto lineEnd = text.find('\n');
            const auto line = text.substr(0,
                lineEnd == std::string_view::npos ? text.size() : lineEnd);
            if (lineEnd == std::string_view::npos) text = {};
            else text.remove_prefix(lineEnd + 1);
            const std::string content = Trim(StripComment(line));
            if (content.empty()) continue;
            const auto equals = content.find('=');
            if (equals == std::string::npos) {
                error = "line " + std::to_string(lineNumber)
                    + " is missing '='";
                return false;
            }
            const auto key = Trim(std::string_view(content).substr(0, equals));
            const auto value = std::string_view(content).substr(equals + 1);
            std::uint32_t number{};
            std::string parsedString;
            std::vector<std::string> parsedArray;
            std::int64_t parsedInteger{};
            if (ParseNumberedKey(key, "item", number)) {
                const std::string trimmedValue = Trim(value);
                if (!trimmedValue.empty() && trimmedValue.front() == '[') {
                    if (!ParseStringArray(value, parsedArray)
                            || parsedArray.empty()) {
                        error = key + " must be a non-empty array of quoted item codes or type selectors";
                        return false;
                    }
                } else {
                    if (!ParseQuotedString(value, parsedString)) {
                        error = key + " must be a quoted item code or a non-empty array of quoted item codes or type selectors";
                        return false;
                    }
                    parsedArray.push_back(std::move(parsedString));
                }
                auto& group = groups[number];
                if (group.hasItem) {
                    error = key + " is repeated";
                    return false;
                }
                group.hasItem = true;
                group.rule.number = number;
                for (auto& spec : parsedArray) {
                    ItemSpec item;
                    if (!ParseItemSpec(spec, item, error)) {
                        error = key + " " + error;
                        return false;
                    }
                    group.rule.itemSpecs.push_back(std::move(item));
                }
                continue;
            }
            if (ParseNumberedKey(key, "droptables_item", number)) {
                const std::string trimmedValue = Trim(value);
                if (!trimmedValue.empty() && trimmedValue.front() == '[') {
                    if (!ParseStringArray(value, parsedArray)
                            || parsedArray.empty()) {
                        error = key + " must be a quoted Treasure Class name or special selector, or a non-empty array of them";
                        return false;
                    }
                } else {
                    if (!ParseQuotedString(value, parsedString)) {
                        error = key + " must be a quoted Treasure Class name or special selector, or a non-empty array of them";
                        return false;
                    }
                    parsedArray.push_back(std::move(parsedString));
                }
                auto& group = groups[number];
                if (group.hasRows) {
                    error = key + " is repeated";
                    return false;
                }
                group.hasRows = true;
                group.rule.number = number;
                const bool blankPlaceholder = parsedArray.size() == 1
                    && Trim(parsedArray.front()).empty();
                for (auto& spec : parsedArray) {
                    spec = Trim(spec);
                    if (spec.empty() && !blankPlaceholder) {
                        error = key + " contains an empty selector";
                        return false;
                    }
                    group.rule.dropTableSelectors.push_back({
                        std::move(spec), {}});
                }
                continue;
            }
            if (ParseNumberedKey(key, "weight", number)) {
                if (!ParseInteger(value, parsedInteger) || parsedInteger <= 0
                        || parsedInteger > 1000000) {
                    error = key + " must be an integer from 1 through 1000000";
                    return false;
                }
                auto& group = groups[number];
                if (group.hasItemWeight) {
                    error = key + " is repeated";
                    return false;
                }
                group.hasItemWeight = true;
                group.rule.number = number;
                group.rule.itemWeight = static_cast<std::uint32_t>(parsedInteger);
                continue;
            }
            error = "unknown or malformed key '" + key + "' on line "
                + std::to_string(lineNumber);
            return false;
        }

        parsed.clear();
        parsed.reserve(groups.size());
        for (auto& [number, group] : groups) {
            if (!group.hasItem || !group.hasRows) {
                error = "group " + std::to_string(number)
                    + " needs both item" + std::to_string(number)
                    + " and droptables_item" + std::to_string(number);
                return false;
            }
            const bool blankRows = group.rule.dropTableSelectors.size() == 1
                && group.rule.dropTableSelectors.front().spec.empty();
            const bool blankItems = group.rule.itemSpecs.size() == 1
                && group.rule.itemSpecs.front().itemCode.empty();
            if (blankRows && blankItems) continue;
            if (group.rule.itemSpecs.empty()) {
                error = "item" + std::to_string(number)
                    + " must contain at least one item code or type selector";
                return false;
            }
            for (const auto& itemSpec : group.rule.itemSpecs) {
                std::string_view itemCode = itemSpec.itemCode;
                if (itemCode.starts_with("type:")) itemCode.remove_prefix(5);
                if (!ValidCode(itemCode)) {
                    error = "item" + std::to_string(number)
                        + " entries must be 3- or 4-character codes, optionally prefixed with type:";
                    return false;
                }
            }
            if (blankRows) continue;
            std::unordered_set<std::string> selectorSpecs;
            for (const auto& selector : group.rule.dropTableSelectors) {
                if (!selectorSpecs.insert(Lower(selector.spec)).second) {
                    error = "droptables_item" + std::to_string(number)
                        + " contains a repeated selector";
                    return false;
                }
            }
            parsed.push_back(std::move(group.rule));
        }
        return true;
    } catch (...) {
        error = "not enough memory while parsing the config";
        return false;
    }
}

auto ConfigPath() -> std::filesystem::path {
    if (Context != nullptr && Context->pluginConfigPath != nullptr
            && Context->pluginConfigPath[0] != L'\0') {
        return std::filesystem::path(Context->pluginConfigPath);
    }
    return {};
}

auto ReadConfigText(std::string& text) noexcept -> bool {
    try {
        const auto path = ConfigPath();
        if (path.empty()) {
            text = DefaultConfig;
            return true;
        }
        std::error_code error;
        if (!std::filesystem::exists(path, error) && !error) {
            std::filesystem::create_directories(path.parent_path(), error);
            if (error) return false;
            std::ofstream output(path, std::ios::binary);
            if (!output.is_open()) return false;
            output.write(DefaultConfig,
                static_cast<std::streamsize>(sizeof(DefaultConfig) - 1));
            if (!output) return false;
        }
        std::ifstream input(path, std::ios::binary);
        if (!input.is_open()) return false;
        text.assign(std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>());
        return !input.bad();
    } catch (...) {
        return false;
    }
}

auto DataRoots(const D2RL::PluginContext* context)
        -> std::vector<std::filesystem::path> {
    std::vector<std::filesystem::path> roots;
    std::unordered_set<std::wstring> seen;
    auto add = [&](const std::filesystem::path& path) {
        if (path.empty()) return;
        std::error_code error;
        const auto normalized = std::filesystem::weakly_canonical(path, error);
        const auto candidate = error ? path.lexically_normal() : normalized;
        const auto key = candidate.native();
        if (seen.insert(key).second) roots.push_back(candidate);
    };
    std::filesystem::path modDirectory;
    if (context != nullptr && context->modDirectory != nullptr) {
        modDirectory = context->modDirectory;
    }
    std::filesystem::path scopeRoot;
    if (context != nullptr && context->scopeRootDirectory != nullptr) {
        scopeRoot = context->scopeRootDirectory;
    }
    const std::string activeMod = context != nullptr
            && context->activeMod != nullptr
            ? context->activeMod
            : "";
    auto addModRoots = [&](const std::filesystem::path& base) {
        if (base.empty()) return;
        if (!activeMod.empty()) {
            const auto mod = Utf8Path(activeMod);
            add(base / mod / (mod.native() + std::filesystem::path(L".mpq").native()));
            add(base / (mod.native() + std::filesystem::path(L".mpq").native()));
            add(base / mod);
        }
        add(base);
    };
    addModRoots(modDirectory);
    if (!scopeRoot.empty() && !activeMod.empty()) {
        const auto mod = Utf8Path(activeMod);
        const auto package = mod.native() + std::filesystem::path(L".mpq").native();
        add(scopeRoot / L"mods" / mod / package);
    }
    addModRoots(scopeRoot);
    return roots;
}

auto FindExcelFile(const std::vector<std::filesystem::path>& roots,
        std::string_view fileName, bool preferBase)
        -> std::filesystem::path {
    const std::filesystem::path name = Utf8Path(fileName);
    for (const auto& root : roots) {
        std::array<std::filesystem::path, 2> candidates{};
        if (preferBase) {
            candidates = {root / L"data/global/excel/base" / name,
                root / L"data/global/excel" / name};
        } else {
            candidates = {root / L"data/global/excel" / name,
                root / L"data/global/excel/base" / name};
        }
        for (const auto& candidate : candidates) {
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error) && !error) {
                return candidate;
            }
        }
    }
    return {};
}

auto ReadTsv(const std::filesystem::path& path, TableText& table,
        std::string& error) -> bool {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        error = "could not open an active mod Excel file";
        return false;
    }
    std::string line;
    if (!std::getline(input, line)) {
        error = "an active mod Excel file is empty";
        return false;
    }
    if (line.size() >= 3
            && static_cast<unsigned char>(line[0]) == 0xEF
            && static_cast<unsigned char>(line[1]) == 0xBB
            && static_cast<unsigned char>(line[2]) == 0xBF) {
        line.erase(0, 3);
    }
    auto split = [](std::string_view row) {
        std::vector<std::string> cells;
        std::size_t begin{};
        while (true) {
            const auto end = row.find('\t', begin);
            cells.push_back(Trim(row.substr(begin,
                end == std::string_view::npos ? row.size() - begin : end - begin)));
            if (end == std::string_view::npos) break;
            begin = end + 1;
        }
        return cells;
    };
    table = {};
    table.headers = split(line);
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto cells = split(line);
        if (!cells.empty() && !cells[0].empty()) {
            table.rows.push_back(std::move(cells));
        }
    }
    if (input.bad() || table.headers.empty()) {
        error = "could not read an active mod Excel file";
        return false;
    }
    return true;
}

auto ColumnIndex(const TableText& table, std::string_view name) -> std::size_t {
    const auto normalized = NormalizeHeader(name);
    for (std::size_t index = 0; index < table.headers.size(); ++index) {
        if (NormalizeHeader(table.headers[index]) == normalized) return index;
    }
    return std::numeric_limits<std::size_t>::max();
}

auto Cell(const std::vector<std::string>& row, std::size_t index)
        -> std::string_view {
    if (index == std::numeric_limits<std::size_t>::max()
            || index >= row.size()) {
        return {};
    }
    return row[index];
}

auto ParseSignedCell(std::string_view cell, std::int64_t fallback)
        -> std::int64_t {
    if (Trim(cell).empty()) return fallback;
    std::int64_t value{};
    return ParseInteger(cell, value) ? value : fallback;
}

auto RowHash(const void* row) noexcept -> std::uint64_t {
    constexpr auto offset = TreasureClassPicksOffset;
    constexpr auto end = offset + sizeof(std::int32_t);
    const auto* bytes = static_cast<const std::uint8_t*>(row);
    std::uint64_t hash = 1469598103934665603ULL;
    for (std::uint32_t index = 0; index < CompiledTreasureClassRowSize; ++index) {
        if (index >= offset && index < end) continue;
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
    }
    return hash;
}

auto RowsEqualExceptPicks(const void* left, const void* right) noexcept -> bool {
    constexpr auto offset = TreasureClassPicksOffset;
    constexpr auto end = offset + sizeof(std::int32_t);
    const auto* first = static_cast<const std::uint8_t*>(left);
    const auto* second = static_cast<const std::uint8_t*>(right);
    return std::memcmp(first, second, offset) == 0
        && std::memcmp(first + end, second + end,
            CompiledTreasureClassRowSize - end) == 0;
}

auto ParseTreasureClasses(const std::vector<std::filesystem::path>& roots,
        std::vector<TreasureClassInfo>& rows,
        std::unordered_map<std::string, std::uint32_t>& byName,
        std::string& error) -> bool {
    const auto path = FindExcelFile(roots, "treasureclassex.txt", false);
    if (path.empty()) {
        error = "treasureclassex.txt was not found in the active mod files";
        return false;
    }
    TableText table;
    if (!ReadTsv(path, table, error)) return false;
    const auto nameColumn = ColumnIndex(table, "Treasure Class");
    const auto picksColumn = ColumnIndex(table, "Picks");
    const auto noDropColumn = ColumnIndex(table, "NoDrop");
    if (nameColumn == std::numeric_limits<std::size_t>::max()
            || picksColumn == std::numeric_limits<std::size_t>::max()
            || noDropColumn == std::numeric_limits<std::size_t>::max()) {
        error = "treasureclassex.txt is missing Treasure Class, Picks, or NoDrop columns";
        return false;
    }
    std::array<std::size_t, 10> probabilityColumns{};
    for (std::size_t index = 0; index < probabilityColumns.size(); ++index) {
        probabilityColumns[index] = ColumnIndex(table,
            "Prob" + std::to_string(index + 1));
        if (probabilityColumns[index] == std::numeric_limits<std::size_t>::max()) {
            error = "treasureclassex.txt is missing a Prob column";
            return false;
        }
    }

    rows.clear();
    byName.clear();
    rows.reserve(table.rows.size());
    for (const auto& sourceRow : table.rows) {
        const auto name = Trim(Cell(sourceRow, nameColumn));
        if (name.empty()) continue;
        const auto picks = ParseSignedCell(Cell(sourceRow, picksColumn), 0);
        const auto noDrop = ParseSignedCell(Cell(sourceRow, noDropColumn), 0);
        if (picks < std::numeric_limits<std::int32_t>::min()
                || picks > std::numeric_limits<std::int32_t>::max()
                || noDrop < 0) {
            error = "treasureclassex.txt has an invalid Picks or NoDrop value in row " + name;
            return false;
        }
        std::uint64_t nativeWeight = static_cast<std::uint64_t>(noDrop);
        for (const auto column : probabilityColumns) {
            const auto probability = ParseSignedCell(Cell(sourceRow, column), 0);
            if (probability < 0) {
                error = "treasureclassex.txt has a negative probability in row " + name;
                return false;
            }
            nativeWeight += static_cast<std::uint64_t>(probability);
        }
        const auto normalized = Lower(name);
        if (byName.contains(normalized)) {
            error = "treasureclassex.txt repeats row name " + name;
            return false;
        }
        const auto rowIndex = static_cast<std::uint32_t>(rows.size());
        byName.emplace(normalized, rowIndex);
        rows.push_back({name, static_cast<std::int32_t>(picks), nativeWeight});
    }
    return !rows.empty();
}

auto AddSelectorRow(std::string_view rowName,
        const std::unordered_map<std::string, std::uint32_t>& rowsByName,
        std::set<std::uint32_t>& rowIndices,
        std::string_view selector, std::string& error,
        bool excludeDesecrated = false) -> bool {
    const std::string name = Trim(rowName);
    if (name.empty()) return true;
    if (excludeDesecrated
            && Lower(name).find("desecrated") != std::string::npos) {
        return true;
    }
    const auto found = rowsByName.find(Lower(name));
    if (found == rowsByName.end()) {
        error = "droptable selector '" + std::string(selector)
            + "' refers to missing Treasure Class row '" + name + "'";
        return false;
    }
    rowIndices.insert(found->second);
    return true;
}

auto CollectActBossRows(const std::vector<std::filesystem::path>& roots,
        std::string_view difficultySuffix,
        const std::unordered_map<std::string, std::uint32_t>& rowsByName,
        std::set<std::uint32_t>& rowIndices, std::string& error) -> bool {
    const auto path = FindExcelFile(roots, "monstats.txt", false);
    if (path.empty()) {
        error = "monstats.txt was not found for the actboss selector";
        return false;
    }
    TableText table;
    if (!ReadTsv(path, table, error)) return false;
    const auto idColumn = ColumnIndex(table, "Id");
    if (idColumn == std::numeric_limits<std::size_t>::max()) {
        error = "monstats.txt is missing the Id column for the actboss selector";
        return false;
    }

    constexpr std::array<std::string_view, 5> ActBossIds{
        "andariel", "duriel", "mephisto", "diablo", "baalcrab",
    };
    constexpr std::array<std::string_view, 5> TreasureClassColumns{
        "TreasureClass",
        "TreasureClassQuest",
        "TreasureClassChamp",
        "TreasureClassUnique",
        "TreasureClassHerald",
    };

    std::array<std::size_t, TreasureClassColumns.size()> columns{};
    bool hasMainColumn{};
    for (std::size_t index = 0; index < TreasureClassColumns.size(); ++index) {
        std::string columnName(TreasureClassColumns[index]);
        columnName.append(difficultySuffix);
        columns[index] = ColumnIndex(table, columnName);
        if (index == 0) {
            hasMainColumn = columns[index]
                != std::numeric_limits<std::size_t>::max();
        }
    }
    if (!hasMainColumn) {
        error = "monstats.txt is missing the act boss TreasureClass column for the selected difficulty";
        return false;
    }

    for (const auto bossId : ActBossIds) {
        const auto boss = std::find_if(table.rows.begin(), table.rows.end(),
            [&](const auto& row) {
                return Lower(Cell(row, idColumn)) == bossId;
            });
        if (boss == table.rows.end()) {
            error = "monstats.txt is missing act boss '"
                + std::string(bossId) + "'";
            return false;
        }
        for (const auto column : columns) {
            if (column == std::numeric_limits<std::size_t>::max()) continue;
            if (!AddSelectorRow(Cell(*boss, column), rowsByName,
                    rowIndices, "actboss", error, true)) {
                return false;
            }
        }
    }
    if (rowIndices.empty()) {
        error = "the actboss selector did not resolve to any Treasure Class rows";
        return false;
    }
    return true;
}

auto CollectSuperUniqueRows(const std::vector<std::filesystem::path>& roots,
        std::string_view difficultySuffix,
        const std::unordered_map<std::string, std::uint32_t>& rowsByName,
        std::set<std::uint32_t>& rowIndices, std::string& error) -> bool {
    const auto path = FindExcelFile(roots, "superuniques.txt", false);
    if (path.empty()) {
        error = "superuniques.txt was not found for the superunique selector";
        return false;
    }
    TableText table;
    if (!ReadTsv(path, table, error)) return false;
    std::string baseColumnName("TC");
    baseColumnName.append(difficultySuffix);
    const auto column = ColumnIndex(table, baseColumnName);
    if (column == std::numeric_limits<std::size_t>::max()) {
        error = "superuniques.txt is missing the TC column";
        return false;
    }
    for (const auto& row : table.rows) {
        if (!AddSelectorRow(Cell(row, column), rowsByName,
                rowIndices, "superunique", error, true)) {
            return false;
        }
    }
    if (rowIndices.empty()) {
        error = "the superunique selector did not resolve to any Treasure Class rows";
        return false;
    }
    return true;
}

auto ResolveDropTableSelectors(std::vector<Rule>& rules,
        const std::vector<std::filesystem::path>& roots,
        const std::vector<TreasureClassInfo>& treasureClasses,
        const std::unordered_map<std::string, std::uint32_t>& rowsByName,
        std::string& error) -> bool {
    bool needsActBoss{};
    bool needsSuperUnique{};
    bool needsDesecrated{};
    bool needsAny{};
    for (const auto& rule : rules) {
        for (const auto& selector : rule.dropTableSelectors) {
            const auto spec = Lower(selector.spec);
            if (spec == "actboss" || spec == "actboss:normal"
                    || spec == "actboss:nightmare" || spec == "actboss:hell") {
                needsActBoss = true;
            } else if (spec.starts_with("actboss:")) {
                error = "unsupported actboss difficulty in selector '"
                    + selector.spec + "'; use normal, nightmare, or hell";
                return false;
            } else if (spec == "superunique") {
                needsSuperUnique = true;
            } else if (spec.starts_with("superunique:")) {
                const auto colon = spec.find(':');
                const auto difficulty =
                    std::string_view(spec).substr(colon + 1);
                if (difficulty != "normal" && difficulty != "nightmare"
                        && difficulty != "hell") {
                    error = "unsupported superunique difficulty in selector '"
                        + selector.spec + "'; use normal, nightmare, or hell";
                    return false;
                }
                needsSuperUnique = true;
            } else if (spec == "desecrated") {
                needsDesecrated = true;
            } else if (spec.starts_with("desecrated:")) {
                const auto colon = spec.find(':');
                const auto difficulty =
                    std::string_view(spec).substr(colon + 1);
                if (difficulty != "normal" && difficulty != "nightmare"
                        && difficulty != "hell") {
                    error = "unsupported desecrated difficulty in selector '"
                        + selector.spec
                        + "'; use normal, nightmare, or hell";
                    return false;
                }
                needsDesecrated = true;
            } else if (spec == "any") {
                needsAny = true;
            } else if (spec.starts_with("any:")) {
                const auto colon = spec.find(':');
                const auto difficulty =
                    std::string_view(spec).substr(colon + 1);
                if (difficulty != "normal" && difficulty != "nightmare"
                        && difficulty != "hell") {
                    error = "unsupported any difficulty in selector '"
                        + selector.spec
                        + "'; use normal, nightmare, or hell";
                    return false;
                }
                needsAny = true;
            }
        }
    }

    std::unordered_map<std::string, std::vector<std::uint32_t>> specialRows;
    if (needsActBoss) {
        constexpr std::array<std::pair<std::string_view, std::string_view>, 3>
            Difficulties{{
                {"normal", ""}, {"nightmare", "(N)"}, {"hell", "(H)"},
            }};
        std::set<std::uint32_t> allRows;
        for (const auto& [difficulty, suffix] : Difficulties) {
            std::set<std::uint32_t> difficultyRows;
            if (!CollectActBossRows(roots, suffix, rowsByName,
                    difficultyRows, error)) {
                error = "actboss:" + std::string(difficulty) + ": " + error;
                return false;
            }
            specialRows.emplace("actboss:" + std::string(difficulty),
                std::vector<std::uint32_t>(difficultyRows.begin(),
                    difficultyRows.end()));
            allRows.insert(difficultyRows.begin(), difficultyRows.end());
        }
        specialRows.emplace("actboss",
            std::vector<std::uint32_t>(allRows.begin(), allRows.end()));
    }
    if (needsSuperUnique) {
        constexpr std::array<std::pair<std::string_view, std::string_view>, 3>
            Difficulties{{
                {"normal", ""}, {"nightmare", "(N)"}, {"hell", "(H)"},
            }};
        std::set<std::uint32_t> allRows;
        for (const auto& [difficulty, suffix] : Difficulties) {
            std::set<std::uint32_t> difficultyRows;
            if (!CollectSuperUniqueRows(roots, suffix, rowsByName,
                    difficultyRows, error)) {
                error = "superunique:" + std::string(difficulty) + ": "
                    + error;
                return false;
            }
            const std::vector<std::uint32_t> rows(
                difficultyRows.begin(), difficultyRows.end());
            specialRows.emplace("superunique:" + std::string(difficulty), rows);
            allRows.insert(difficultyRows.begin(), difficultyRows.end());
        }
        const std::vector<std::uint32_t> rows(
            allRows.begin(), allRows.end());
        specialRows.emplace("superunique", rows);
    }
    if (needsDesecrated) {
        constexpr std::array<std::string_view, 3> Difficulties{
            "normal", "nightmare", "hell",
        };
        std::set<std::uint32_t> allRows;
        std::array<std::set<std::uint32_t>, Difficulties.size()> rowsByDifficulty;
        for (std::size_t index = 0; index < treasureClasses.size(); ++index) {
            const auto name = Lower(treasureClasses[index].name);
            if (name.find("desecrated") == std::string::npos) continue;
            const auto rowIndex = static_cast<std::uint32_t>(index);
            allRows.insert(rowIndex);
            const bool isNightmare = name.find("(n)") != std::string::npos;
            const bool isHell = name.find("(h)") != std::string::npos;
            const std::size_t difficultyIndex = isHell ? 2
                : (isNightmare ? 1 : 0);
            rowsByDifficulty[difficultyIndex].insert(rowIndex);
        }
        if (allRows.empty()) {
            error = "the desecrated selector did not find any Treasure Class rows containing 'Desecrated'";
            return false;
        }
        specialRows.emplace("desecrated",
            std::vector<std::uint32_t>(allRows.begin(), allRows.end()));
        for (std::size_t difficultyIndex = 0;
                difficultyIndex < Difficulties.size(); ++difficultyIndex) {
            const auto& rows = rowsByDifficulty[difficultyIndex];
            if (rows.empty()) {
                error = "the desecrated:" + std::string(
                    Difficulties[difficultyIndex])
                    + " selector did not find any matching Treasure Class rows";
                return false;
            }
            specialRows.emplace("desecrated:"
                    + std::string(Difficulties[difficultyIndex]),
                std::vector<std::uint32_t>(rows.begin(), rows.end()));
        }
    }
    if (needsAny) {
        constexpr std::array<std::string_view, 3> Difficulties{
            "normal", "nightmare", "hell",
        };
        std::set<std::uint32_t> allRows;
        std::array<std::set<std::uint32_t>, Difficulties.size()>
            rowsByDifficulty;
        for (std::size_t index = 0; index < treasureClasses.size(); ++index) {
            const auto name = Lower(treasureClasses[index].name);
            const auto rowIndex = static_cast<std::uint32_t>(index);
            allRows.insert(rowIndex);
            const bool isNightmare = name.find("(n)") != std::string::npos;
            const bool isHell = name.find("(h)") != std::string::npos;
            const std::size_t difficultyIndex = isHell ? 2
                : (isNightmare ? 1 : 0);
            rowsByDifficulty[difficultyIndex].insert(rowIndex);
        }
        if (allRows.empty()) {
            error = "the any selector did not find any Treasure Class rows";
            return false;
        }
        specialRows.emplace("any",
            std::vector<std::uint32_t>(allRows.begin(), allRows.end()));
        for (std::size_t difficultyIndex = 0;
                difficultyIndex < Difficulties.size(); ++difficultyIndex) {
            const auto& rows = rowsByDifficulty[difficultyIndex];
            if (rows.empty()) {
                error = "the any:" + std::string(
                    Difficulties[difficultyIndex])
                    + " selector did not find any matching Treasure Class rows";
                return false;
            }
            specialRows.emplace("any:"
                    + std::string(Difficulties[difficultyIndex]),
                std::vector<std::uint32_t>(rows.begin(), rows.end()));
        }
    }

    for (auto& rule : rules) {
        for (auto& selector : rule.dropTableSelectors) {
            const auto spec = Lower(selector.spec);
            const auto special = specialRows.find(spec);
            if (special != specialRows.end()) {
                selector.sourceRowIndices = special->second;
                continue;
            }
            const auto row = rowsByName.find(spec);
            if (row == rowsByName.end()) {
                error = "configured droptable '" + selector.spec
                    + "' is neither a known Treasure Class row nor a supported special selector";
                return false;
            }
            selector.sourceRowIndices.assign(1, row->second);
        }
    }
    return true;
}

struct TypeEquivalence {
    std::string code;
    std::string parent1;
    std::string parent2;
};

struct SourceItem {
    std::string code;
    std::string type1;
    std::string type2;
    std::uint32_t rarity{};
};

auto ReadItemSources(const std::vector<std::filesystem::path>& roots,
        std::vector<SourceItem>& items,
        std::vector<TypeEquivalence>& types,
        std::string& error) -> bool {
    items.clear();
    types.clear();
    const auto itemTypesPath = FindExcelFile(roots, "itemtypes.txt", false);
    if (!itemTypesPath.empty()) {
        TableText table;
        if (!ReadTsv(itemTypesPath, table, error)) return false;
        const auto codeColumn = ColumnIndex(table, "Code");
        const auto equiv1Column = ColumnIndex(table, "Equiv1");
        const auto equiv2Column = ColumnIndex(table, "Equiv2");
        if (codeColumn == std::numeric_limits<std::size_t>::max()) {
            error = "itemtypes.txt is missing its Code column";
            return false;
        }
        for (const auto& row : table.rows) {
            const auto code = Lower(Cell(row, codeColumn));
            if (code.empty()) continue;
            types.push_back({code, Lower(Cell(row, equiv1Column)),
                Lower(Cell(row, equiv2Column))});
        }
    }

    constexpr std::array<std::string_view, 3> itemFiles{
        "weapons.txt", "armor.txt", "misc.txt"};
    std::unordered_set<std::string> seen;
    for (const auto file : itemFiles) {
        const auto path = FindExcelFile(roots, file, false);
        if (path.empty()) continue;
        TableText table;
        if (!ReadTsv(path, table, error)) return false;
        const auto codeColumn = ColumnIndex(table, "code");
        const auto type1Column = ColumnIndex(table, "type");
        const auto type2Column = ColumnIndex(table, "type2");
        const auto rarityColumn = ColumnIndex(table, "rarity");
        if (codeColumn == std::numeric_limits<std::size_t>::max()) {
            error = std::string(file) + " is missing its code column";
            return false;
        }
        for (const auto& row : table.rows) {
            const auto code = Lower(Cell(row, codeColumn));
            if (!ValidCode(code) || !seen.insert(code).second) continue;
            const auto rarityValue = ParseSignedCell(Cell(row, rarityColumn), 1);
            const auto rarity = rarityValue > 0
                ? static_cast<std::uint32_t>(std::min<std::int64_t>(rarityValue, 1000000))
                : 1U;
            items.push_back({code, Lower(Cell(row, type1Column)),
                Lower(Cell(row, type2Column)), rarity});
        }
    }
    if (items.empty()) {
        error = "no valid item rows were found in the active mod files";
        return false;
    }
    return true;
}

auto ResolveRuleItems(std::vector<Rule>& rules,
        const std::vector<SourceItem>& items,
        const std::vector<TypeEquivalence>& types,
        std::string& error) -> bool {
    std::unordered_map<std::string, const SourceItem*> itemsByCode;
    itemsByCode.reserve(items.size());
    for (const auto& item : items) itemsByCode.emplace(item.code, &item);
    for (auto& rule : rules) {
        rule.itemOptions.clear();
        rule.itemOptions.reserve(rule.itemSpecs.size());
        for (const auto& itemSpec : rule.itemSpecs) {
            ItemOption option;
            option.quality = itemSpec.quality;
            const bool isType = itemSpec.itemCode.starts_with("type:");
            const std::string code = Lower(isType
                ? std::string_view(itemSpec.itemCode).substr(5)
                : std::string_view(itemSpec.itemCode));
            if (!isType) {
                const auto found = itemsByCode.find(code);
                if (found == itemsByCode.end()) {
                    error = "item" + std::to_string(rule.number)
                        + " code '" + code + "' was not found in the active mod item files";
                    return false;
                }
                option.candidates.push_back({
                    D2RL::Items::MakeItemCode(code[0], code[1],
                        code.size() > 2 ? code[2] : ' ',
                        code.size() > 3 ? code[3] : ' '),
                    std::numeric_limits<std::uint32_t>::max(),
                    found->second->rarity});
                rule.itemOptions.push_back(std::move(option));
                continue;
            }

            std::unordered_set<std::string> acceptedTypes{code};
            bool changed = true;
            while (changed) {
                changed = false;
                for (const auto& type : types) {
                    if (acceptedTypes.contains(type.code)) continue;
                    if ((!type.parent1.empty()
                            && acceptedTypes.contains(type.parent1))
                            || (!type.parent2.empty()
                                && acceptedTypes.contains(type.parent2))) {
                        acceptedTypes.insert(type.code);
                        changed = true;
                    }
                }
            }
            for (const auto& item : items) {
                if (acceptedTypes.contains(item.type1)
                        || acceptedTypes.contains(item.type2)) {
                    option.candidates.push_back({
                        D2RL::Items::MakeItemCode(item.code[0], item.code[1],
                            item.code.size() > 2 ? item.code[2] : ' ',
                            item.code.size() > 3 ? item.code[3] : ' '),
                        std::numeric_limits<std::uint32_t>::max(),
                        item.rarity});
                }
            }
            if (option.candidates.empty()) {
                error = "item" + std::to_string(rule.number)
                    + " type '" + code
                    + "' did not match any base items in the active mod";
                return false;
            }
            rule.itemOptions.push_back(std::move(option));
        }
    }
    return true;
}

auto RandomBelow(std::uint64_t upperExclusive) -> std::uint64_t {
    std::uniform_int_distribution<std::uint64_t> distribution(
        0, upperExclusive - 1);
    return distribution(RandomEngine);
}

auto SelectItemCandidate(const Rule& rule) -> ItemCandidate {
    if (rule.itemOptions.empty()) return {};
    const auto selectedOption = static_cast<std::size_t>(
        RandomBelow(rule.itemOptions.size()));
    const auto& option = rule.itemOptions[selectedOption];
    const auto& candidates = option.candidates;
    std::uint64_t total{};
    for (const auto& candidate : candidates) {
        total += candidate.rarityWeight;
    }
    if (candidates.empty()) return {};
    auto withQuality = [&](ItemCandidate candidate) {
        candidate.quality = option.quality;
        return candidate;
    };
    if (total == 0) return withQuality(candidates.front());
    auto selection = RandomBelow(total);
    for (const auto& candidate : candidates) {
        if (selection < candidate.rarityWeight) return withQuality(candidate);
        selection -= candidate.rarityWeight;
    }
    return withQuality(candidates.back());
}

auto HasSupportedBuild(const D2RL::PluginContext* context) noexcept -> bool {
    const char* values[] = {
        context != nullptr ? context->buildName : nullptr,
        context != nullptr ? context->buildVersion : nullptr,
    };
    return std::any_of(std::begin(values), std::end(values), [](const char* value) {
        return value != nullptr
            && (std::strcmp(value, SupportedBuildToken) == 0
                || std::strstr(value, "3.3.93847") != nullptr);
    });
}

auto IsExecutableAddress(std::uintptr_t address, std::size_t size) noexcept -> bool {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info,
            sizeof(info)) != sizeof(info)
            || info.State != MEM_COMMIT
            || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD protection = info.Protect & 0xFFU;
    const bool executable = protection == PAGE_EXECUTE
        || protection == PAGE_EXECUTE_READ
        || protection == PAGE_EXECUTE_READWRITE
        || protection == PAGE_EXECUTE_WRITECOPY;
    const auto end = address + size;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress)
        + info.RegionSize;
    return executable && end >= address && end <= regionEnd;
}

auto InstallGroundCoordinateAccessors() noexcept -> bool {
    if (Context == nullptr || Context->exeBase == 0) return false;
    const auto validate = [](std::uint64_t rva,
            const auto& expected) noexcept {
        const auto address = Context->exeBase + rva;
        return IsExecutableAddress(address, expected.size())
            && Context->CheckExpectedBytes(rva, expected.data(),
                static_cast<std::uint32_t>(expected.size()));
    };
    if (!validate(GetDynamicPathRva, GetDynamicPathExpectedBytes)
            || !validate(PathGetXRva, PathGetXExpectedBytes)
            || !validate(PathGetYRva, PathGetYExpectedBytes)) {
        return false;
    }
    GetDynamicPath = reinterpret_cast<GetDynamicPathFn>(
        Context->exeBase + GetDynamicPathRva);
    PathGetX = reinterpret_cast<PathGetCoordinateFn>(
        Context->exeBase + PathGetXRva);
    PathGetY = reinterpret_cast<PathGetCoordinateFn>(
        Context->exeBase + PathGetYRva);
    return true;
}

auto InstallNativeDropHelpers() noexcept -> bool {
    if (Context == nullptr || Context->exeBase == 0) return false;
    const auto validate = [](std::uint64_t rva,
            const auto& expected) noexcept {
        const auto address = Context->exeBase + rva;
        return IsExecutableAddress(address, expected.size())
            && Context->CheckExpectedBytes(rva, expected.data(),
                static_cast<std::uint32_t>(expected.size()));
    };
    if (!validate(GetUnitRoomRva, GetUnitRoomExpectedBytes)
            || !validate(CreateItemUnitRva,
                RandomTreasureDrop::NativeSignatures::CreateItemUnit)
            || !validate(ReturnItemToGroundRva,
                RandomTreasureDrop::NativeSignatures::ReturnItemToGround)) {
        return false;
    }
    GetUnitRoom = reinterpret_cast<GetUnitRoomFn>(
        Context->exeBase + GetUnitRoomRva);
    CreateItemUnit = reinterpret_cast<CreateItemUnitFn>(
        Context->exeBase + CreateItemUnitRva);
    ReturnItemToGround = reinterpret_cast<ReturnItemToGroundFn>(
        Context->exeBase + ReturnItemToGroundRva);
    return true;
}

auto InstallDropHook() noexcept -> bool {
    if (Context == nullptr || Context->exeBase == 0
            || !HasSupportedBuild(Context)) {
        if (Context != nullptr) {
            Context->LogError(
                "RandomTreasureDrop: native hook is limited to D2R build 3.3.93847.");
        }
        return false;
    }
    if (!InstallGroundCoordinateAccessors()) {
        Context->LogError(
            "RandomTreasureDrop: the ground-coordinate accessors did not match the validated D2R 3.3.93847 layout; the drop hook was not installed.");
        return false;
    }
    if (!InstallNativeDropHelpers()) {
        Context->LogError(
            "RandomTreasureDrop: the item-creation, ground-placement, or room helper signature did not match the validated D2R 3.3.93847 bytes; the drop hook was not installed.");
        return false;
    }
    const auto address = Context->exeBase + DropTreasureClassRva;
    if (!IsExecutableAddress(address, HookExpectedBytes.size())) {
        Context->LogError(
            "RandomTreasureDrop: the Treasure Class hook address is not executable.");
        return false;
    }

    // This signature was captured from the live D2R 3.3.93847 mapping. Keep it
    // independent of the current bytes so an unknown prologue fails closed.
    if (!Context->CheckExpectedBytes(DropTreasureClassRva,
            HookExpectedBytes.data(),
            static_cast<std::uint32_t>(HookExpectedBytes.size()))) {
        Context->LogError(
            "RandomTreasureDrop: the 3.3.93847 Treasure Class hook signature did not match; the hook was not installed.");
        return false;
    }
    if (!Context->InstallInlineHook(DropTreasureClassRva,
            HookExpectedBytes.data(),
            static_cast<std::uint32_t>(HookExpectedBytes.size()),
            &DropTreasureClassHook, &OriginalDropTreasureClass)
            || OriginalDropTreasureClass == nullptr) {
        Context->LogError(
            "RandomTreasureDrop: D2RLoader refused the Treasure Class hook.");
        return false;
    }
    Context->LogInfo(
        "RandomTreasureDrop: native drop hook installed with byte-validated item creation, room, and ground-placement helpers.");
    return true;
}

auto ReadMemory(const void* address, void* output, std::size_t size) noexcept -> bool {
    SIZE_T bytesRead{};
    return address != nullptr && output != nullptr
        && ReadProcessMemory(GetCurrentProcess(), address, output, size,
            &bytesRead) != 0
        && bytesRead == size;
}

__declspec(noinline) auto TryGetGroundCoordinates(void* unit,
        std::uint32_t& x, std::uint32_t& y) noexcept -> bool {
    if (unit == nullptr || GetDynamicPath == nullptr
            || PathGetX == nullptr || PathGetY == nullptr
            || (reinterpret_cast<std::uintptr_t>(unit)
                & (alignof(void*) - 1U)) != 0U) {
        return false;
    }
    __try {
        void* const path = GetDynamicPath(unit);
        if (path == nullptr
                || (reinterpret_cast<std::uintptr_t>(path)
                    & (alignof(void*) - 1U)) != 0U) {
            return false;
        }
        const auto nativeX = PathGetX(path);
        const auto nativeY = PathGetY(path);
        if (nativeX <= 0 || nativeY <= 0
                || nativeX > 0xFFFF || nativeY > 0xFFFF) {
            return false;
        }
        x = static_cast<std::uint32_t>(nativeX);
        y = static_cast<std::uint32_t>(nativeY);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

__declspec(noinline) auto TryGetDropLocation(void* unit,
        std::uint32_t& x, std::uint32_t& y, void*& room) noexcept -> bool {
    room = nullptr;
    if (unit == nullptr || GetUnitRoom == nullptr
            || (reinterpret_cast<std::uintptr_t>(unit)
                & (alignof(void*) - 1U)) != 0U
            || !TryGetGroundCoordinates(unit, x, y)) {
        return false;
    }
    __try {
        room = GetUnitRoom(unit);
        return room != nullptr
            && (reinterpret_cast<std::uintptr_t>(room)
                & (alignof(void*) - 1U)) == 0U;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        room = nullptr;
        return false;
    }
}

auto IsRepeatedDropEvent(void* game, void* sourceUnit,
        std::uint32_t rowIndex, void* room,
        std::uint32_t x, std::uint32_t y) noexcept -> bool {
    const auto now = static_cast<std::uint64_t>(GetTickCount64());
    const std::lock_guard<std::mutex> lock(StateMutex);
    for (std::size_t offset = 0; offset < RecentDropEventCount; ++offset) {
        const auto index = (RecentDropEventNext + RecentDropEventCapacity
            - 1U - offset) % RecentDropEventCapacity;
        const auto& recent = RecentDropEvents[index];
        if (now < recent.tick
                || now - recent.tick > DuplicateDropSuppressionWindowMs) {
            continue;
        }
        if (recent.game != game || recent.rowIndex != rowIndex) continue;
        const bool sameSource = sourceUnit != nullptr
            && recent.sourceUnit == sourceUnit;
        const bool samePosition = room != nullptr && recent.room == room
            && recent.x == x && recent.y == y;
        if (sameSource || samePosition) return true;
    }

    RecentDropEvents[RecentDropEventNext] = {
        game, sourceUnit, room, rowIndex, x, y, now,
    };
    RecentDropEventNext = (RecentDropEventNext + 1U)
        % RecentDropEventCapacity;
    RecentDropEventCount = std::min(RecentDropEventCount + 1U,
        RecentDropEventCapacity);
    return false;
}

__declspec(noinline) auto CreateAndPlaceBonusDrop(void* game, void* player,
        std::uint32_t itemClassId, std::uint32_t itemLevel,
        ItemQuality itemQuality, void* room,
        std::uint32_t x, std::uint32_t y) noexcept -> NativeDropResult {
    if (game == nullptr || player == nullptr || room == nullptr
            || CreateItemUnit == nullptr || ReturnItemToGround == nullptr
            || (reinterpret_cast<std::uintptr_t>(game)
                & (alignof(void*) - 1U)) != 0U
            || (reinterpret_cast<std::uintptr_t>(player)
                & (alignof(void*) - 1U)) != 0U
            || (reinterpret_cast<std::uintptr_t>(room)
                & (alignof(void*) - 1U)) != 0U
            || x > 0xFFFF || y > 0xFFFF) {
        return NativeDropResult::InvalidContext;
    }
    void* item{};
    __try {
        item = CreateItemUnit(player, itemClassId, game,
            4, static_cast<std::int32_t>(itemQuality), 1, 1,
            static_cast<std::int32_t>(itemLevel), 0, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NativeDropResult::ItemCreationException;
    }
    if (item == nullptr) return NativeDropResult::ItemCreationReturnedNull;
    if ((reinterpret_cast<std::uintptr_t>(item)
            & (alignof(void*) - 1U)) != 0U) {
        return NativeDropResult::ItemCreationReturnedNull;
    }
    __try {
        ReturnItemToGround(game, nullptr, item, room,
            static_cast<std::int32_t>(x), static_cast<std::int32_t>(y));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return NativeDropResult::GroundPlacementException;
    }
    return NativeDropResult::Success;
}

void LogNativeDropProbe(
        const void* validatedRow,
        void* game,
        void* monster,
        void* player,
        std::int32_t quality,
        std::int32_t level,
        std::int32_t arg7,
        void** items,
        std::int32_t* itemsDropped,
        std::int32_t maxItems,
        std::int32_t picks) noexcept {
    if (Context == nullptr) return;
    std::int32_t droppedCount = -1;
    const bool hasDroppedCount = itemsDropped != nullptr
        && ReadMemory(itemsDropped, &droppedCount, sizeof(droppedCount));
    char summary[512]{};
    (void)std::snprintf(summary, sizeof(summary),
        "RandomTreasureDrop probe: picks=%d quality=%d level=%d arg7=%d dropped=%d maxItems=%d game=%p monster=%p player=%p items=%p.",
        picks, quality, level, arg7,
        hasDroppedCount ? droppedCount : -1, maxItems,
        game, monster, player, static_cast<void*>(items));
    Context->LogInfo(summary);

    std::array<std::uint8_t, CompiledTreasureClassRowSize> rowBytes{};
    if (validatedRow == nullptr
            || !ReadMemory(validatedRow, rowBytes.data(), rowBytes.size())) {
        Context->LogWarn(
            "RandomTreasureDrop probe: could not safely copy the validated runtime row.");
        return;
    }
    constexpr std::size_t bytesPerLine = 48;
    for (std::size_t offset = 0; offset < rowBytes.size(); offset += bytesPerLine) {
        char message[192]{};
        const auto lineBytes = std::min(bytesPerLine, rowBytes.size() - offset);
        int length = std::snprintf(message, sizeof(message),
            "RandomTreasureDrop probe: row-bytes offset=0x%03X data=",
            static_cast<unsigned>(offset));
        for (std::size_t index = 0; index < lineBytes
                && length > 0
                && static_cast<std::size_t>(length) < sizeof(message); ++index) {
            length += std::snprintf(message + length,
                sizeof(message) - static_cast<std::size_t>(length), "%02X",
                static_cast<unsigned>(rowBytes[offset + index]));
        }
        Context->LogInfo(message);
    }

    std::uint64_t itemListAddress{};
    std::uint64_t tcEntryCount{};
    std::uint64_t itemCapacity{};
    const auto* row = static_cast<const std::uint8_t*>(validatedRow);
    if (!ReadMemory(row + TreasureClassItemListOffset,
                &itemListAddress, sizeof(itemListAddress))
            || !ReadMemory(row + TreasureClassItemListCountOffset,
                &tcEntryCount, sizeof(tcEntryCount))
            || !ReadMemory(row + TreasureClassItemListCapacityOffset,
                &itemCapacity, sizeof(itemCapacity))) {
        Context->LogWarn(
            "RandomTreasureDrop probe: could not safely read the runtime item-list header.");
    } else {
        char message[224]{};
        (void)std::snprintf(message, sizeof(message),
            "RandomTreasureDrop probe: item-list address=%p size=%llu capacity=%llu.",
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(itemListAddress)),
            static_cast<unsigned long long>(tcEntryCount),
            static_cast<unsigned long long>(itemCapacity));
        Context->LogInfo(message);
        if (tcEntryCount > TreasureClassMaxItems
                || itemCapacity > TreasureClassMaxItems
                || tcEntryCount > itemCapacity
                || (tcEntryCount != 0 && itemListAddress == 0)) {
            Context->LogWarn(
                "RandomTreasureDrop probe: runtime item-list dimensions were outside the expected TXT bounds.");
        } else if (tcEntryCount == 0) {
            Context->LogInfo(
                "RandomTreasureDrop probe: dynamic item list is empty.");
        } else {
            std::array<std::uint8_t,
                TreasureClassMaxItems
                    * TreasureClassItemEntryDiagnosticSize> entries{};
            const auto bytesToRead = static_cast<std::size_t>(tcEntryCount)
                * TreasureClassItemEntryDiagnosticSize;
            if (!ReadMemory(reinterpret_cast<const void*>(
                        static_cast<std::uintptr_t>(itemListAddress)),
                    entries.data(), bytesToRead)) {
                Context->LogWarn(
                    "RandomTreasureDrop probe: could not copy candidate 32-byte runtime item-list entries.");
            } else {
                for (std::size_t index = 0; index < tcEntryCount; ++index) {
                    char entryMessage[224]{};
                    const auto* entry = entries.data()
                        + index * TreasureClassItemEntryDiagnosticSize;
                    int length = std::snprintf(entryMessage,
                        sizeof(entryMessage),
                        "RandomTreasureDrop probe: item-list[%zu] candidate32=", index);
                    for (std::size_t byte = 0;
                            byte < TreasureClassItemEntryDiagnosticSize
                                && length > 0
                                && static_cast<std::size_t>(length)
                                    < sizeof(entryMessage);
                            ++byte) {
                        length += std::snprintf(entryMessage + length,
                            sizeof(entryMessage)
                                - static_cast<std::size_t>(length), "%02X",
                            static_cast<unsigned>(entry[byte]));
                    }
                    Context->LogInfo(entryMessage);
                }
            }
        }
    }

    if (!hasDroppedCount || items == nullptr || maxItems <= 0
            || droppedCount < 0 || droppedCount > maxItems) {
        return;
    }
    const auto itemCount = std::min<std::int32_t>(droppedCount, 16);
    for (std::int32_t index = 0; index < itemCount; ++index) {
        void* nativeItem{};
        const auto* itemSlot = reinterpret_cast<const std::uint8_t*>(items)
            + static_cast<std::size_t>(index) * sizeof(nativeItem);
        if (!ReadMemory(itemSlot, &nativeItem, sizeof(nativeItem))) break;
        char message[160]{};
        (void)std::snprintf(message, sizeof(message),
            "RandomTreasureDrop probe: native-output[%d]=%p.", index,
            nativeItem);
        Context->LogInfo(message);
    }
}

auto GetTableRowIndex(const void* row,
        const D2RL::DataTables::TableView& view) noexcept -> std::uint32_t {
    if (row == nullptr || view.rows == nullptr
            || view.rowSize != CompiledTreasureClassRowSize
            || !TableReady.load(std::memory_order_acquire)
            || view.revision != RuntimeTableRevision.load(
                std::memory_order_acquire)) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    const auto base = reinterpret_cast<std::uintptr_t>(view.rows);
    const auto count = view.rowCount;
    const auto address = reinterpret_cast<std::uintptr_t>(row);
    if (base == 0 || count == 0) return std::numeric_limits<std::uint32_t>::max();
    const auto tableBytes = static_cast<std::uintptr_t>(count)
        * CompiledTreasureClassRowSize;
    if (address >= base && address - base < tableBytes
            && (address - base) % CompiledTreasureClassRowSize == 0) {
        return static_cast<std::uint32_t>(
            (address - base) / CompiledTreasureClassRowSize);
    }

    std::lock_guard<std::mutex> lock(StateMutex);
    const auto found = TreasureClassByHash.find(RowHash(row));
    if (found == TreasureClassByHash.end()) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    for (const auto index : found->second) {
        const auto* tableRow = reinterpret_cast<const void*>(base
            + static_cast<std::uintptr_t>(index)
                * CompiledTreasureClassRowSize);
        if (RowsEqualExceptPicks(row, tableRow)) return index;
    }
    return std::numeric_limits<std::uint32_t>::max();
}

void __fastcall DropTreasureClassHook(
        void* game,
        void* monster,
        void* player,
        void* treasureClass,
        std::int32_t quality,
        std::int32_t level,
        std::int32_t arg7,
        void** items,
        std::int32_t* itemsDropped,
        std::int32_t maxItems) {
    if constexpr (EnableDropOutcomeDiagnostics) {
        if (GameActive.load(std::memory_order_acquire)) {
            ObservedDropHookCount.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    std::uint32_t probeOrdinal = std::numeric_limits<std::uint32_t>::max();
    if (EnableProbeLogging && Context != nullptr
            && GameActive.load(std::memory_order_acquire)) {
        probeOrdinal = NativeDropProbeCallCount.fetch_add(
            1, std::memory_order_acq_rel);
        if (probeOrdinal < MaximumNativeProbeCalls) {
            char summary[384]{};
            (void)std::snprintf(summary, sizeof(summary),
                "RandomTreasureDrop probe: hook-entry[%u] quality=%d level=%d arg7=%d items=%p droppedCount=%p maxItems=%d.",
                static_cast<unsigned>(probeOrdinal),
                quality, level, arg7, static_cast<void*>(items),
                static_cast<void*>(itemsDropped), maxItems);
            Context->LogInfo(summary);
        }
    }
    if (OriginalDropTreasureClass != nullptr) {
        OriginalDropTreasureClass(game, monster, player, treasureClass,
            quality, level, arg7, items, itemsDropped, maxItems);
    }
    if (Context == nullptr || !GameActive.load(std::memory_order_acquire)
            || !TableReady.load(std::memory_order_acquire)
            || DataTableService == nullptr || treasureClass == nullptr) {
        return;
    }
    try {
        // Query the read-only table on the game thread for each hook call so
        // no row pointer is retained across a table reload.
        D2RL::DataTables::TableView tableView{
            .structSize = D2RL::DataTables::TableViewSize,
        };
        if (DataTableService->getTable(Context,
                D2RL::DataTables::Bank::Rotw,
                static_cast<D2RL::DataTables::TableId>(
                    CompiledTreasureClassTableId),
                &tableView) != D2RL::DataTables::Result::Success
                || tableView.rowCount != RuntimeTableRowCount.load(
                    std::memory_order_acquire)
                || tableView.rowSize != CompiledTreasureClassRowSize) {
            return;
        }
        const auto rowIndex = GetTableRowIndex(treasureClass, tableView);
        if (rowIndex != std::numeric_limits<std::uint32_t>::max()) {
            MappedDropHookCount.fetch_add(1, std::memory_order_acq_rel);
        }
        if (probeOrdinal < MaximumNativeProbeCalls) {
            const auto sourceOffset = RuntimeSourceRowOffset.load(
                std::memory_order_acquire);
            const bool rowIndexValid = rowIndex
                != std::numeric_limits<std::uint32_t>::max();
            const bool sourceIndexValid = rowIndexValid
                && rowIndex >= sourceOffset
                && static_cast<std::size_t>(rowIndex - sourceOffset)
                    < TreasureClasses.size();
            bool matchesSelectedRow{};
            if (sourceIndexValid) {
                const std::lock_guard<std::mutex> lock(StateMutex);
                for (const auto& [selectedRowIndex, ruleIndex] : ActiveRules) {
                    if (selectedRowIndex == rowIndex
                            && ruleIndex < Rules.size()) {
                        matchesSelectedRow = true;
                        break;
                    }
                }
            }
            char summary[384]{};
            (void)std::snprintf(summary, sizeof(summary),
                "RandomTreasureDrop probe: hook-map[%u] selected-table-match=%s.",
                static_cast<unsigned>(probeOrdinal),
                matchesSelectedRow ? "yes" : "no");
            Context->LogInfo(summary);
        }
        if (rowIndex == std::numeric_limits<std::uint32_t>::max()) return;
        const auto sourceOffset = RuntimeSourceRowOffset.load(
            std::memory_order_acquire);
        if (rowIndex < sourceOffset) return;
        const auto sourceIndex = static_cast<std::size_t>(rowIndex - sourceOffset);
        if (sourceIndex >= TreasureClasses.size()) return;
        std::uint32_t matchingGroupCount{};
        {
            const std::lock_guard<std::mutex> lock(StateMutex);
            for (const auto& [selectedRowIndex, ruleIndex] : ActiveRules) {
                if (selectedRowIndex == rowIndex && ruleIndex < Rules.size()) {
                    ++matchingGroupCount;
                }
            }
        }
        if (matchingGroupCount == 0) return;
        const auto matchedCallback = MatchedDropCallbackCount.fetch_add(
            1, std::memory_order_acq_rel) + 1;
        if (!EnableBonusDrops) return;
        if (game == nullptr || player == nullptr) {
            if constexpr (EnableDropOutcomeDiagnostics) {
                char summary[192]{};
                (void)std::snprintf(summary, sizeof(summary),
                    "RandomTreasureDrop trace: matched callback=%u groups=%u; drop context unavailable.",
                    static_cast<unsigned>(matchedCallback),
                    static_cast<unsigned>(matchingGroupCount));
                Context->LogInfo(summary);
            }
            return;
        }

        const std::uint32_t itemLevel = static_cast<std::uint32_t>(
            std::clamp(level, 1, 99));
        std::uint32_t dropX{};
        std::uint32_t dropY{};
        void* dropRoom{};
        if (!TryGetDropLocation(monster, dropX, dropY, dropRoom)
                && !TryGetDropLocation(player, dropX, dropY, dropRoom)) {
            if constexpr (EnableDropOutcomeDiagnostics) {
                char summary[192]{};
                (void)std::snprintf(summary, sizeof(summary),
                    "RandomTreasureDrop trace: matched callback=%u groups=%u; no usable drop location.",
                    static_cast<unsigned>(matchedCallback),
                    static_cast<unsigned>(matchingGroupCount));
                Context->LogInfo(summary);
            }
            return;
        }
        if (IsRepeatedDropEvent(game, monster, rowIndex, dropRoom,
                dropX, dropY)) {
            const auto suppressedCount =
                SuppressedDuplicateDropCount.fetch_add(
                    1, std::memory_order_acq_rel) + 1;
            if constexpr (EnableDropOutcomeDiagnostics) {
                char summary[192]{};
                (void)std::snprintf(summary, sizeof(summary),
                    "RandomTreasureDrop trace: callback=%u repeat suppressed at (%u,%u); suppressed=%u.",
                    static_cast<unsigned>(matchedCallback),
                    static_cast<unsigned>(dropX),
                    static_cast<unsigned>(dropY),
                    static_cast<unsigned>(suppressedCount));
                Context->LogInfo(summary);
            }
            return;
        }
        if constexpr (EnableDropOutcomeDiagnostics) {
            char summary[192]{};
            (void)std::snprintf(summary, sizeof(summary),
                "RandomTreasureDrop trace: matched callback=%u groups=%u at (%u,%u).",
                static_cast<unsigned>(matchedCallback),
                static_cast<unsigned>(matchingGroupCount),
                static_cast<unsigned>(dropX),
                static_cast<unsigned>(dropY));
            Context->LogInfo(summary);
        }
        std::vector<BonusDrop> earnedDrops;
        std::vector<std::string> diagnosticMessages;
        {
            const std::lock_guard<std::mutex> lock(StateMutex);
            for (const auto& [selectedRow, ruleIndex] : ActiveRules) {
                if (selectedRow != rowIndex || ruleIndex >= Rules.size()) continue;
                const auto itemWeight = static_cast<std::uint64_t>(
                    Rules[ruleIndex].itemWeight);
                const auto nativeWeight = TreasureClasses[sourceIndex].nativeWeight;
                if (itemWeight == 0
                        || nativeWeight > std::numeric_limits<std::uint64_t>::max()
                            - itemWeight) {
                    continue;
                }
                const auto totalWeight = nativeWeight + itemWeight;
                if (totalWeight == 0) {
                    continue;
                }
                const bool rollPassed = RandomBelow(totalWeight) >= nativeWeight;
                if constexpr (EnableDropOutcomeDiagnostics) {
                    char summary[192]{};
                    (void)std::snprintf(summary, sizeof(summary),
                        "RandomTreasureDrop trace: callback=%u item%u roll=%s.",
                        static_cast<unsigned>(matchedCallback),
                        static_cast<unsigned>(Rules[ruleIndex].number),
                        rollPassed ? "passed" : "missed");
                    diagnosticMessages.emplace_back(summary);
                }
                if (!rollPassed) continue;
                const auto candidate = SelectItemCandidate(Rules[ruleIndex]);
                if (candidate.packedCode == 0
                        || candidate.classId
                            == std::numeric_limits<std::uint32_t>::max()) {
                    if constexpr (EnableDropOutcomeDiagnostics) {
                        char summary[192]{};
                        (void)std::snprintf(summary, sizeof(summary),
                            "RandomTreasureDrop trace: callback=%u item%u passed its roll but had no resolved item candidate.",
                            static_cast<unsigned>(matchedCallback),
                            static_cast<unsigned>(Rules[ruleIndex].number));
                        diagnosticMessages.emplace_back(summary);
                    }
                    continue;
                }
                earnedDrops.push_back({
                    candidate.packedCode,
                    candidate.classId,
                    itemLevel,
                    candidate.quality,
                    dropX,
                    dropY,
                    dropRoom,
                });
            }
        }
        for (const auto& message : diagnosticMessages) {
            Context->LogInfo(message.c_str());
        }
        for (const auto& drop : earnedDrops) {
            const auto nativeResult = CreateAndPlaceBonusDrop(game, player,
                drop.itemClassId, drop.itemLevel, drop.quality,
                drop.room, drop.x, drop.y);
            if (nativeResult != NativeDropResult::Success) {
                char message[192]{};
                (void)std::snprintf(message, sizeof(message),
                    "RandomTreasureDrop: native bonus drop failed stage=%s code=0x%08X classId=%u at (%u,%u).",
                    NativeDropResultName(nativeResult),
                    static_cast<unsigned>(drop.itemCode),
                    static_cast<unsigned>(drop.itemClassId),
                    static_cast<unsigned>(drop.x),
                    static_cast<unsigned>(drop.y));
                Context->LogWarn(message);
                continue;
            }
            char message[192]{};
            (void)std::snprintf(message, sizeof(message),
                "RandomTreasureDrop: native bonus drop created code=0x%08X classId=%u at (%u,%u).",
                static_cast<unsigned>(drop.itemCode),
                static_cast<unsigned>(drop.itemClassId),
                static_cast<unsigned>(drop.x),
                static_cast<unsigned>(drop.y));
            Context->LogInfo(message);
        }
    } catch (...) {
        if (Context != nullptr) {
            Context->LogWarn(
                "RandomTreasureDrop: an extra-drop evaluation failed; native drops were preserved.");
        }
    }
}

auto ResolveNativeItemClassIds(std::uint64_t revision) noexcept -> bool {
    if (Context == nullptr || DataTableService == nullptr) return false;
    D2RL::DataTables::TableView itemsTable{
        .structSize = D2RL::DataTables::TableViewSize,
    };
    if (DataTableService->getTable(Context, D2RL::DataTables::Bank::Rotw,
            D2RL::DataTables::TableId::Items, &itemsTable)
                != D2RL::DataTables::Result::Success
            || !D2RL::DataTables::HasTableViewField(&itemsTable,
                D2RL::DataTables::TableViewRequiredSize)
            || itemsTable.revision != revision
            || itemsTable.rows == nullptr || itemsTable.rowCount == 0
            || itemsTable.rowSize == 0) {
        Context->LogError(
            "RandomTreasureDrop: compiled Items table validation failed; bonus drops are disabled.");
        return false;
    }
    const auto itemsBase = reinterpret_cast<std::uintptr_t>(itemsTable.rows);
    std::size_t resolved{};
    std::lock_guard<std::mutex> lock(StateMutex);
    for (auto& rule : Rules) {
        for (auto& option : rule.itemOptions) {
            for (auto& candidate : option.candidates) {
                D2RL::DataTables::RowView row{
                    .structSize = D2RL::DataTables::RowViewSize,
                };
                if (DataTableService->findRowByCode(Context,
                        D2RL::DataTables::Bank::Rotw,
                        D2RL::DataTables::TableId::Items,
                        candidate.packedCode, &row)
                            != D2RL::DataTables::Result::Success
                        || !D2RL::DataTables::HasRowViewField(&row,
                            D2RL::DataTables::RowViewRequiredSize)
                        || row.tableId != D2RL::DataTables::TableId::Items
                        || row.bank != D2RL::DataTables::Bank::Rotw
                        || row.revision != revision
                        || row.row == nullptr
                        || row.rowSize != itemsTable.rowSize
                        || row.rowIndex >= itemsTable.rowCount) {
                    char message[192]{};
                    (void)std::snprintf(message, sizeof(message),
                        "RandomTreasureDrop: could not resolve item code 0x%08X to a native class ID; bonus drops are disabled.",
                        static_cast<unsigned>(candidate.packedCode));
                    Context->LogError(message);
                    return false;
                }
                const auto expectedAddress = itemsBase
                    + static_cast<std::uintptr_t>(row.rowIndex)
                        * itemsTable.rowSize;
                if (expectedAddress < itemsBase
                        || reinterpret_cast<std::uintptr_t>(row.row)
                            != expectedAddress) {
                    Context->LogError(
                        "RandomTreasureDrop: item code lookup returned an inconsistent Items row; bonus drops are disabled.");
                    return false;
                }
                candidate.classId = row.rowIndex;
                ++resolved;
            }
        }
    }
    if (resolved == 0) return Rules.empty();
    char message[160]{};
    (void)std::snprintf(message, sizeof(message),
        "RandomTreasureDrop: resolved %u configured item candidates to native class IDs.",
        static_cast<unsigned>(resolved));
    Context->LogInfo(message);
    return true;
}

void __cdecl OnDataTablesLoaded(
        const D2RL::PluginContext* context,
        const D2RL::Lifecycle::DataTablesLoadedEvent* event,
        void*) noexcept {
    TableReady.store(false, std::memory_order_release);
    GameActive.store(false, std::memory_order_release);
    NativeDropProbeLogged.store(false, std::memory_order_release);
    NativeDropProbeCallCount.store(0, std::memory_order_release);
    ObservedDropHookCount.store(0, std::memory_order_release);
    MappedDropHookCount.store(0, std::memory_order_release);
    MatchedDropCallbackCount.store(0, std::memory_order_release);
    SuppressedDuplicateDropCount.store(0, std::memory_order_release);
    RuntimeTableRowCount.store(0, std::memory_order_release);
    RuntimeSourceRowOffset.store(0, std::memory_order_release);
    RuntimeTableRevision.store(0, std::memory_order_release);
    if (context == nullptr || context != Context || DataTableService == nullptr
            || !D2RL::Lifecycle::HasDataTablesLoadedEventField(event,
                D2RL::Lifecycle::DataTablesLoadedEventRequiredSize)) {
        if (Context != nullptr) {
            Context->LogError(
                "RandomTreasureDrop: invalid DataTablesLoaded event; custom drops are disabled.");
        }
        return;
    }
    D2RL::DataTables::TableView view{
        .structSize = D2RL::DataTables::TableViewSize,
    };
    const auto result = DataTableService->getTable(context,
        D2RL::DataTables::Bank::Rotw,
        static_cast<D2RL::DataTables::TableId>(CompiledTreasureClassTableId),
        &view);
    if (result != D2RL::DataTables::Result::Success
            || view.revision != event->revision
            || view.rows == nullptr
            || view.rowCount == 0
            || view.rowSize != CompiledTreasureClassRowSize
            || view.rowCount < TreasureClasses.size()) {
        const std::string diagnostic =
            "RandomTreasureDrop: compiled Treasure Classes table validation failed; custom drops are disabled. result="
            + std::to_string(static_cast<std::uint32_t>(result))
            + " eventRevision=" + std::to_string(event->revision)
            + " viewRevision=" + std::to_string(view.revision)
            + " tableId=" + std::to_string(static_cast<std::uint32_t>(view.tableId))
            + " bank=" + std::to_string(static_cast<std::uint32_t>(view.bank))
            + " rows=" + std::to_string(reinterpret_cast<std::uintptr_t>(view.rows))
            + " runtimeRowCount=" + std::to_string(view.rowCount)
            + " parsedRowCount=" + std::to_string(TreasureClasses.size())
            + " runtimeRowSize=" + std::to_string(view.rowSize)
            + " expectedRowSize=" + std::to_string(CompiledTreasureClassRowSize) + ".";
        Context->LogError(diagnostic.c_str());
        return;
    }
    const auto* base = static_cast<const std::uint8_t*>(view.rows);
    const std::size_t lastOffset = view.rowCount - TreasureClasses.size();
    std::size_t matchedOffset = std::numeric_limits<std::size_t>::max();
    std::size_t exactMatches{};
    std::size_t bestOffset{};
    std::size_t bestMatchingRows{};
    for (std::size_t offset = 0; offset <= lastOffset; ++offset) {
        std::size_t matchingRows{};
        for (std::size_t sourceIndex = 0;
                sourceIndex < TreasureClasses.size(); ++sourceIndex) {
            const auto* row = base
                + (offset + sourceIndex) * view.rowSize;
            std::int32_t runtimePicks{};
            std::memcpy(&runtimePicks, row + TreasureClassPicksOffset,
                sizeof(runtimePicks));
            if (runtimePicks == TreasureClasses[sourceIndex].picks) {
                ++matchingRows;
            }
        }
        if (matchingRows > bestMatchingRows) {
            bestOffset = offset;
            bestMatchingRows = matchingRows;
        }
        if (matchingRows == TreasureClasses.size()) {
            matchedOffset = offset;
            ++exactMatches;
        }
    }
    if (exactMatches != 1) {
        const std::string diagnostic =
            "RandomTreasureDrop: could not uniquely align source rows with compiled Treasure Classes; custom drops are disabled. exactAlignments="
            + std::to_string(exactMatches)
            + " bestRuntimeOffset=" + std::to_string(bestOffset)
            + " matchingPicks=" + std::to_string(bestMatchingRows)
            + " sourceRowCount=" + std::to_string(TreasureClasses.size())
            + " runtimeRowCount=" + std::to_string(view.rowCount) + ".";
        Context->LogError(diagnostic.c_str());
        return;
    }
    try {
        if (!ResolveNativeItemClassIds(event->revision)) return;
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> hashes;
        hashes.reserve(view.rowCount);
        for (std::uint32_t index = 0; index < view.rowCount; ++index) {
            const auto* row = base
                + static_cast<std::size_t>(index) * view.rowSize;
            hashes[RowHash(row)].push_back(index);
        }
        {
            const std::lock_guard<std::mutex> lock(StateMutex);
            TreasureClassByHash = std::move(hashes);
            ActiveRules.clear();
            RecentDropEvents.fill(RecentDropEvent{});
            RecentDropEventNext = 0;
            RecentDropEventCount = 0;
        }
        RuntimeTableRowCount.store(view.rowCount, std::memory_order_release);
        RuntimeSourceRowOffset.store(
            static_cast<std::uint32_t>(matchedOffset),
            std::memory_order_release);
        RuntimeTableRevision.store(view.revision, std::memory_order_release);
        TableReady.store(true, std::memory_order_release);
        const std::string validated =
            "RandomTreasureDrop: compiled Treasure Classes rows aligned uniquely by Picks; runtimeOffset="
            + std::to_string(matchedOffset)
            + " runtimeRowCount="
            + std::to_string(view.rowCount)
            + " sourceRowCount=" + std::to_string(TreasureClasses.size()) + ".";
        Context->LogInfo(validated.c_str());

        if constexpr (EnableProbeLogging) {
            constexpr std::array<std::string_view, 4> SnapshotRowNames{
                "Gold",
                "Jewelry C",
                "Act 1 Chest A",
                "Act 1 (H) Good Desecrated Herald",
            };
            for (const auto rowName : SnapshotRowNames) {
                const auto source = TreasureClassByName.find(Lower(rowName));
                if (source == TreasureClassByName.end()) continue;
                const auto runtimeIndex = static_cast<std::size_t>(source->second)
                    + matchedOffset;
                if (runtimeIndex >= view.rowCount) continue;
                const auto* row = base + runtimeIndex * view.rowSize;
                std::int32_t rowPicks{};
                if (!ReadMemory(row + TreasureClassPicksOffset,
                        &rowPicks, sizeof(rowPicks))) {
                    continue;
                }
                LogNativeDropProbe(row,
                    nullptr, nullptr, nullptr, 0, 0, 0,
                    nullptr, nullptr, 0, rowPicks);
            }
        }
    } catch (...) {
        Context->LogError(
            "RandomTreasureDrop: could not cache compiled Treasure Classes rows.");
    }
}

void __cdecl OnGameJoined(
        const D2RL::PluginContext* context,
        const D2RL::Lifecycle::GameplayEvent* event,
        void*) noexcept {
    if (context == nullptr || context != Context
            || !D2RL::Lifecycle::HasGameplayEventField(event,
                D2RL::Lifecycle::GameplayEventRequiredSize)) {
        return;
    }
    GameActive.store(false, std::memory_order_release);
    NativeDropProbeLogged.store(false, std::memory_order_release);
    NativeDropProbeCallCount.store(0, std::memory_order_release);
    ObservedDropHookCount.store(0, std::memory_order_release);
    MappedDropHookCount.store(0, std::memory_order_release);
    MatchedDropCallbackCount.store(0, std::memory_order_release);
    SuppressedDuplicateDropCount.store(0, std::memory_order_release);
    try {
    std::vector<std::string> selectionMessages;
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        ActiveRules.clear();
        RecentDropEvents.fill(RecentDropEvent{});
        RecentDropEventNext = 0;
        RecentDropEventCount = 0;
        if (!TableReady.load(std::memory_order_acquire)) {
            Context->LogError(
                "RandomTreasureDrop: game joined before Treasure Classes were validated; custom drops are disabled for this session.");
            GameActive.store(false, std::memory_order_release);
            return;
        }
        for (std::size_t ruleIndex = 0; ruleIndex < Rules.size(); ++ruleIndex) {
            const auto& rule = Rules[ruleIndex];
            if (rule.dropTableSelectors.empty()) continue;
            const auto selectedSelectorIndex = static_cast<std::size_t>(
                RandomBelow(rule.dropTableSelectors.size()));
            const auto& selector =
                rule.dropTableSelectors[selectedSelectorIndex];
            if (selector.sourceRowIndices.empty()) {
                Context->LogError(
                    "RandomTreasureDrop: a configured droptable selector resolved incorrectly; custom drops are disabled.");
                ActiveRules.clear();
                GameActive.store(false, std::memory_order_release);
                return;
            }
            const auto sourceOffset = RuntimeSourceRowOffset.load(
                std::memory_order_acquire);
            const auto runtimeRowCount = RuntimeTableRowCount.load(
                std::memory_order_acquire);
            for (const auto sourceRow : selector.sourceRowIndices) {
                if (sourceRow >= TreasureClasses.size()
                        || sourceRow > std::numeric_limits<std::uint32_t>::max()
                            - sourceOffset) {
                    Context->LogError(
                        "RandomTreasureDrop: a selected droptable row was outside the validated source table; custom drops are disabled.");
                    ActiveRules.clear();
                    GameActive.store(false, std::memory_order_release);
                    return;
                }
                const auto runtimeRow = sourceRow + sourceOffset;
                if (runtimeRow >= runtimeRowCount) {
                    Context->LogError(
                        "RandomTreasureDrop: a selected droptable row was outside the validated runtime table; custom drops are disabled.");
                    ActiveRules.clear();
                    GameActive.store(false, std::memory_order_release);
                    return;
                }
            }
            const auto selectedRowIndex = static_cast<std::size_t>(
                RandomBelow(selector.sourceRowIndices.size()));
            const auto selectedSourceRow =
                selector.sourceRowIndices[selectedRowIndex];
            const auto runtimeRow = selectedSourceRow + sourceOffset;
            ActiveRules.emplace_back(runtimeRow, ruleIndex);
            char message[160]{};
            (void)std::snprintf(message, sizeof(message),
                "RandomTreasureDrop: item%u selected a droptable for this game.",
                static_cast<unsigned>(rule.number));
            selectionMessages.emplace_back(message);
        }
        GameActive.store(true, std::memory_order_release);
    }
    for (const auto& message : selectionMessages) {
        Context->LogInfo(message.c_str());
    }
    Context->LogInfo(
        "RandomTreasureDrop: selected one droptable per configured item group.");
    } catch (...) {
        GameActive.store(false, std::memory_order_release);
        if (Context != nullptr) {
            Context->LogError(
                "RandomTreasureDrop: failed to select rows at GameJoined; custom drops are disabled.");
        }
    }
}

void __cdecl OnGameLeft(
        const D2RL::PluginContext* context,
        const D2RL::Lifecycle::GameplayEvent* event,
        void*) noexcept {
    if (context == nullptr || context != Context
            || !D2RL::Lifecycle::HasGameplayEventField(event,
                D2RL::Lifecycle::GameplayEventRequiredSize)) {
        return;
    }
    if (EnableProbeLogging && Context != nullptr) {
        char summary[160]{};
        (void)std::snprintf(summary, sizeof(summary),
            "RandomTreasureDrop probe: GameLeft observed %u native hook calls; selected-table match observed=%s.",
            static_cast<unsigned>(NativeDropProbeCallCount.load(
                std::memory_order_acquire)),
            NativeDropProbeLogged.load(std::memory_order_acquire)
                ? "yes" : "no");
        Context->LogInfo(summary);
    }
    if constexpr (EnableDropOutcomeDiagnostics) {
        char summary[192]{};
        (void)std::snprintf(summary, sizeof(summary),
            "RandomTreasureDrop trace: game ended; callbacks=%u mapped=%u selected-matches=%u repeats-suppressed=%u.",
            static_cast<unsigned>(ObservedDropHookCount.load(
                std::memory_order_acquire)),
            static_cast<unsigned>(MappedDropHookCount.load(
                std::memory_order_acquire)),
            static_cast<unsigned>(MatchedDropCallbackCount.load(
                std::memory_order_acquire)),
            static_cast<unsigned>(SuppressedDuplicateDropCount.load(
                std::memory_order_acquire)));
        Context->LogInfo(summary);
    }
    GameActive.store(false, std::memory_order_release);
    NativeDropProbeLogged.store(false, std::memory_order_release);
    ObservedDropHookCount.store(0, std::memory_order_release);
    MappedDropHookCount.store(0, std::memory_order_release);
    MatchedDropCallbackCount.store(0, std::memory_order_release);
    SuppressedDuplicateDropCount.store(0, std::memory_order_release);
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        ActiveRules.clear();
        RecentDropEvents.fill(RecentDropEvent{});
        RecentDropEventNext = 0;
        RecentDropEventCount = 0;
    }
}

auto LoadServices() noexcept -> bool {
    if (Context->QueryService(D2RL::ServiceId::Lifecycle,
            D2RL::LifecycleServiceV1Version, &LifecycleService)
                != D2RL::ServiceQueryResult::Success
            || !D2RL::HasLifecycleServiceV1Field(LifecycleService,
                D2RL::LifecycleServiceV1RequiredSize)
            || Context->QueryService(D2RL::ServiceId::DataTable,
                D2RL::DataTableServiceV1Version, &DataTableService)
                != D2RL::ServiceQueryResult::Success
            || !D2RL::HasDataTableServiceV1Field(DataTableService,
                D2RL::DataTableServiceV1RequiredSize)) {
        Context->LogError(
            "RandomTreasureDrop: a required lifecycle or data-table service is unavailable.");
        return false;
    }
    return true;
}

auto RegisterListeners() noexcept -> bool {
    const D2RL::Lifecycle::DataTablesLoadedListener tablesListener{
        .structSize = D2RL::Lifecycle::DataTablesLoadedListenerSize,
        .callback = &OnDataTablesLoaded,
    };
    if (LifecycleService->registerDataTablesLoadedListener(Context,
            &tablesListener, &DataTablesListenerHandle)
            != D2RL::Lifecycle::Result::Success) {
        return false;
    }
    const D2RL::Lifecycle::GameplayEventListener joinedListener{
        .structSize = D2RL::Lifecycle::GameplayEventListenerSize,
        .kind = D2RL::Lifecycle::GameplayEventKind::GameJoined,
        .callback = &OnGameJoined,
    };
    if (LifecycleService->registerGameplayEventListener(Context,
            &joinedListener, &GameJoinedListenerHandle)
            != D2RL::Lifecycle::Result::Success) {
        return false;
    }
    const D2RL::Lifecycle::GameplayEventListener leftListener{
        .structSize = D2RL::Lifecycle::GameplayEventListenerSize,
        .kind = D2RL::Lifecycle::GameplayEventKind::GameLeft,
        .callback = &OnGameLeft,
    };
    return LifecycleService->registerGameplayEventListener(Context,
        &leftListener, &GameLeftListenerHandle)
        == D2RL::Lifecycle::Result::Success;
}

void Shutdown() noexcept {
    GameActive.store(false, std::memory_order_release);
    TableReady.store(false, std::memory_order_release);
    if (Context != nullptr && LifecycleService != nullptr) {
        if (DataTablesListenerHandle != D2RL::Lifecycle::InvalidHandle) {
            (void)LifecycleService->unregisterDataTablesLoadedListener(
                Context, DataTablesListenerHandle);
        }
        if (GameJoinedListenerHandle != D2RL::Lifecycle::InvalidHandle) {
            (void)LifecycleService->unregisterGameplayEventListener(
                Context, GameJoinedListenerHandle);
        }
        if (GameLeftListenerHandle != D2RL::Lifecycle::InvalidHandle) {
            (void)LifecycleService->unregisterGameplayEventListener(
                Context, GameLeftListenerHandle);
        }
    }
    DataTablesListenerHandle = D2RL::Lifecycle::InvalidHandle;
    GameJoinedListenerHandle = D2RL::Lifecycle::InvalidHandle;
    GameLeftListenerHandle = D2RL::Lifecycle::InvalidHandle;
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        ActiveRules.clear();
        RecentDropEvents.fill(RecentDropEvent{});
        RecentDropEventNext = 0;
        RecentDropEventCount = 0;
        Rules.clear();
        TreasureClasses.clear();
        TreasureClassByName.clear();
        TreasureClassByHash.clear();
    }
    RuntimeTableRevision.store(0, std::memory_order_release);
    OriginalDropTreasureClass = nullptr;
    GetDynamicPath = nullptr;
    PathGetX = nullptr;
    PathGetY = nullptr;
    GetUnitRoom = nullptr;
    CreateItemUnit = nullptr;
    ReturnItemToGround = nullptr;
    LifecycleService = nullptr;
    DataTableService = nullptr;
}

auto LoadConfiguredRules() noexcept -> bool {
    try {
    std::string configText;
    if (!ReadConfigText(configText)) {
        Context->LogError(
            "RandomTreasureDrop: the plugin config could not be read or created.");
        return false;
    }
    std::vector<Rule> parsed;
    std::string error;
    if (!ParseConfig(configText, parsed, error)) {
        const std::string message = "RandomTreasureDrop: invalid config: " + error;
        Context->LogError(message.c_str());
        return false;
    }
    if (parsed.empty()) {
        Context->LogInfo(
            "RandomTreasureDrop: no item groups are configured; no native hook was installed.");
        return true;
    }

    const auto roots = DataRoots(Context);
    std::vector<TreasureClassInfo> treasureRows;
    std::unordered_map<std::string, std::uint32_t> rowsByName;
    if (!ParseTreasureClasses(roots, treasureRows, rowsByName, error)) {
        const std::string message = "RandomTreasureDrop: " + error;
        Context->LogError(message.c_str());
        return false;
    }
    if (!ResolveDropTableSelectors(parsed, roots, treasureRows,
            rowsByName, error)) {
        const std::string message = "RandomTreasureDrop: " + error;
        Context->LogError(message.c_str());
        return false;
    }
    std::vector<SourceItem> items;
    std::vector<TypeEquivalence> types;
    if (!ReadItemSources(roots, items, types, error)
            || !ResolveRuleItems(parsed, items, types, error)) {
        const std::string message = "RandomTreasureDrop: " + error;
        Context->LogError(message.c_str());
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(StateMutex);
        Rules = std::move(parsed);
        TreasureClasses = std::move(treasureRows);
        TreasureClassByName = std::move(rowsByName);
    }
    return true;
    } catch (...) {
        if (Context != nullptr) {
            Context->LogError(
                "RandomTreasureDrop: failed while loading config or active mod data.");
        }
        return false;
    }
}

} // namespace

D2RL_PLUGIN_EXPORT auto D2RLoaderGetPluginInfo() noexcept
        -> const D2RL::PluginInfo* {
    return &Info;
}

D2RL_PLUGIN_EXPORT auto D2RLoaderLoadPlugin(
        const D2RL::PluginContext* context) noexcept -> bool {
    Shutdown();
    if (!D2RL::HasContext(context)
            || context->apiVersion != D2RL_PLUGIN_API_VERSION) {
        return false;
    }
    Context = context;
    try {
        std::random_device device;
        RandomEngine.seed((static_cast<std::uint64_t>(device()) << 32U)
            ^ device());
    } catch (...) {
        RandomEngine.seed(0xD2A11D0FULL);
    }
    if (!HasSupportedBuild(context)) {
        Context->LogError(
            "RandomTreasureDrop: this build supports only D2R 3.3.93847; plugin load was refused.");
        Shutdown();
        Context = nullptr;
        return false;
    }
    if (!LoadConfiguredRules()) {
        Shutdown();
        Context = nullptr;
        return false;
    }
    if (Rules.empty()) return true;
    if (!LoadServices() || !RegisterListeners() || !InstallDropHook()) {
        Shutdown();
        Context = nullptr;
        return false;
    }
    Context->LogInfo(
        "RandomTreasureDrop 0.1.40 diagnostic build: repeated same-source callbacks within 250 ms are suppressed; callback matches and bonus-roll outcomes are logged without naming rows. Zero path coordinates still fall back to the player's location.");
    return true;
}

D2RL_PLUGIN_EXPORT void D2RLoaderUnloadPlugin() noexcept {
    Shutdown();
    Context = nullptr;
}

