#pragma once

#include "geometry/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace w3shadow {

enum class TerrainGeometryMode {
    ClassicTriangulated,
    SmoothSubTile
};

struct W3EInfo {
    std::uint32_t version = 0;
    char tileset = 0;
    std::uint32_t vertexWidth = 0;
    std::uint32_t vertexHeight = 0;
    std::uint32_t tileWidth = 0;
    std::uint32_t tileHeight = 0;
    float offsetX = 0.0F;
    float offsetY = 0.0F;
    float minimumHeight = 0.0F;
    float maximumHeight = 0.0F;
};

struct W3EVertex {
    float groundHeight = 0.0F;
    float waterHeight = 0.0F;
    std::uint16_t flags = 0;
    std::uint8_t groundTexture = 0;
    std::uint8_t groundVariation = 0;
    std::uint8_t cliffVariation = 0;
    std::uint8_t cliffTexture = 0;
    std::uint8_t cliffLayer = 0;
};

class W3EMap {
public:
    W3EInfo info;
    std::vector<std::string> groundTilesets;
    std::vector<std::string> cliffTilesets;
    std::vector<W3EVertex> vertices;

    [[nodiscard]] const W3EVertex& vertex(std::uint32_t x, std::uint32_t y) const;
    [[nodiscard]] float sampleHeight(
        float worldX, float worldY,
        TerrainGeometryMode mode = TerrainGeometryMode::ClassicTriangulated) const;
    [[nodiscard]] std::vector<Triangle> terrainTriangles(
        TerrainGeometryMode mode = TerrainGeometryMode::ClassicTriangulated) const;
    [[nodiscard]] std::vector<Triangle> cliffWallTriangles() const;
    [[nodiscard]] std::uint8_t groundTextureAt(float worldX, float worldY) const;
};

struct W3EParseResult {
    W3EInfo info;
    W3EMap map;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] W3EParseResult parseW3E(std::span<const std::byte> bytes);

} // namespace w3shadow
