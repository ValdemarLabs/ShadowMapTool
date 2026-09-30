#include "objects/ObjectDatabase.hpp"

#include "util/BinaryReader.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace w3shadow {
namespace {

using Row = std::unordered_map<std::string, std::string>;

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
    return value;
}

std::string trim(std::string value)
{
    const auto notSpace = [](const unsigned char character) {
        return !std::isspace(character);
    };
    const auto first = std::find_if(value.begin(), value.end(), notSpace);
    if (first == value.end()) return {};
    const auto last = std::find_if(value.rbegin(), value.rend(), notSpace).base();
    value = std::string(first, last);
    if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
        value = value.substr(1U, value.size() - 2U);
    }
    return value;
}

bool hasShadowValue(const std::string_view value)
{
    return !value.empty() && value != "_";
}

std::vector<std::string> splitSlkLine(const std::string_view line)
{
    std::vector<std::string> result;
    std::string field;
    bool quoted = false;
    for (const char value : line) {
        if (value == '"') quoted = !quoted;
        if (value == ';' && !quoted) {
            result.push_back(std::move(field));
            field.clear();
        } else {
            field.push_back(value);
        }
    }
    result.push_back(std::move(field));
    return result;
}

std::string decodeSlkValue(std::string value)
{
    if (!value.empty() && value.front() == 'K') value.erase(value.begin());
    if (value.size() >= 2U && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2U);
        std::size_t position = 0;
        while ((position = value.find("\"\"", position)) != std::string::npos) {
            value.replace(position, 2, "\"");
            ++position;
        }
    }
    return value;
}

std::vector<Row> parseSlk(const std::span<const std::byte> bytes)
{
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::unordered_map<std::uint32_t, std::unordered_map<std::uint32_t, std::string>> cells;
    std::istringstream input(text);
    std::string line;
    std::uint32_t currentX = 0;
    std::uint32_t currentY = 0;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 2U || line[0] != 'C' || line[1] != ';') continue;
        auto fields = splitSlkLine(line);
        std::optional<std::string> value;
        bool hasX = false;
        bool hasY = false;
        for (std::size_t index = 1; index < fields.size(); ++index) {
            const auto& field = fields[index];
            if (field.size() > 1U && field[0] == 'X') {
                std::from_chars(field.data() + 1, field.data() + field.size(), currentX);
                hasX = true;
            } else if (field.size() > 1U && field[0] == 'Y') {
                std::from_chars(field.data() + 1, field.data() + field.size(), currentY);
                hasY = true;
            } else if (!field.empty() && field[0] == 'K') {
                value = decodeSlkValue(field);
            }
        }
        if (hasY && !hasX) currentX = 1U;
        if (value && currentX != 0U && currentY != 0U) cells[currentY][currentX] = *value;
    }
    const auto headerIterator = cells.find(1U);
    if (headerIterator == cells.end()) throw std::runtime_error("SLK has no header row");
    std::unordered_map<std::uint32_t, std::string> headers;
    for (const auto& [column, name] : headerIterator->second) headers[column] = lower(name);
    std::vector<Row> rows;
    for (const auto& [rowNumber, cellsInRow] : cells) {
        if (rowNumber == 1U) continue;
        Row row;
        for (const auto& [column, value] : cellsInRow) {
            if (const auto header = headers.find(column); header != headers.end()) row[header->second] = value;
        }
        if (!row.empty()) rows.push_back(std::move(row));
    }
    return rows;
}

std::uint32_t positiveInteger(const Row& row, const std::string& key, const std::uint32_t fallback)
{
    const auto iterator = row.find(key);
    if (iterator == row.end()) return fallback;
    std::uint32_t value = fallback;
    const auto result = std::from_chars(iterator->second.data(),
                                        iterator->second.data() + iterator->second.size(), value);
    return result.ec == std::errc{} && value > 0U ? value : fallback;
}

std::string rowValue(const Row& row, const std::string& key)
{
    const auto iterator = row.find(key);
    return iterator == row.end() ? std::string{} : iterator->second;
}

bool hasModelExtension(const std::string& value)
{
    const auto lowered = lower(value);
    return lowered.ends_with(".mdl") || lowered.ends_with(".mdx");
}

} // namespace

