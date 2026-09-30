#include "formats/W3E.hpp"

#include "util/BinaryReader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace w3shadow {
namespace {

constexpr float tileSize = 128.0F;

bool checkedIdBytes(const std::uint32_t count, std::size_t& byteCount)
{
    if (count > 4096U) return false;
    const auto wide = static_cast<std::uint64_t>(count) * 4U;
    if (wide > std::numeric_limits<std::size_t>::max()) return false;
    byteCount = static_cast<std::size_t>(wide);
    return true;
}

W3EParseResult parseImpl(const std::span<const std::byte> bytes)
{
    BinaryReader reader(bytes, "W3E");
    if (reader.readTag("signature") != "W3E!") {
        throw std::runtime_error("W3E parse error: invalid signature");
    }

    W3EMap map;
    map.info.version = reader.readU32("format version");
    if (map.info.version != 11U && map.info.version != 12U) {
        throw std::runtime_error("W3E parse error: unsupported format version " +
                                 std::to_string(map.info.version));
    }
    map.info.tileset = static_cast<char>(reader.readU8("tileset"));
    const auto customTilesets = reader.readU32("custom-tileset flag");
    if (customTilesets > 1U) throw std::runtime_error("W3E parse error: invalid custom-tileset flag");

    std::size_t idBytes = 0;
    const auto groundCount = reader.readU32("ground tile count");
    if (!checkedIdBytes(groundCount, idBytes)) {
        throw std::runtime_error("W3E parse error: invalid ground tile count");
    }
    reader.skip(idBytes, "ground tile list");
    const auto cliffCount = reader.readU32("cliff tile count");
    if (!checkedIdBytes(cliffCount, idBytes)) {
        throw std::runtime_error("W3E parse error: invalid cliff tile count");
    }
    reader.skip(idBytes, "cliff tile list");

    map.info.vertexWidth = reader.readU32("terrain width");
    map.info.vertexHeight = reader.readU32("terrain height");
    if (map.info.vertexWidth < 2U || map.info.vertexHeight < 2U ||
        map.info.vertexWidth > 8193U || map.info.vertexHeight > 8193U) {
        throw std::runtime_error("W3E parse error: terrain dimensions are outside safety limits");
    }
    map.info.tileWidth = map.info.vertexWidth - 1U;
    map.info.tileHeight = map.info.vertexHeight - 1U;
    map.info.offsetX = reader.readF32("terrain X offset");
    map.info.offsetY = reader.readF32("terrain Y offset");
    if (!std::isfinite(map.info.offsetX) || !std::isfinite(map.info.offsetY)) {
        throw std::runtime_error("W3E parse error: non-finite terrain offset");
    }

    const auto vertexCount64 = static_cast<std::uint64_t>(map.info.vertexWidth) *
                               map.info.vertexHeight;
    if (vertexCount64 > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("W3E parse error: vertex count overflow");
    }
    map.vertices.reserve(static_cast<std::size_t>(vertexCount64));
    map.info.minimumHeight = std::numeric_limits<float>::infinity();
    map.info.maximumHeight = -std::numeric_limits<float>::infinity();

    for (std::uint64_t index = 0; index < vertexCount64; ++index) {
        W3EVertex vertex;
        const auto rawGround = static_cast<std::int16_t>(reader.readU16("ground height"));
        const auto rawWaterAndBoundary = reader.readU16("water height");
        std::uint16_t textureAndFlags = reader.readU8("texture and flags");
        if (map.info.version >= 12U) {
            textureAndFlags |= static_cast<std::uint16_t>(reader.readU8("extended flags")) << 8U;
            vertex.groundTexture = static_cast<std::uint8_t>(textureAndFlags & 0x3FU);
            vertex.flags = static_cast<std::uint16_t>(textureAndFlags & 0xFFC0U);
        } else {
            vertex.groundTexture = static_cast<std::uint8_t>(textureAndFlags & 0x0FU);
            vertex.flags = static_cast<std::uint16_t>(textureAndFlags & 0x00F0U);
        }
        const auto variations = reader.readU8("terrain variations");
        const auto cliffAndLayer = reader.readU8("cliff texture and layer");
        vertex.groundVariation = static_cast<std::uint8_t>(variations >> 3U);
        vertex.cliffVariation = static_cast<std::uint8_t>(variations & 0x07U);
        vertex.cliffTexture = static_cast<std::uint8_t>(cliffAndLayer >> 4U);
        vertex.cliffLayer = static_cast<std::uint8_t>(cliffAndLayer & 0x0FU);
        vertex.groundHeight = (static_cast<float>(rawGround) - 8192.0F +
                               (static_cast<float>(vertex.cliffLayer) - 2.0F) * 512.0F) / 4.0F;
        vertex.waterHeight = (static_cast<float>(rawWaterAndBoundary & 0x3FFFU) - 8192.0F) /
                             4.0F - 89.6F;
        if ((rawWaterAndBoundary & 0x4000U) != 0U) vertex.flags |= 0x4000U;
        map.info.minimumHeight = std::min(map.info.minimumHeight, vertex.groundHeight);
        map.info.maximumHeight = std::max(map.info.maximumHeight, vertex.groundHeight);
        map.vertices.push_back(vertex);
    }
    const auto info = map.info;
    return {info, std::move(map), {}};
}

} // namespace

