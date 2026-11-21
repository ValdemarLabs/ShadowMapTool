#include "objects/ShadowExclusions.hpp"

#include "util/FileIO.hpp"

#include <charconv>
#include <map>
#include <sstream>
#include <stdexcept>

namespace w3shadow {

ObjectDatabase loadMapObjectDatabase(const MapArchive& archive, const AssetProvider& assets)
{
    ObjectDatabase objects;
    objects.loadStock(assets);
    const auto read = [&](const char* name) -> std::optional<std::vector<std::byte>> {
        if (!archive.contains(name)) return std::nullopt;
        return archive.read(name);
    };
    // Do not generate from partially read overrides: they may contain Has shadow = False.
    objects.applyMapOverrides(read("war3map.w3d"), read("war3map.w3b"));
    objects.applyMapOverrides(read("war3mapSkin.w3d"), read("war3mapSkin.w3b"));
    return objects;
}

const ObjectDefinition* placementDefinition(
    const ObjectDatabase& objects, const DoodadPlacement& placement)
{
    const auto validSkin = placement.skinRawcode.size() == 4U &&
        placement.skinRawcode != std::string(4U, '\0');
    const auto* definition = validSkin ? objects.find(placement.skinRawcode) : nullptr;
    return definition ? definition : objects.find(placement.rawcode);
}

std::vector<ShadowObjectRow> shadowObjectRows(
    const MapArchive& archive, const ObjectDatabase& objects, const DOOLayoutMode layout)
{
    if (!archive.contains("war3map.doo")) return {};
    const auto placements = parseDOO(archive.read("war3map.doo"), layout);
    if (!placements) throw std::runtime_error(placements.error);
    std::map<std::string, std::string> names;
    if (archive.contains("war3map.wts")) {
        const auto bytes = archive.read("war3map.wts");
        std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        std::string line;
        std::string id;
        std::string value;
        bool inString = false;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (inString) {
                if (line == "}") { names[id] = value; inString = false; }
                else { if (!value.empty()) value += ' '; value += line; }
            } else if (line.starts_with("STRING ")) {
                id = line.substr(7U);
                value.clear();
            } else if (line == "{" && !id.empty()) inString = true;
        }
    }
    std::map<std::string, ShadowObjectRow> rows;
    for (const auto& placement : placements.placements) {
        auto& row = rows[placement.rawcode];
        row.rawcode = placement.rawcode;
        ++row.placements;
        const auto* definition = placementDefinition(objects, placement);
        if (!definition) { row.unresolved = true; continue; }
        if (row.name.empty()) row.name = definition->name;
        if (row.modelPath.empty()) row.modelPath = definition->modelPath;
        else if (row.modelPath != definition->modelPath) row.modelPath = "(multiple skins/models)";
        row.anyEnabled |= definition->castsShadow;
        row.anyDisabled |= !definition->castsShadow;
    }
    std::vector<ShadowObjectRow> result;
    for (auto& [id, row] : rows) {
        if (row.name.starts_with("TRIGSTR_")) {
            const auto digits = row.name.substr(8U);
            unsigned int index = 0;
            const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), index);
            if (parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size()) {
                if (const auto found = names.find(std::to_string(index)); found != names.end()) row.name = found->second;
            }
        }
        if (row.name.empty() && !row.modelPath.empty()) {
            const auto slash = row.modelPath.find_last_of("/\\");
            row.name = row.modelPath.substr(slash == std::string::npos ? 0U : slash + 1U);
        }
        result.push_back(std::move(row));
    }
    return result;
}

std::filesystem::path shadowExclusionsPath(const std::filesystem::path& map)
{
    auto path = map;
    path += ".w3shadow-exclusions";
    return path;
}

ShadowExclusions loadShadowExclusions(const std::filesystem::path& map)
{
    const auto path = shadowExclusionsPath(map);
    if (!std::filesystem::exists(path)) return {};
    const auto bytes = readBinaryFile(path, 1024U * 1024U);
    if (bytes.empty()) throw std::runtime_error("empty shadow exclusion file: " + path.string());
    std::istringstream input(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    std::string line;
    const auto next = [&] {
        if (!std::getline(input, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    if (!next() || line != "W3SHADOW-EXCLUSIONS 1") {
        throw std::runtime_error("invalid shadow exclusion file: " + path.string());
    }
    ShadowExclusions result;
    while (next()) {
        if (line.size() != 8U) throw std::runtime_error("invalid exclusion rawcode in " + path.string());
        std::string id;
        for (std::size_t i = 0; i < 8U; i += 2U) {
            unsigned int value = 0;
            const auto parsed = std::from_chars(line.data() + i, line.data() + i + 2U, value, 16);
            if (parsed.ec != std::errc{} || parsed.ptr != line.data() + i + 2U) {
                throw std::runtime_error("invalid exclusion rawcode in " + path.string());
            }
            id += static_cast<char>(value);
        }
        result.insert(id);
    }
    return result;
}

void saveShadowExclusions(const std::filesystem::path& map, const ShadowExclusions& exclusions)
{
    std::string text = "W3SHADOW-EXCLUSIONS 1\n";
    constexpr char hex[] = "0123456789abcdef";
    for (const auto& id : exclusions) {
        if (id.size() != 4U) throw std::invalid_argument("exclusion IDs must be four bytes");
        for (const unsigned char value : id) {
            text += hex[value >> 4U]; text += hex[value & 15U];
        }
        text += '\n';
    }
    writeBinaryFileAtomic(shadowExclusionsPath(map),
        {reinterpret_cast<const std::byte*>(text.data()), text.size()}, true);
}

} // namespace w3shadow
