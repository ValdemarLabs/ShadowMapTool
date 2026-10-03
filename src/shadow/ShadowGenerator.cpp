#include "shadow/ShadowGenerator.hpp"

#include "formats/DOO.hpp"
#include "formats/BLP.hpp"
#include "formats/MDX.hpp"
#include "formats/W3R.hpp"
#include "formats/W3E.hpp"
#include "geometry/BVH.hpp"
#include "objects/ObjectDatabase.hpp"
#include "terrain/TerrainAssets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool differsFromOne(const float value)
{
    return std::abs(value - 1.0F) > 0.0001F;
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
    if (!std::isfinite(options.rayOriginOffset) || options.rayOriginOffset < 0.0F ||
        options.rayOriginOffset > 32.0F) {
        throw std::invalid_argument("ray origin offset must be from 0 to 32");
    }
    if (options.gaussianRadius > 3U) {
        throw std::invalid_argument("Gaussian radius must be from 0 to 3");
    }
    if (!std::isfinite(options.coverageThreshold) || options.coverageThreshold < 0.0F ||
        options.coverageThreshold > 1.0F) {
        throw std::invalid_argument("coverage threshold must be from 0 to 1");
    }
    if (options.minimumShadowIslandPixels > 64U) {
        throw std::invalid_argument("minimum shadow island size must be from 0 to 64");
    }
    if (!std::isfinite(options.maximumCasterHorizontalSpan) ||
        options.maximumCasterHorizontalSpan < 0.0F ||
        options.maximumCasterHorizontalSpan > 131072.0F) {
        throw std::invalid_argument("maximum caster span must be from 0 to 131072");
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
        if (options.cliffWalls) {
            auto walls = terrain.cliffWallTriangles();
            result.stats.cliffWallTriangles = walls.size();
            result.sceneTriangles.insert(result.sceneTriangles.end(), walls.begin(), walls.end());
        }
    }

    TerrainReceiverMask receiverMask;
    if (options.ignoreTransparentTerrain) {
        receiverMask = detectTransparentTerrain(terrain, assets);
        result.stats.transparentTerrainTypes = receiverMask.detectedTilesets;
        result.warnings.insert(result.warnings.end(), receiverMask.warnings.begin(),
                               receiverMask.warnings.end());
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
        std::unordered_map<std::string, std::shared_ptr<const AlphaMask>> alphaTextureCache;
        std::unordered_set<std::string> alphaTextureWarnings;
        std::unordered_map<std::string, std::uint64_t> unresolvedWarnings;
        std::unordered_map<std::string, std::uint64_t> oversizedCasterWarnings;
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
                auto parsed = parseMDX(*bytes);
                if (!parsed) {
                    result.warnings.push_back(path + ": " + parsed.error);
                    continue;
                }
                for (const auto& warning : parsed.warnings) {
                    result.warnings.push_back(path + ": " + warning);
                }
                for (auto& triangle : parsed.model.triangles) {
                    if (!triangle.alphaTested ||
                        triangle.shadowTextureIndex >= parsed.model.textures.size()) continue;
                    auto texturePath = normalizeAssetPath(
                        parsed.model.textures[triangle.shadowTextureIndex].path);
                    if (texturePath.empty()) continue;
                    const auto lowered = lower(texturePath);
                    if (!lowered.ends_with(".blp")) {
                        const auto dot = texturePath.find_last_of('.');
                        if (dot != std::string::npos) texturePath.erase(dot);
                        texturePath += ".blp";
                    }
                    const auto cachedTexture = alphaTextureCache.find(texturePath);
                    if (cachedTexture != alphaTextureCache.end()) {
                        triangle.alphaMask = cachedTexture->second;
                        continue;
                    }
                    std::shared_ptr<const AlphaMask> mask;
                    const auto textureBytes = assets.load(texturePath);
                    if (textureBytes) {
                        auto decoded = decodeBlpAlpha(*textureBytes);
                        if (decoded) {
                            mask = std::move(decoded.mask);
                            ++result.stats.alphaTexturesLoaded;
                        } else if (alphaTextureWarnings.insert(texturePath).second) {
                            result.warnings.push_back(texturePath +
                                ": alpha-mask decode failed (" + decoded.error +
                                "); using opaque fallback");
                        }
                    } else if (alphaTextureWarnings.insert(texturePath).second) {
                        result.warnings.push_back(texturePath +
                            ": alpha-tested material texture was not found; using opaque fallback");
                    }
                    if (!mask) ++result.stats.alphaTexturesUnavailable;
                    alphaTextureCache.emplace(texturePath, mask);
                    triangle.alphaMask = std::move(mask);
                }
                const auto [iterator, inserted] = modelCache.emplace(path, std::move(parsed.model));
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
            result.stats.materialFilteredTriangles += model->materialFilteredTriangles;
            result.stats.alphaTestedTriangles += model->alphaTestedTriangles;
            auto transformed = transformTriangles(model->triangles, placement.position,
                                                   placement.rotation, placement.scale,
                                                   placement.roll, placement.pitch);
            if (result.stats.resolvedPlacements == 0U) {
                result.stats.minimumPlacementScale = placement.scale;
                result.stats.maximumPlacementScale = placement.scale;
            } else {
                result.stats.minimumPlacementScale.x = std::min(
                    result.stats.minimumPlacementScale.x, placement.scale.x);
                result.stats.minimumPlacementScale.y = std::min(
                    result.stats.minimumPlacementScale.y, placement.scale.y);
                result.stats.minimumPlacementScale.z = std::min(
                    result.stats.minimumPlacementScale.z, placement.scale.z);
                result.stats.maximumPlacementScale.x = std::max(
                    result.stats.maximumPlacementScale.x, placement.scale.x);
                result.stats.maximumPlacementScale.y = std::max(
                    result.stats.maximumPlacementScale.y, placement.scale.y);
                result.stats.maximumPlacementScale.z = std::max(
                    result.stats.maximumPlacementScale.z, placement.scale.z);
            }
            bool oversizedCaster = false;
            if (differsFromOne(placement.scale.x) || differsFromOne(placement.scale.y) ||
                differsFromOne(placement.scale.z)) {
                ++result.stats.nonUnitScalePlacements;
            }
            if (!transformed.empty()) {
                Aabb casterBounds;
                for (const auto& triangle : transformed) casterBounds.expand(bounds(triangle));
                const auto casterExtent = casterBounds.extent();
                const auto casterDiagonalSquared = dot(casterExtent, casterExtent);
                if (casterDiagonalSquared > dot(result.stats.largestCasterExtent,
                                                 result.stats.largestCasterExtent)) {
                    result.stats.largestCasterExtent = casterExtent;
                    result.stats.largestCasterScale = placement.scale;
                    result.stats.largestCasterRawcode = placement.rawcode;
                    result.stats.largestCasterModelPath = selectedPath;
                }
                const auto horizontalSpan = std::max(casterExtent.x, casterExtent.y);
                if (options.maximumCasterHorizontalSpan > 0.0F &&
                    horizontalSpan > options.maximumCasterHorizontalSpan) {
                    oversizedCaster = true;
                    ++result.stats.oversizedCasterPlacements;
                    std::ostringstream warning;
                    warning << "oversized caster " << placement.rawcode << " [" << selectedPath
                            << "] scale (" << placement.scale.x << ',' << placement.scale.y << ','
                            << placement.scale.z << ") position (" << placement.position.x << ','
                            << placement.position.y << ',' << placement.position.z << ") editor ID "
                            << placement.editorId << " horizontal span " << horizontalSpan
                            << " exceeds " << options.maximumCasterHorizontalSpan
                            << "; excluded (set maximum caster span to 0 to allow it)";
                    ++oversizedCasterWarnings[warning.str()];
                }
            }
            if (!oversizedCaster) {
                result.sceneTriangles.insert(result.sceneTriangles.end(), transformed.begin(),
                                             transformed.end());
            }
            ++result.stats.resolvedPlacements;
        }
        for (const auto& [warning, count] : unresolvedWarnings) {
            result.warnings.push_back(warning + " (" + std::to_string(count) + " placements)");
        }
        for (const auto& [warning, count] : oversizedCasterWarnings) {
            result.warnings.push_back(warning + " (" + std::to_string(count) +
                                      (count == 1U ? " placement)" : " placements)"));
        }
        result.stats.uniqueModels = modelCache.size();
        if (result.stats.resolvedPlacements == 0U && result.stats.unresolvedPlacements > 0U) {
            throw std::runtime_error(
                "no placed-object models could be resolved; verify the selected Warcraft III "
                "CASC or classic MPQ installation, pass --war3-dir/--casc-lib, or provide "
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
    const auto shadowWidth = result.shadow.widthPixels();
    const auto shadowHeight = result.shadow.heightPixels();
    const auto pixelCount = static_cast<std::size_t>(shadowWidth) * shadowHeight;
    std::vector<std::uint8_t> coverageSamples(pixelCount, 0U);
    std::vector<std::uint8_t> receiverAllowed(pixelCount, 1U);
    std::atomic<std::uint64_t> partialCoverage{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (std::uint32_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&] {
            std::uint64_t localPartialCoverage = 0;
            for (;;) {
                const auto y = nextRow.fetch_add(1U, std::memory_order_relaxed);
                if (y >= result.shadow.heightPixels()) break;
                for (std::uint32_t x = 0; x < result.shadow.widthPixels(); ++x) {
                    std::uint32_t occludedSamples = 0;
                    std::uint32_t eligibleSamples = 0;
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
                            if (options.ignoreTransparentTerrain &&
                                receiverMask.transparentAt(terrain, worldX, worldY)) continue;
                            ++eligibleSamples;
                            const auto height =
                                terrain.sampleHeight(worldX, worldY, options.terrainGeometry);
                            constexpr float normalStep = 4.0F;
                            const auto heightLeft = terrain.sampleHeight(
                                worldX - normalStep, worldY, options.terrainGeometry);
                            const auto heightRight = terrain.sampleHeight(
                                worldX + normalStep, worldY, options.terrainGeometry);
                            const auto heightBottom = terrain.sampleHeight(
                                worldX, worldY - normalStep, options.terrainGeometry);
                            const auto heightTop = terrain.sampleHeight(
                                worldX, worldY + normalStep, options.terrainGeometry);
                            const auto surfaceNormal = normalized(Vec3{
                                heightLeft - heightRight, heightBottom - heightTop,
                                normalStep * 2.0F});
                            const Vec3 surface{worldX, worldY, height};
                            const Vec3 origin = surface + surfaceNormal * options.rayOriginOffset +
                                                rayDirection * (options.rayOriginOffset * 0.25F);
                            if (scene.intersects(origin, rayDirection,
                                                 std::max(0.05F, options.rayOriginOffset * 0.25F))) {
                                ++occludedSamples;
                            }
                        }
                    }
                    if (occludedSamples != 0U && occludedSamples != samplesPerPixel) {
                        ++localPartialCoverage;
                    }
                    const auto index = static_cast<std::size_t>(y) * shadowWidth + x;
                    coverageSamples[index] = static_cast<std::uint8_t>(occludedSamples);
                    receiverAllowed[index] = eligibleSamples == 0U ? 0U : 1U;
                }
            }
            partialCoverage.fetch_add(localPartialCoverage, std::memory_order_relaxed);
        });
    }
    for (auto& worker : workers) worker.join();

    const auto rawCoverage = [&](const std::uint32_t x, const std::uint32_t y) {
        return static_cast<float>(coverageSamples[static_cast<std::size_t>(y) * shadowWidth + x]) /
               static_cast<float>(samplesPerPixel);
    };
    std::vector<float> gaussianKernel(options.gaussianRadius * 2U + 1U, 1.0F);
    if (options.gaussianRadius > 0U) {
        const auto order = options.gaussianRadius * 2U;
        for (std::uint32_t index = 1U; index <= order; ++index) {
            gaussianKernel[index] = gaussianKernel[index - 1U] *
                static_cast<float>(order - index + 1U) / static_cast<float>(index);
        }
    }

    // Label disconnected raw shadow islands before filtering. The Gaussian
    // stage uses the strongest component at each output cell instead of adding
    // coverage from separate nearby casters, preventing blur-only bridges.
    std::vector<std::uint32_t> coverageComponents(pixelCount, 0U);
    if (options.coverageMode == ShadowCoverageMode::CoherentFilter &&
        options.gaussianRadius > 0U) {
        std::vector<std::size_t> pending;
        pending.reserve(128U);
        std::uint32_t nextComponent = 1U;
        for (std::size_t startIndex = 0; startIndex < pixelCount; ++startIndex) {
            if (coverageComponents[startIndex] != 0U || coverageSamples[startIndex] == 0U ||
                receiverAllowed[startIndex] == 0U) continue;
            coverageComponents[startIndex] = nextComponent;
            pending.clear();
            pending.push_back(startIndex);
            while (!pending.empty()) {
                const auto index = pending.back();
                pending.pop_back();
                const auto x = static_cast<int>(index % shadowWidth);
                const auto y = static_cast<int>(index / shadowWidth);
                for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                    for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                        if (offsetX == 0 && offsetY == 0) continue;
                        const auto neighborX = x + offsetX;
                        const auto neighborY = y + offsetY;
                        if (neighborX < 0 || neighborY < 0 ||
                            neighborX >= static_cast<int>(shadowWidth) ||
                            neighborY >= static_cast<int>(shadowHeight)) continue;
                        const auto neighborIndex = static_cast<std::size_t>(neighborY) *
                            shadowWidth + static_cast<std::size_t>(neighborX);
                        if (coverageComponents[neighborIndex] != 0U ||
                            coverageSamples[neighborIndex] == 0U ||
                            receiverAllowed[neighborIndex] == 0U) continue;
                        coverageComponents[neighborIndex] = nextComponent;
                        pending.push_back(neighborIndex);
                    }
                }
            }
            if (nextComponent != std::numeric_limits<std::uint32_t>::max()) ++nextComponent;
        }
    }
    for (std::uint32_t y = 0; y < shadowHeight; ++y) {
        for (std::uint32_t x = 0; x < shadowWidth; ++x) {
            const auto index = static_cast<std::size_t>(y) * shadowWidth + x;
            bool occluded = false;
            if (receiverAllowed[index] != 0U) {
                if (options.coverageMode == ShadowCoverageMode::CoherentFilter) {
                    // A configurable binomial approximation of a Gaussian filter
                    // produces connected, rounded binary contours without dither.
                    float weightedCoverage = 0.0F;
                    float totalWeight = 0.0F;
                    std::array<std::uint32_t, 49> localComponents{};
                    std::array<float, 49> componentCoverage{};
                    std::size_t localComponentCount = 0U;
                    const auto radius = static_cast<int>(options.gaussianRadius);
                    for (int offsetY = -radius; offsetY <= radius; ++offsetY) {
                        const auto sampleY = static_cast<int>(y) + offsetY;
                        if (sampleY < 0 || sampleY >= static_cast<int>(shadowHeight)) continue;
                        for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
                            const auto sampleX = static_cast<int>(x) + offsetX;
                            if (sampleX < 0 || sampleX >= static_cast<int>(shadowWidth)) continue;
                            const auto weight =
                                gaussianKernel[static_cast<std::size_t>(offsetX + radius)] *
                                gaussianKernel[static_cast<std::size_t>(offsetY + radius)];
                            const auto sampleCoverage = rawCoverage(
                                static_cast<std::uint32_t>(sampleX),
                                static_cast<std::uint32_t>(sampleY));
                            const auto contribution = sampleCoverage * weight;
                            weightedCoverage += contribution;
                            totalWeight += weight;
                            if (contribution <= 0.0F || coverageComponents.empty()) continue;
                            const auto sampleIndex = static_cast<std::size_t>(sampleY) *
                                shadowWidth + static_cast<std::size_t>(sampleX);
                            const auto component = coverageComponents[sampleIndex];
                            if (component == 0U) continue;
                            std::size_t local = 0U;
                            while (local < localComponentCount &&
                                   localComponents[local] != component) ++local;
                            if (local == localComponentCount) {
                                localComponents[local] = component;
                                ++localComponentCount;
                            }
                            componentCoverage[local] += contribution;
                        }
                    }
                    auto strongestCoverage = weightedCoverage;
                    if (options.gaussianRadius > 0U && localComponentCount > 0U) {
                        strongestCoverage = *std::max_element(
                            componentCoverage.begin(),
                            componentCoverage.begin() + static_cast<std::ptrdiff_t>(localComponentCount));
                    }
                    occluded = totalWeight > 0.0F && strongestCoverage / totalWeight >=
                               options.coverageThreshold;
                    if (!occluded && totalWeight > 0.0F &&
                        weightedCoverage / totalWeight >= options.coverageThreshold) {
                        ++result.stats.preventedFilterJoinPixels;
                    }
                } else {
                    occluded = coverageSamples[index] >= samplesPerPixel / 2U + 1U;
                }
            }
            result.shadow.set(x, y, occluded);
        }
    }
    if (options.coverageMode == ShadowCoverageMode::CoherentFilter &&
        options.minimumShadowIslandPixels > 0U) {
        std::vector<std::uint8_t> visited(pixelCount, 0U);
        std::vector<std::size_t> pending;
        std::vector<std::size_t> component;
        pending.reserve(64U);
        component.reserve(64U);
        for (std::size_t startIndex = 0; startIndex < pixelCount; ++startIndex) {
            if (visited[startIndex] != 0U) continue;
            const auto startX = static_cast<std::uint32_t>(startIndex % shadowWidth);
            const auto startY = static_cast<std::uint32_t>(startIndex / shadowWidth);
            if (!result.shadow.get(startX, startY)) continue;
            pending.clear();
            component.clear();
            pending.push_back(startIndex);
            visited[startIndex] = 1U;
            while (!pending.empty()) {
                const auto index = pending.back();
                pending.pop_back();
                component.push_back(index);
                const auto x = static_cast<int>(index % shadowWidth);
                const auto y = static_cast<int>(index / shadowWidth);
                for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                    for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                        if (offsetX == 0 && offsetY == 0) continue;
                        const auto neighborX = x + offsetX;
                        const auto neighborY = y + offsetY;
                        if (neighborX < 0 || neighborY < 0 ||
                            neighborX >= static_cast<int>(shadowWidth) ||
                            neighborY >= static_cast<int>(shadowHeight)) continue;
                        const auto neighborIndex = static_cast<std::size_t>(neighborY) *
                                                   shadowWidth +
                                                   static_cast<std::size_t>(neighborX);
                        if (visited[neighborIndex] != 0U ||
                            !result.shadow.get(static_cast<std::uint32_t>(neighborX),
                                               static_cast<std::uint32_t>(neighborY))) continue;
                        visited[neighborIndex] = 1U;
                        pending.push_back(neighborIndex);
                    }
                }
            }
            if (component.size() < options.minimumShadowIslandPixels) {
                result.stats.removedSmallIslandPixels += component.size();
                for (const auto index : component) {
                    result.shadow.set(static_cast<std::uint32_t>(index % shadowWidth),
                                      static_cast<std::uint32_t>(index / shadowWidth), false);
                }
            }
        }
    }
    const auto shadowed = static_cast<std::uint64_t>(std::count_if(
        result.shadow.bytes().begin(), result.shadow.bytes().end(),
        [](const std::byte value) { return value != std::byte{0}; }));
    const auto finished = Clock::now();
    result.stats.raySeconds = elapsed(built, finished);
    result.stats.rays = static_cast<std::uint64_t>(result.shadow.widthPixels()) *
                        result.shadow.heightPixels() * samplesPerPixel;
    result.stats.shadowedSamples = shadowed;
    result.stats.partialCoveragePixels = partialCoverage.load();
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