void ObjectDatabase::loadStock(const AssetProvider& assets)
{
    if (const auto doodads = assets.load("Doodads\\Doodads.slk")) {
        loadSlk(*doodads, false);
    } else {
        warnings_.emplace_back("stock Doodads\\Doodads.slk could not be loaded");
    }
    if (const auto destructibles = assets.load("Units\\DestructableData.slk")) {
        loadSlk(*destructibles, true);
    } else {
        warnings_.emplace_back("stock Units\\DestructableData.slk could not be loaded");
    }
    if (const auto doodadSkins = assets.load("Doodads\\DoodadSkins.txt")) {
        loadProfile(*doodadSkins, false);
    } else if (const auto doodadSkin = assets.load("Doodads\\DoodadSkin.txt")) {
        loadProfile(*doodadSkin, false);
    } else {
        warnings_.emplace_back("stock Doodads\\DoodadSkins.txt could not be loaded");
    }
    if (const auto destructibleSkins = assets.load("Units\\DestructableSkin.txt")) {
        loadProfile(*destructibleSkins, true);
    } else {
        warnings_.emplace_back("stock Units\\DestructableSkin.txt could not be loaded");
    }
}

void ObjectDatabase::loadSlk(const std::span<const std::byte> bytes, const bool destructible)
{
    for (const auto& row : parseSlk(bytes)) {
        auto rawcode = rowValue(row, destructible ? "destructableid" : "doodid");
        if (rawcode.empty()) rawcode = rowValue(row, "id");
        if (rawcode.empty()) {
            const auto first = row.find("");
            if (first != row.end()) rawcode = first->second;
        }
        auto model = rowValue(row, "file");
        if (rawcode.size() != 4U || model.empty()) continue;
        const auto directory = rowValue(row, "dir");
        if (!directory.empty() && model.find('\\') == std::string::npos &&
            model.find('/') == std::string::npos) {
            model = directory + "\\" + model;
        }
        ObjectDefinition definition;
        definition.rawcode = rawcode;
        definition.modelPath = model;
        definition.variationCount = positiveInteger(row, "numvar", 1U);
        definition.shadowTexture = rowValue(row, "shadow");
        definition.castsShadow = hasShadowValue(definition.shadowTexture);
        definition.destructible = destructible;
        definitions_[rawcode] = std::move(definition);
    }
}

void ObjectDatabase::loadProfile(
    const std::span<const std::byte> bytes, const bool destructible)
{
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::istringstream input(text);
    ObjectDefinition definition;
    definition.destructible = destructible;
    const auto commit = [&] {
        if (definition.rawcode.size() == 4U && !definition.modelPath.empty()) {
            definitions_[definition.rawcode] = definition;
        }
    };

    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        line = trim(std::move(line));
        if (line.empty() || line[0] == ';' ||
            (line.size() >= 2U && line[0] == '/' && line[1] == '/')) {
            continue;
        }
        if (line.size() >= 3U && line.front() == '[' && line.back() == ']') {
            commit();
            definition = {};
            definition.rawcode = trim(line.substr(1U, line.size() - 2U));
            definition.destructible = destructible;
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string::npos || definition.rawcode.empty()) continue;
        const auto key = lower(trim(line.substr(0U, equals)));
        const auto value = trim(line.substr(equals + 1U));
        if (key == "file") {
            definition.modelPath = value;
        } else if (key == "numvar") {
            std::uint32_t parsed = 1U;
            const auto result = std::from_chars(
                value.data(), value.data() + value.size(), parsed);
            if (result.ec == std::errc{} && parsed > 0U) definition.variationCount = parsed;
        } else if (key == "shadow") {
            definition.shadowTexture = value;
            definition.castsShadow = hasShadowValue(value);
        }
    }
    commit();
}

void ObjectDatabase::applyMapOverrides(
    std::optional<std::vector<std::byte>> doodadData,
    std::optional<std::vector<std::byte>> destructibleData)
{
    if (doodadData) applyObjectFile(*doodadData, false);
    if (destructibleData) applyObjectFile(*destructibleData, true);
}