const W3EVertex& W3EMap::vertex(const std::uint32_t x, const std::uint32_t y) const
{
    if (x >= info.vertexWidth || y >= info.vertexHeight) {
        throw std::out_of_range("terrain vertex coordinates are outside the map");
    }
    return vertices[static_cast<std::size_t>(y) * info.vertexWidth + x];
}

float W3EMap::sampleHeight(const float worldX, const float worldY) const
{
    const auto localX = std::clamp((worldX - info.offsetX) / tileSize,
                                   0.0F, static_cast<float>(info.tileWidth));
    const auto localY = std::clamp((worldY - info.offsetY) / tileSize,
                                   0.0F, static_cast<float>(info.tileHeight));
    const auto tileX = std::min(static_cast<std::uint32_t>(localX), info.tileWidth - 1U);
    const auto tileY = std::min(static_cast<std::uint32_t>(localY), info.tileHeight - 1U);
    const auto fractionX = localX - static_cast<float>(tileX);
    const auto fractionY = localY - static_cast<float>(tileY);
    const auto h00 = vertex(tileX, tileY).groundHeight;
    const auto h10 = vertex(tileX + 1U, tileY).groundHeight;
    const auto h01 = vertex(tileX, tileY + 1U).groundHeight;
    const auto h11 = vertex(tileX + 1U, tileY + 1U).groundHeight;
    if (((tileX + tileY) & 1U) == 0U) {
        if (fractionX >= fractionY) {
            return h00 * (1.0F - fractionX) + h10 * (fractionX - fractionY) +
                   h11 * fractionY;
        }
        return h00 * (1.0F - fractionY) + h11 * fractionX +
               h01 * (fractionY - fractionX);
    }
    if (fractionX + fractionY <= 1.0F) {
        return h00 * (1.0F - fractionX - fractionY) + h10 * fractionX +
               h01 * fractionY;
    }
    return h10 * (1.0F - fractionY) + h11 * (fractionX + fractionY - 1.0F) +
           h01 * (1.0F - fractionX);
}

std::vector<Triangle> W3EMap::terrainTriangles() const
{
    const auto triangleCount = static_cast<std::uint64_t>(info.tileWidth) * info.tileHeight * 2U;
    if (triangleCount > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("terrain triangle count overflow");
    }
    std::vector<Triangle> result;
    result.reserve(static_cast<std::size_t>(triangleCount));
    for (std::uint32_t y = 0; y < info.tileHeight; ++y) {
        for (std::uint32_t x = 0; x < info.tileWidth; ++x) {
            const auto wx = info.offsetX + static_cast<float>(x) * tileSize;
            const auto wy = info.offsetY + static_cast<float>(y) * tileSize;
            const Vec3 p00{wx, wy, vertex(x, y).groundHeight};
            const Vec3 p10{wx + tileSize, wy, vertex(x + 1U, y).groundHeight};
            const Vec3 p01{wx, wy + tileSize, vertex(x, y + 1U).groundHeight};
            const Vec3 p11{wx + tileSize, wy + tileSize, vertex(x + 1U, y + 1U).groundHeight};
            if (((x + y) & 1U) == 0U) {
                result.push_back({p00, p10, p11});
                result.push_back({p00, p11, p01});
            } else {
                result.push_back({p00, p10, p01});
                result.push_back({p10, p11, p01});
            }
        }
    }
    return result;
}

W3EParseResult parseW3E(const std::span<const std::byte> bytes)
{
    try {
        return parseImpl(bytes);
    } catch (const std::exception& error) {
        return {{}, {}, error.what()};
    }
}

} // namespace w3shadow
