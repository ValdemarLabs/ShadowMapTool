#include "terrain/TerrainAssets.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <optional>
#include <sstream>
#include <unordered_map>

namespace w3shadow {
namespace {

using Row = std::unordered_map<std::string, std::string>;

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
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
        value = value.substr(1U, value.size() - 2U);
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
        const auto fields = splitSlkLine(line);
        std::optional<std::string> value;
        bool hasX = false;
        bool hasY = false;
        for (std::size_t index = 1U; index < fields.size(); ++index) {
            const auto& field = fields[index];
            if (field.size() > 1U && field[0] == 'X') {
                std::from_chars(field.data() + 1U, field.data() + field.size(), currentX);
                hasX = true;
            } else if (field.size() > 1U && field[0] == 'Y') {
                std::from_chars(field.data() + 1U, field.data() + field.size(), currentY);
                hasY = true;
            } else if (!field.empty() && field[0] == 'K') {
                value = decodeSlkValue(field);
            }
        }
        if (hasY && !hasX) currentX = 1U;
        if (value && currentX != 0U && currentY != 0U) cells[currentY][currentX] = *value;
    }
    const auto header = cells.find(1U);
    if (header == cells.end()) return {};
    std::unordered_map<std::uint32_t, std::string> headers;
    for (const auto& [column, name] : header->second) headers[column] = lower(name);
    std::vector<Row> rows;
    for (const auto& [number, rowCells] : cells) {
        if (number == 1U) continue;
        Row row;
        for (const auto& [column, value] : rowCells) {
            if (const auto found = headers.find(column); found != headers.end()) {
                row[found->second] = value;
            }
        }
        if (!row.empty()) rows.push_back(std::move(row));
    }
    return rows;
}

std::string value(const Row& row, const std::string_view key)
{
    const auto found = row.find(std::string(key));
    return found == row.end() ? std::string{} : found->second;
}