void ObjectDatabase::applyObjectFile(const std::span<const std::byte> bytes, const bool destructible)
{
    BinaryReader reader(bytes, destructible ? "W3B" : "W3D");
    const auto version = reader.readU32("version");
    if (version != 1U && version != 2U && version != 3U) {
        throw std::runtime_error("unsupported custom object-data version " + std::to_string(version));
    }
    for (int table = 0; table < 2; ++table) {
        const auto count = reader.readU32("object count");
        if (count > 1000000U) throw std::runtime_error("custom object count exceeds safety limit");
        for (std::uint32_t objectIndex = 0; objectIndex < count; ++objectIndex) {
            const auto baseId = reader.readTag("base rawcode");
            const auto newId = reader.readTag("new rawcode");
            const auto targetId = newId == std::string(4, '\0') ? baseId : newId;
            ObjectDefinition definition;
            if (const auto existing = definitions_.find(baseId); existing != definitions_.end()) {
                definition = existing->second;
            }
            definition.rawcode = targetId;
            definition.destructible = destructible;
            if (version >= 3U) {
                static_cast<void>(reader.readU32("object data version"));
                static_cast<void>(reader.readU32("object data flags"));
            }
            const auto modificationCount = reader.readU32("modification count");
            if (modificationCount > 1000000U) throw std::runtime_error("modification count exceeds safety limit");
            for (std::uint32_t modification = 0; modification < modificationCount; ++modification) {
                const auto field = reader.readTag("field rawcode");
                const auto type = reader.readU32("value type");
                if (!destructible) {
                    static_cast<void>(reader.readU32("variation"));
                    static_cast<void>(reader.readU32("data pointer"));
                }
                std::optional<std::string> stringValue;
                if (type == 0U) {
                    const auto value = reader.readI32("integer value");
                    if (field == "dvar" || field == "bvar") definition.variationCount = static_cast<std::uint32_t>(std::max(value, 1));
                } else if (type == 1U || type == 2U) {
                    static_cast<void>(reader.readF32("real value"));
                } else if (type == 3U) {
                    stringValue = reader.readCString("string value");
                } else {
                    throw std::runtime_error("unknown custom object value type");
                }
                static_cast<void>(reader.readTag("modification terminator"));
                if (stringValue && (field == "dfil" || field == "bfil" || hasModelExtension(*stringValue))) {
                    definition.modelPath = *stringValue;
                }
                if (stringValue && (field == "dshd" || field == "bshd")) {
                    definition.shadowTexture = *stringValue;
                    definition.castsShadow = hasShadowValue(*stringValue);
                }
            }
            if (!definition.modelPath.empty()) definitions_[targetId] = std::move(definition);
        }
    }
}

const ObjectDefinition* ObjectDatabase::find(const std::string_view rawcode) const
{
    const auto iterator = definitions_.find(std::string(rawcode));
    return iterator == definitions_.end() ? nullptr : &iterator->second;
}

std::vector<std::string> modelPathCandidates(
    const ObjectDefinition& definition, const std::uint32_t variation)
{
    auto base = normalizeAssetPath(definition.modelPath);
    if (base.empty()) return {};
    const auto lowered = lower(base);
    if (lowered.ends_with(".mdl")) base.replace(base.size() - 4U, 4U, ".mdx");
    else if (!lowered.ends_with(".mdx")) base += ".mdx";
    std::vector<std::string> result;
    const auto dot = base.rfind('.');
    const auto suffix = std::to_string(definition.variationCount == 0U ? 0U : variation % definition.variationCount);
    if (definition.variationCount > 1U && dot != std::string::npos) {
        auto varied = base.substr(0, dot);
        if (!varied.empty() && std::isdigit(static_cast<unsigned char>(varied.back()))) {
            result.push_back(varied + suffix + base.substr(dot));
            varied.pop_back();
        }
        varied += suffix + base.substr(dot);
        if (std::find(result.begin(), result.end(), varied) == result.end()) {
            result.push_back(std::move(varied));
        }
    }
    if (std::find(result.begin(), result.end(), base) == result.end()) {
        result.push_back(std::move(base));
    }
    return result;
}

} // namespace w3shadow
