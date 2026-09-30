#include "shadow/ShadowGenerator.hpp"

#include "formats/DOO.hpp"
#include "formats/MDX.hpp"
#include "formats/W3R.hpp"
#include "formats/W3E.hpp"
#include "geometry/BVH.hpp"
#include "objects/ObjectDatabase.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace w3shadow {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed(const Clock::time_point start, const Clock::time_point end)
{
    return std::chrono::duration<double>(end - start).count();
}

std::optional<std::vector<std::byte>> optionalArchiveFile(
    const MapArchive& archive, const std::string_view path)
{
    return archive.contains(path) ? std::optional(archive.read(path)) : std::nullopt;
}

} // namespace

GenerationResult generateShadowMap(
    const MapArchive& archive, const AssetProvider& assets,
    const GenerationOptions& options)
{
    if (options.shadowSampleGrid != 1U && options.shadowSampleGrid != 2U &&
        options.shadowSampleGrid != 4U) {
        throw std::invalid_argument("shadow sample grid must be 1, 2, or 4");
    }
    const auto start = Clock::now();
    const auto parsedTerrain = parseW3E(archive.read("war3map.w3e"));
    if (!parsedTerrain) throw std::runtime_error(parsedTerrain.error);
    const auto& terrain = parsedTerrain.map;
    GenerationResult result{ShadowMap(terrain.info.tileWidth, terrain.info.tileHeight)};
    result.stats.mapWidth = terrain.info.tileWidth;
    result.stats.mapHeight = terrain.info.tileHeight;
    result.stats.shadowSampleGrid = options.shadowSampleGrid;

    if (options.terrain) {
        result.sceneTriangles = terrain.terrainTriangles(options.terrainGeometry);
    }

    std::vector<MapRegion> ignoreShadowRegions;
    if (options.honorIgnoreShadowRegions && archive.contains("war3map.w3r")) {
        const auto parsedRegions = parseW3R(archive.read("war3map.w3r"));
        if (!parsedRegions) {
            result.warnings.push_back(parsedRegions.error);
        } else {
            for (const auto& region : parsedRegions.regions) {
                if (isIgnoreShadowRegion(region.name)) ignoreShadowRegions.push_back(region);
            }
        }
    }
    result.stats.ignoredRegions = ignoreShadowRegions.size();

    if ((options.doodads || options.destructibles) && archive.contains("war3map.doo")) {
        const auto placements = parseDOO(archive.read("war3map.doo"));
        if (!placements) throw std::runtime_error(placements.error);
        result.stats.placements = placements.placements.size();

        ObjectDatabase objects;
        objects.loadStock(assets);
        try {
            objects.applyMapOverrides(optionalArchiveFile(archive, "war3map.w3d"),
                                      optionalArchiveFile(archive, "war3map.w3b"));
            objects.applyMapOverrides(optionalArchiveFile(archive, "war3mapSkin.w3d"),
                                      optionalArchiveFile(archive, "war3mapSkin.w3b"));
        } catch (const std::exception& error) {
            result.warnings.push_back(std::string("custom object data: ") + error.what());
        }
        result.warnings.insert(result.warnings.end(), objects.warnings().begin(), objects.warnings().end());

        std::unordered_map<std::string, MDXModel> modelCache;
        std::unordered_map<std::string, std::uint64_t> unresolvedWarnings;
        for (const auto& placement : placements.placements) {
            if (placement.life == 0U) continue;
            const auto validSkin = placement.skinRawcode.size() == 4U &&
                placement.skinRawcode != std::string(4U, '\0');
            const auto* definition = validSkin ? objects.find(placement.skinRawcode) : nullptr;
            if (definition == nullptr) definition = objects.find(placement.rawcode);
            if (definition == nullptr) {
                ++result.stats.unresolvedPlacements;
                ++unresolvedWarnings["unresolved object " + placement.rawcode];
                continue;
            }
            if ((definition->destructible && !options.destructibles) ||
                (!definition->destructible && !options.doodads)) continue;
            if (!definition->castsShadow) continue;

            const MDXModel* model = nullptr;
            std::string selectedPath;
            for (const auto& path : modelPathCandidates(*definition, placement.variation)) {
                if (const auto cached = modelCache.find(path); cached != modelCache.end()) {
                    model = &cached->second;
                    selectedPath = path;
                    break;
                }
                const auto bytes = assets.load(path);
                if (!bytes) continue;
                const auto parsed = parseMDX(*bytes);
                if (!parsed) {
                    result.warnings.push_back(path + ": " + parsed.error);
                    continue;
                }
                const auto [iterator, inserted] = modelCache.emplace(path, parsed.model);
                static_cast<void>(inserted);
                model = &iterator->second;
                selectedPath = path;
                break;
            }
            if (model == nullptr) {
                ++result.stats.unresolvedPlacements;
                ++unresolvedWarnings["model not found for " + placement.rawcode +
                                     " (" + definition->modelPath + ")"];
                continue;
            }
            auto transformed = transformTriangles(model->triangles, placement.position,
                                                   placement.rotation, placement.scale,
                                                   placement.roll, placement.pitch);
            result.sceneTriangles.insert(result.sceneTriangles.end(), transformed.begin(), transformed.end());
            ++result.stats.resolvedPlacements;
        }
        for (const auto& [warning, count] : unresolvedWarnings) {
            result.warnings.push_back(warning + " (" + std::to_string(count) + " placements)");
        }
        result.stats.uniqueModels = modelCache.size();
        if (result.stats.resolvedPlacements == 0U && result.stats.unresolvedPlacements > 0U) {
            throw std::runtime_error(
                "no placed-object models could be resolved; verify that CascLib.dll is beside the "
                "executable and Warcraft III is installed, pass --war3-dir/--casc-lib, or provide "
                "extracted assets with --asset-dir");
        }
    }
    result.stats.triangles = result.sceneTriangles.size();
    const auto loaded = Clock::now();
    result.stats.loadSeconds = elapsed(start, loaded);

    Bvh scene(result.sceneTriangles);
    const auto built = Clock::now();
    result.stats.bvhSeconds = elapsed(loaded, built);
    const Vec3 rayDirection = normalized(options.lightDirection * -1.0F);
    if (dot(rayDirection, rayDirection) == 0.0F || rayDirection.z <= 0.0F) {
        throw std::invalid_argument("light direction must be finite and point toward the terrain (negative Z)");
    }

    const auto workerCount = options.threadCount == 0U
        ? std::max(1U, std::thread::hardware_concurrency())
        : options.threadCount;
    std::atomic<std::uint32_t> nextRow{0};
    const auto samplesPerPixel = options.shadowSampleGrid * options.shadowSampleGrid;
    const auto requiredOccluded = samplesPerPixel / 2U + 1U;
    std::atomic<std::uint64_t> shadowed{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (std::uint32_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&] {
            std::uint64_t localShadowed = 0;
            for (;;) {
                const auto y = nextRow.fetch_add(1U, std::memory_order_relaxed);
                if (y >= result.shadow.heightPixels()) break;
                for (std::uint32_t x = 0; x < result.shadow.widthPixels(); ++x) {
                    std::uint32_t occludedSamples = 0;
                    for (std::uint32_t sampleY = 0; sampleY < options.shadowSampleGrid; ++sampleY) {
                        const auto fractionY =
                            (static_cast<float>(sampleY) + 0.5F) /
                            static_cast<float>(options.shadowSampleGrid);
                        const auto worldY = terrain.info.offsetY +
                            static_cast<float>(terrain.info.tileHeight) * 128.0F -
                            (static_cast<float>(y) + fractionY) * 32.0F;
                        for (std::uint32_t sampleX = 0;
                             sampleX < options.shadowSampleGrid; ++sampleX) {
                            const auto fractionX =
                                (static_cast<float>(sampleX) + 0.5F) /
                                static_cast<float>(options.shadowSampleGrid);
                            const auto worldX = terrain.info.offsetX +
                                (static_cast<float>(x) + fractionX) * 32.0F;
                            const auto ignored = std::any_of(
                                ignoreShadowRegions.begin(), ignoreShadowRegions.end(),
                                [&](const MapRegion& region) {
                                    return region.contains(worldX, worldY);
                                });
                            if (ignored) continue;
                            const auto height =
                                terrain.sampleHeight(worldX, worldY, options.terrainGeometry);
                            const Vec3 origin{
                                worldX, worldY, height + options.rayOriginOffset};
                            if (scene.intersects(origin, rayDirection,
                                                 options.rayOriginOffset * 0.25F)) {
                                ++occludedSamples;
                            }
                        }
                    }
                    const auto occluded = occludedSamples >= requiredOccluded;
                    result.shadow.set(x, y, occluded);
                    if (occluded) ++localShadowed;
                }
            }
            shadowed.fetch_add(localShadowed, std::memory_order_relaxed);
        });
    }
    for (auto& worker : workers) worker.join();
    const auto finished = Clock::now();
    result.stats.raySeconds = elapsed(built, finished);
    result.stats.rays = static_cast<std::uint64_t>(result.shadow.widthPixels()) *
                        result.shadow.heightPixels() * samplesPerPixel;
    result.stats.shadowedSamples = shadowed.load();
    return result;
}

std::vector<std::byte> exportObj(const std::span<const Triangle> triangles)
{
    std::ostringstream output;
    output << "# ShadowMapTool scene\n";
    for (const auto& triangle : triangles) {
        output << "v " << triangle.a.x << ' ' << triangle.a.y << ' ' << triangle.a.z << '\n'
               << "v " << triangle.b.x << ' ' << triangle.b.y << ' ' << triangle.b.z << '\n'
               << "v " << triangle.c.x << ' ' << triangle.c.y << ' ' << triangle.c.z << '\n';
    }
    for (std::size_t index = 0; index < triangles.size(); ++index) {
        const auto first = index * 3U + 1U;
        output << "f " << first << ' ' << first + 1U << ' ' << first + 2U << '\n';
    }
    const auto text = output.str();
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                  reinterpret_cast<const std::byte*>(text.data() + text.size()));
}

} // namespace w3shadow
