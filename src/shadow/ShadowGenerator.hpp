#pragma once

#include "archive/MapArchive.hpp"
#include "assets/AssetProvider.hpp"
#include "geometry/Geometry.hpp"
#include "shadow/ShadowMap.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace w3shadow {

struct GenerationOptions {
    Vec3 lightDirection{1.0F, 1.0F, -1.0F};
    std::uint32_t threadCount = 0;
    float rayOriginOffset = 0.5F;
    bool terrain = true;
    bool doodads = true;
    bool destructibles = true;
    bool honorIgnoreShadowRegions = true;
};

struct GenerationStats {
    std::uint32_t mapWidth = 0;
    std::uint32_t mapHeight = 0;
    std::size_t placements = 0;
    std::size_t resolvedPlacements = 0;
    std::size_t unresolvedPlacements = 0;
    std::size_t uniqueModels = 0;
    std::size_t triangles = 0;
    std::size_t ignoredRegions = 0;
    std::uint64_t rays = 0;
    std::uint64_t shadowedSamples = 0;
    double loadSeconds = 0.0;
    double bvhSeconds = 0.0;
    double raySeconds = 0.0;
};

struct GenerationResult {
    ShadowMap shadow;
    GenerationStats stats;
    std::vector<std::string> warnings;
    std::vector<Triangle> sceneTriangles;
};

[[nodiscard]] GenerationResult generateShadowMap(
    const MapArchive& archive, const AssetProvider& assets,
    const GenerationOptions& options = {});

[[nodiscard]] std::vector<std::byte> exportObj(
    std::span<const Triangle> triangles);

} // namespace w3shadow