std::uint32_t u32(const std::span<const std::byte> bytes, const std::size_t offset)
{
    if (offset + 4U > bytes.size()) return 0U;
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

bool allAlphaZero(const std::span<const std::byte> bytes, const std::size_t offset,
                  const std::size_t pixels, const unsigned alphaBits)
{
    const auto alphaBytes = (pixels * alphaBits + 7U) / 8U;
    if (offset > bytes.size() || alphaBytes > bytes.size() - offset) return false;
    for (std::size_t index = 0; index < alphaBytes; ++index) {
        if (std::to_integer<unsigned char>(bytes[offset + index]) != 0U) return false;
    }
    return alphaBytes != 0U;
}

bool allRawBgraAlphaZero(const std::span<const std::byte> bytes,
                         const std::size_t offset, const std::size_t pixels)
{
    if (pixels > (bytes.size() - std::min(offset, bytes.size())) / 4U) return false;
    for (std::size_t index = 0; index < pixels; ++index) {
        if (std::to_integer<unsigned char>(bytes[offset + index * 4U + 3U]) != 0U) return false;
    }
    return pixels != 0U;
}

bool allDxtAlphaZero(const std::span<const std::byte> bytes, const std::size_t offset,
                     const std::uint32_t width, const std::uint32_t height,
                     const unsigned alphaBits)
{
    const auto blocks = static_cast<std::size_t>((width + 3U) / 4U) * ((height + 3U) / 4U);
    const auto blockSize = alphaBits == 1U ? 8U : 16U;
    if (offset > bytes.size() || blocks > (bytes.size() - offset) / blockSize) return false;
    for (std::size_t block = 0; block < blocks; ++block) {
        const auto base = offset + block * blockSize;
        if (alphaBits == 1U) {
            const auto color0 = static_cast<unsigned>(std::to_integer<unsigned char>(bytes[base])) |
                                (static_cast<unsigned>(std::to_integer<unsigned char>(bytes[base + 1U])) << 8U);
            const auto color1 = static_cast<unsigned>(std::to_integer<unsigned char>(bytes[base + 2U])) |
                                (static_cast<unsigned>(std::to_integer<unsigned char>(bytes[base + 3U])) << 8U);
            if (color0 > color1) return false;
            const auto selectors = u32(bytes, base + 4U);
            if (selectors != 0xFFFFFFFFU) return false;
        } else if (alphaBits == 4U) {
            for (std::size_t index = 0; index < 8U; ++index) {
                if (bytes[base + index] != std::byte{0}) return false;
            }
        } else if (alphaBits == 8U) {
            // A fully transparent DXT5 block has zero alpha endpoints. Every
            // interpolated palette entry is then also zero, independent of indices.
            if (bytes[base] != std::byte{0} || bytes[base + 1U] != std::byte{0}) return false;
        } else {
            return false;
        }
    }
    return blocks != 0U;
}

// Alpha terrain imports used by Warcraft III are normally paletted BLP1 files.
// We intentionally require a completely transparent top mip: ordinary terrain
// textures that merely carry an alpha channel must remain valid receivers.
bool isFullyTransparentBlp(const std::span<const std::byte> bytes)
{
    if (bytes.size() < 156U) return false;
    const std::string magic(reinterpret_cast<const char*>(bytes.data()), 4U);
    if (magic == "BLP1") {
        const auto content = u32(bytes, 4U);
        const auto alphaBits = u32(bytes, 8U);
        const auto width = u32(bytes, 12U);
        const auto height = u32(bytes, 16U);
        const auto mipOffset = u32(bytes, 28U);
        const auto mipSize = u32(bytes, 92U);
        if ((alphaBits != 1U && alphaBits != 4U && alphaBits != 8U) ||
            width == 0U || height == 0U) return false;
        const auto pixels64 = static_cast<std::uint64_t>(width) * height;
        if (pixels64 > bytes.size()) return false;
        const auto pixels = static_cast<std::size_t>(pixels64);
        const auto alphaBytes = (pixels * static_cast<std::size_t>(alphaBits) + 7U) / 8U;
        if (content == 1U) {
            return allAlphaZero(bytes, static_cast<std::size_t>(mipOffset) + pixels,
                                pixels, static_cast<unsigned>(alphaBits));
        }
        if (content == 0U && mipSize >= alphaBytes) {
            return allAlphaZero(bytes,
                                static_cast<std::size_t>(mipOffset) + mipSize - alphaBytes,
                                pixels, static_cast<unsigned>(alphaBits));
        }
        return false;
    }
    if (magic == "BLP2") {
        const auto compression = std::to_integer<unsigned char>(bytes[8U]);
        const auto alphaBits = std::to_integer<unsigned char>(bytes[9U]);
        const auto width = u32(bytes, 12U);
        const auto height = u32(bytes, 16U);
        const auto mipOffset = u32(bytes, 20U);
        if ((alphaBits != 1U && alphaBits != 4U && alphaBits != 8U) ||
            width == 0U || height == 0U) return false;
        const auto pixels64 = static_cast<std::uint64_t>(width) * height;
        if (pixels64 > bytes.size()) return false;
        const auto pixels = static_cast<std::size_t>(pixels64);
        if (compression == 1U) {
            return allAlphaZero(bytes, static_cast<std::size_t>(mipOffset) + pixels,
                                pixels, alphaBits);
        }
        if (compression == 2U) {
            return allDxtAlphaZero(bytes, mipOffset, width, height, alphaBits);
        }
        if (compression == 3U) {
            return allRawBgraAlphaZero(bytes, mipOffset, pixels);
        }
        return false;
    }
    return false;
}

std::string texturePath(const Row& row)
{
    auto file = value(row, "file");
    auto directory = value(row, "dir");
    if (file.empty()) return {};
    if (file.find('\\') == std::string::npos && file.find('/') == std::string::npos &&
        !directory.empty()) file = directory + "\\" + file;
    const auto lowered = lower(file);
    if (!lowered.ends_with(".blp")) file += ".blp";
    return file;
}

} // namespace

bool TerrainReceiverMask::transparentAt(
    const W3EMap& terrain, const float worldX, const float worldY) const
{
    const auto index = terrain.groundTextureAt(worldX, worldY);
    return index < transparentTilesets.size() && transparentTilesets[index];
}

TerrainReceiverMask detectTransparentTerrain(
    const W3EMap& terrain, const AssetProvider& assets)
{
    TerrainReceiverMask result;
    result.transparentTilesets.assign(terrain.groundTilesets.size(), false);
    const auto slk = assets.load("TerrainArt\\Terrain.slk");
    if (!slk) {
        result.warnings.emplace_back(
            "TerrainArt\\Terrain.slk unavailable; alpha-terrain receiver detection was skipped");
        return result;
    }
    std::unordered_map<std::string, Row> definitions;
    for (const auto& row : parseSlk(*slk)) {
        auto id = value(row, "tileid");
        if (id.empty()) id = value(row, "id");
        if (id.size() == 4U) definitions[id] = row;
    }
    for (std::size_t index = 0; index < terrain.groundTilesets.size(); ++index) {
        const auto found = definitions.find(terrain.groundTilesets[index]);
        if (found == definitions.end()) continue;
        const auto path = texturePath(found->second);
        if (path.empty()) continue;
        const auto texture = assets.load(path);
        if (!texture) continue;
        if (isFullyTransparentBlp(*texture)) {
            result.transparentTilesets[index] = true;
            ++result.detectedTilesets;
        }
    }
    return result;
}

} // namespace w3shadow
