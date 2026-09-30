#include "archive/MapArchive.hpp"
#include "assets/AssetProvider.hpp"
#include "formats/DOO.hpp"
#include "formats/MDX.hpp"
#include "formats/Png.hpp"
#include "formats/W3R.hpp"
#include "formats/W3E.hpp"
#include "geometry/BVH.hpp"
#include "objects/ObjectDatabase.hpp"
#include "shadow/Pattern.hpp"
#include "shadow/ShadowGenerator.hpp"
#include "shadow/ShadowMap.hpp"
#include "util/FileIO.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(const bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void appendU32(std::vector<std::byte>& bytes, const std::uint32_t value)
{
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
}

void appendU16(std::vector<std::byte>& bytes, const std::uint16_t value)
{
    bytes.push_back(static_cast<std::byte>(value & 0xFFU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void appendF32(std::vector<std::byte>& bytes, const float value)
{
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    appendU32(bytes, raw);
}

void appendTag(std::vector<std::byte>& bytes, const char* value)
{
    for (int index = 0; index < 4; ++index) bytes.push_back(static_cast<std::byte>(value[index]));
}

void appendCString(std::vector<std::byte>& bytes, const char* value)
{
    while (*value != '\0') bytes.push_back(static_cast<std::byte>(*value++));
    bytes.push_back(std::byte{0});
}

void testShadowMap()
{
    w3shadow::ShadowMap map(2, 3);
    require(map.widthPixels() == 8 && map.heightPixels() == 12, "wrong SHD dimensions");
    require(map.bytes().size() == 96, "wrong SHD byte count");
    map.set(7, 11, true);
    require(map.get(7, 11), "set/get failed");
    map.setValue(0, 0, 123);
    require(map.value(0, 0) == 123, "raw value failed");

    bool threw = false;
    try { static_cast<void>(map.get(8, 0)); } catch (const std::out_of_range&) { threw = true; }
    require(threw, "out-of-range access was not rejected");

    w3shadow::ShadowMap orientation(1, 1);
    for (std::uint32_t y = 0; y < orientation.heightPixels(); ++y) {
        for (std::uint32_t x = 0; x < orientation.widthPixels(); ++x) {
            orientation.setValue(x, y, static_cast<std::uint8_t>(y * 16U + x));
        }
    }
    const auto warcraft = orientation.warcraftBytes();
    require(std::to_integer<std::uint8_t>(warcraft.front()) == orientation.value(0, 3),
            "Warcraft SHD serialization did not place the bottom row first");
    require(std::to_integer<std::uint8_t>(warcraft[12]) == orientation.value(0, 0),
            "Warcraft SHD serialization did not place the top row last");

    w3shadow::ShadowMap decoded(1, 1);
    decoded.loadWarcraftBytes(warcraft);
    require(std::equal(decoded.bytes().begin(), decoded.bytes().end(), orientation.bytes().begin()),
            "Warcraft SHD decoding did not restore top-down orientation");
}

void testPatterns()
{
    w3shadow::ShadowMap checker(2, 2);
    w3shadow::generatePattern(checker, w3shadow::Pattern::Checker);
    require(!checker.get(0, 0), "checker origin should be lit");
    require(checker.get(4, 0), "checker adjacent tile should be shadowed");
    require(checker.get(0, 4), "checker adjacent row should be shadowed");
    require(!checker.get(4, 4), "checker diagonal tile should be lit");

    w3shadow::ShadowMap gradient(1, 1);
    w3shadow::generatePattern(gradient, w3shadow::Pattern::XGradient);
    require(gradient.value(0, 0) == 0, "x-gradient should begin at zero");
    require(gradient.value(3, 0) == 255, "x-gradient should end at 255");
}

void testW3E()
{
    std::vector<std::byte> bytes{
        std::byte{'W'}, std::byte{'3'}, std::byte{'E'}, std::byte{'!'}};
    appendU32(bytes, 11);
    bytes.push_back(std::byte{'A'});
    appendU32(bytes, 0);
    appendU32(bytes, 1);
    bytes.insert(bytes.end(), {std::byte{'A'}, std::byte{'d'}, std::byte{'r'}, std::byte{'t'}});
    appendU32(bytes, 1);
    bytes.insert(bytes.end(), {std::byte{'C'}, std::byte{'L'}, std::byte{'i'}, std::byte{'f'}});
    appendU32(bytes, 65);
    appendU32(bytes, 97);
    bytes.resize(bytes.size() + 8U + 65U * 97U * 7U, std::byte{0});

    const auto result = w3shadow::parseW3E(bytes);
    require(static_cast<bool>(result), result.error);
    require(result.info.tileWidth == 64 && result.info.tileHeight == 96, "W3E tile dimensions are wrong");

    w3shadow::W3EMap saddle;
    saddle.info.vertexWidth = 2U;
    saddle.info.vertexHeight = 2U;
    saddle.info.tileWidth = 1U;
    saddle.info.tileHeight = 1U;
    saddle.vertices.resize(4U);
    saddle.vertices[3].groundHeight = 10.0F;
    require(saddle.sampleHeight(96.0F, 32.0F) == 2.5F &&
            saddle.sampleHeight(32.0F, 96.0F) == 2.5F,
            "W3E height sampling does not match the generated terrain triangles");
    require(saddle.sampleHeight(96.0F, 32.0F, w3shadow::TerrainGeometryMode::SmoothSubTile) ==
                1.875F &&
            saddle.sampleHeight(32.0F, 96.0F, w3shadow::TerrainGeometryMode::SmoothSubTile) ==
                1.875F,
            "smooth sub-tile terrain did not use bilinear height reconstruction");
    require(saddle.terrainTriangles().size() == 2U &&
            saddle.terrainTriangles(w3shadow::TerrainGeometryMode::SmoothSubTile).size() == 8U,
            "terrain geometry modes produced the wrong triangle counts");

    bytes.resize(8);
    require(!w3shadow::parseW3E(bytes), "truncated W3E was accepted");
}

void testGeometryAndFormats()
{
    const std::array<w3shadow::Triangle, 1> triangles{
        w3shadow::Triangle{{-1.0F, -1.0F, 5.0F}, {1.0F, -1.0F, 5.0F}, {0.0F, 1.0F, 5.0F}}};
    const w3shadow::Bvh bvh(triangles);
    require(bvh.intersects({0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}),
            "BVH missed a triangle");
    require(!bvh.intersects({5.0F, 5.0F, 0.0F}, {0.0F, 0.0F, 1.0F}),
            "BVH reported a false intersection");

    std::vector<std::byte> doo;
    appendTag(doo, "W3do"); appendU32(doo, 8U); appendU32(doo, 11U); appendU32(doo, 1U);
    appendTag(doo, "LTlt"); appendU32(doo, 2U);
    appendF32(doo, 128.0F); appendF32(doo, -64.0F); appendF32(doo, 16.0F);
    appendF32(doo, 0.5F); appendF32(doo, 1.0F); appendF32(doo, 2.0F); appendF32(doo, 3.0F);
    appendTag(doo, "LTlt");
    doo.push_back(std::byte{0}); doo.push_back(std::byte{100});
    appendU32(doo, 0xFFFFFFFFU); appendU32(doo, 0U); appendU32(doo, 77U);
    appendU32(doo, 0U); appendU32(doo, 2U);
    appendTag(doo, "CLfa"); appendU32(doo, 3U); appendU32(doo, 12U); appendU32(doo, 34U);
    appendTag(doo, "CLfb"); appendU32(doo, 1U); appendU32(doo, 56U); appendU32(doo, 78U);
    const auto parsedDoo = w3shadow::parseDOO(doo);
    require(static_cast<bool>(parsedDoo), parsedDoo.error);
    require(parsedDoo.placements.size() == 1U && parsedDoo.placements[0].rawcode == "LTlt",
            "DOO placement was not decoded");
    require(parsedDoo.placements[0].skinRawcode == "LTlt", "DOO v8 skin rawcode was not decoded");
    require(parsedDoo.specialVersion == 0U && parsedDoo.specialPlacements.size() == 2U,
            "DOO special-doodad section was not decoded");
    require(parsedDoo.specialPlacements[0].rawcode == "CLfa" &&
            parsedDoo.specialPlacements[0].variation == 3U &&
            parsedDoo.specialPlacements[0].tileX == 12 && parsedDoo.specialPlacements[0].tileY == 34,
            "DOO special-doodad record is misaligned");

    std::vector<std::byte> doo13;
    appendTag(doo13, "W3do"); appendU32(doo13, 13U); appendU32(doo13, 11U); appendU32(doo13, 1U);
    appendTag(doo13, "LTlt"); appendU32(doo13, 3U);
    appendF32(doo13, 32.0F); appendF32(doo13, 64.0F); appendF32(doo13, 96.0F);
    appendF32(doo13, 0.25F); appendF32(doo13, 1.0F); appendF32(doo13, 1.5F); appendF32(doo13, 2.0F);
    appendTag(doo13, "LTlt"); appendU32(doo13, 7U);
    doo13.push_back(std::byte{2}); doo13.push_back(std::byte{90});
    appendU32(doo13, 0xFFFFFFFFU); appendU32(doo13, 0U);
    appendU32(doo13, 0xFF3366CCU); appendU32(doo13, 123U);
    appendF32(doo13, 0.1F); appendF32(doo13, -0.2F); appendU32(doo13, 1U);
    appendU32(doo13, 4U); appendU32(doo13, 1U); appendU32(doo13, 0xFFFFFFFFU);
    appendF32(doo13, 2.0F); appendF32(doo13, 100.0F); appendF32(doo13, 300.0F);
    appendF32(doo13, 0.25F); appendF32(doo13, 0.5F); appendF32(doo13, 0.75F);
    const auto parsedDoo13 = w3shadow::parseDOO(doo13);
    require(static_cast<bool>(parsedDoo13), parsedDoo13.error);
    require(parsedDoo13.version == 13U && parsedDoo13.placements.size() == 1U,
            "DOO v13 placement was not decoded");
    require(parsedDoo13.placements[0].groupId == 7 && parsedDoo13.placements[0].lightCount == 1U &&
            parsedDoo13.placements[0].editorId == 123U,
            "DOO v13 extension fields are misaligned");

#ifdef W3SHADOW_DOO_FIXTURE
    const auto productionDoo = w3shadow::parseDOO(
        w3shadow::readBinaryFile(std::filesystem::path(W3SHADOW_DOO_FIXTURE)));
    require(static_cast<bool>(productionDoo), productionDoo.error);
    require(productionDoo.version == 8U && productionDoo.placements.size() == 50118U,
            "production DOO placement count is wrong");
    require(productionDoo.placements.front().rawcode == "B003" &&
            productionDoo.placements.front().skinRawcode == "B003",
            "production DOO first record is misaligned");
#endif

#ifdef W3SHADOW_CURRENT_MAP_FIXTURE
    {
        const w3shadow::MapArchive currentMap(std::filesystem::path(W3SHADOW_CURRENT_MAP_FIXTURE));
        const auto currentDoo = w3shadow::parseDOO(currentMap.read("war3map.doo"));
        require(static_cast<bool>(currentDoo), currentDoo.error);
        require(!currentDoo.placements.empty(), "current-map DOO has no placements");
    }
#endif

    std::vector<std::byte> geoset;
    appendTag(geoset, "VRTX"); appendU32(geoset, 3U);
    const std::array<w3shadow::Vec3, 3> vertices{
        w3shadow::Vec3{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}};
    for (const auto& vertex : vertices) {
        appendF32(geoset, vertex.x); appendF32(geoset, vertex.y); appendF32(geoset, vertex.z);
    }
    appendTag(geoset, "NRMS"); appendU32(geoset, 0U);
    appendTag(geoset, "PTYP"); appendU32(geoset, 0U);
    appendTag(geoset, "PCNT"); appendU32(geoset, 0U);
    appendTag(geoset, "PVTX"); appendU32(geoset, 3U);
    appendU16(geoset, 0U); appendU16(geoset, 1U); appendU16(geoset, 2U);
    std::vector<std::byte> mdx;
    appendTag(mdx, "MDLX");
    appendTag(mdx, "VERS"); appendU32(mdx, 4U); appendU32(mdx, 800U);
    appendTag(mdx, "GEOS"); appendU32(mdx, static_cast<std::uint32_t>(geoset.size() + 4U));
    appendU32(mdx, static_cast<std::uint32_t>(geoset.size() + 4U));
    mdx.insert(mdx.end(), geoset.begin(), geoset.end());
    const auto parsedMdx = w3shadow::parseMDX(mdx);
    require(static_cast<bool>(parsedMdx), parsedMdx.error);
    require(parsedMdx.model.triangles.size() == 1U, "MDX geoset triangle was not decoded");
}

void testObjectShadowOverride()
{
    std::vector<std::byte> data;
    appendU32(data, 3U);
    appendU32(data, 0U);
    appendU32(data, 1U);
    appendTag(data, "LTlt");
    appendTag(data, "B123");
    appendU32(data, 1U);
    appendU32(data, 0U);
    appendU32(data, 2U);
    appendTag(data, "bfil"); appendU32(data, 3U); appendCString(data, "Custom\\Tree.mdx");
    appendTag(data, "B123");
    appendTag(data, "bshd"); appendU32(data, 3U); appendCString(data, "");
    appendTag(data, "B123");

    w3shadow::ObjectDatabase objects;
    objects.applyMapOverrides(std::nullopt, data);
    const auto* definition = objects.find("B123");
    require(definition != nullptr, "custom destructible object was not decoded");
    require(!definition->castsShadow, "empty bshd override did not disable object shadow");

#ifdef W3SHADOW_CURRENT_MAP_FIXTURE
    const w3shadow::MapArchive currentMap(std::filesystem::path(W3SHADOW_CURRENT_MAP_FIXTURE));
    w3shadow::ObjectDatabase currentObjects;
    currentObjects.applyMapOverrides(
        currentMap.contains("war3map.w3d")
            ? std::optional<std::vector<std::byte>>(currentMap.read("war3map.w3d"))
            : std::nullopt,
        currentMap.contains("war3map.w3b")
            ? std::optional<std::vector<std::byte>>(currentMap.read("war3map.w3b"))
            : std::nullopt);
    currentObjects.applyMapOverrides(
        currentMap.contains("war3mapSkin.w3d")
            ? std::optional<std::vector<std::byte>>(currentMap.read("war3mapSkin.w3d"))
            : std::nullopt,
        currentMap.contains("war3mapSkin.w3b")
            ? std::optional<std::vector<std::byte>>(currentMap.read("war3mapSkin.w3b"))
            : std::nullopt);
    require(currentObjects.find("B003") != nullptr,
            "current-map version-3 custom destructible skin B003 was not decoded");
#endif
}

void testInstalledWarcraftAssets()
{
#ifdef _WIN32
    const std::array<std::filesystem::path, 2> candidates{
        L"C:\\Program Files (x86)\\Warcraft III", L"C:\\Program Files\\Warcraft III"};
    for (const auto& directory : candidates) {
        if (!std::filesystem::exists(directory / L".build.info")) continue;
        const w3shadow::CascAssetProvider assets(directory);
        require(assets.available(), assets.error());
        require(assets.load("Doodads\\Doodads.slk").has_value(),
                "CASC could not load Doodads\\Doodads.slk");
        require(assets.load("Units\\DestructableData.slk").has_value(),
                "CASC could not load Units\\DestructableData.slk");
#ifdef W3SHADOW_CURRENT_MAP_FIXTURE
        w3shadow::ObjectDatabase objects;
        objects.loadStock(assets);
        const auto* stockTree = objects.find("LTlt");
        if (stockTree == nullptr) {
            const auto stockBytes = assets.load("Units\\DestructableSkin.txt");
            std::string prefix;
            if (stockBytes) {
                const std::string text(reinterpret_cast<const char*>(stockBytes->data()),
                                       stockBytes->size());
                const auto found = text.find("[LTlt]");
                const auto start = found == std::string::npos || found < 80U ? 0U : found - 80U;
                const auto count = std::min<std::size_t>(text.size() - start, 1200U);
                prefix = text.substr(start, count);
                std::replace_if(prefix.begin(), prefix.end(),
                                [](const char value) { return value == '\r' || value == '\n'; },
                                ' ');
            }
            throw std::runtime_error("stock LTlt definition was not loaded; database size " +
                                     std::to_string(objects.size()) + "; prefix: " + prefix);
        }
        bool resolvedModel = false;
        std::string attempted;
        for (const auto& path : w3shadow::modelPathCandidates(*stockTree, 0U)) {
            if (!attempted.empty()) attempted += ", ";
            attempted += path;
            if (assets.load(path)) {
                resolvedModel = true;
                break;
            }
        }
        require(resolvedModel, "CASC could not resolve stock LTlt MDX; tried " + attempted);

        const auto* seaweed = objects.find("ZWsw");
        require(seaweed != nullptr, "stock ZWsw definition was not loaded");
        resolvedModel = false;
        attempted.clear();
        for (const auto& path : w3shadow::modelPathCandidates(*seaweed, 0U)) {
            if (!attempted.empty()) attempted += ", ";
            attempted += path;
            if (assets.load(path)) {
                resolvedModel = true;
                break;
            }
        }
        require(resolvedModel, "CASC could not resolve stock ZWsw MDX; tried " + attempted);
#endif
        return;
    }
#endif
}

void testRegions()
{
    std::vector<std::byte> bytes;
    appendU32(bytes, 5U);
    appendU32(bytes, 1U);
    appendF32(bytes, 512.0F);
    appendF32(bytes, -128.0F);
    appendF32(bytes, 256.0F);
    appendF32(bytes, -64.0F);
    appendCString(bytes, "IgnoreShadow village");
    appendU32(bytes, 17U);
    appendTag(bytes, "RAhr");
    appendCString(bytes, "Sound\\Ambient");
    appendU32(bytes, 0xFF3366CCU);

    const auto parsed = w3shadow::parseW3R(bytes);
    require(static_cast<bool>(parsed), parsed.error);
    require(parsed.regions.size() == 1U, "W3R region was not decoded");
    const auto& region = parsed.regions.front();
    require(region.left == -128.0F && region.right == 512.0F &&
            region.bottom == -64.0F && region.top == 256.0F,
            "W3R reversed bounds were not normalized");
    require(region.contains(0.0F, 0.0F), "W3R containment failed");
    require(w3shadow::isIgnoreShadowRegion(region.name), "ignore-shadow prefix was not recognized");
    require(!w3shadow::isIgnoreShadowRegion("DoNotIgnoreShadow"), "invalid ignore-shadow name matched");

#ifdef W3SHADOW_CURRENT_MAP_FIXTURE
    const w3shadow::MapArchive currentMap(std::filesystem::path(W3SHADOW_CURRENT_MAP_FIXTURE));
    const auto currentRegionBytes = currentMap.read("war3map.w3r");
    const auto currentRegions = w3shadow::parseW3R(currentRegionBytes);
    require(static_cast<bool>(currentRegions), currentRegions.error);
#endif

    bytes.pop_back();
    require(!w3shadow::parseW3R(bytes), "truncated W3R was accepted");
}

void testPng()
{
    w3shadow::ShadowMap map(1, 1);
    w3shadow::generatePattern(map, w3shadow::Pattern::Checker);
    const auto png = w3shadow::makeShadowPreviewPng(4, 4, map.bytes());
    const std::array<std::uint8_t, 8> signature{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    require(png.size() > signature.size(), "PNG is unexpectedly short");
    for (std::size_t index = 0; index < signature.size(); ++index) {
        require(std::to_integer<std::uint8_t>(png[index]) == signature[index], "PNG signature mismatch");
    }
}

void testArchiveReplacement()
{
    const std::filesystem::path fixture(W3SHADOW_FIXTURE_MAP);
    const auto uniqueName = "w3shadow-test-" + std::to_string(
        std::chrono::high_resolution_clock::now().time_since_epoch().count());
    const auto testDirectory = std::filesystem::temp_directory_path() / uniqueName;
    std::filesystem::create_directory(testDirectory);

    try {
        const auto input = testDirectory / "input.w3m";
        const auto output = testDirectory / "output.w3m";
        std::filesystem::copy_file(fixture, input);
        const auto original = w3shadow::readBinaryFile(input);

        w3shadow::ShadowMap shadow(64, 64);
        w3shadow::generatePattern(shadow, w3shadow::Pattern::Quadrants);
        const auto warcraftBytes = shadow.warcraftBytes();
        w3shadow::MapArchive::replaceShadowInCopy(input, output, warcraftBytes, false);
        require(w3shadow::readBinaryFile(input) == original, "copy mode changed the input map");
        {
            const w3shadow::MapArchive archive(output);
            require(archive.read("war3map.shd") == warcraftBytes,
                    "copy-mode SHD round trip failed");
        }

        const auto backup = w3shadow::MapArchive::replaceShadowInPlace(input, warcraftBytes);
        require(std::filesystem::exists(backup), "in-place mode did not retain a backup");
        require(w3shadow::readBinaryFile(backup) == original, "in-place backup differs from the original");
        {
            const w3shadow::MapArchive archive(input);
            require(archive.read("war3map.shd") == warcraftBytes,
                    "in-place SHD round trip failed");
        }

        std::filesystem::remove_all(testDirectory);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(testDirectory, ignored);
        throw;
    }
}

void testWorldEditorReferencePair()
{
#if defined(W3SHADOW_WE_SHADOW_FIXTURE) && defined(W3SHADOW_TOOL_SHADOW_FIXTURE)
    const w3shadow::MapArchive worldEditorMap(
        std::filesystem::path(W3SHADOW_WE_SHADOW_FIXTURE));
    const w3shadow::MapArchive toolMap(
        std::filesystem::path(W3SHADOW_TOOL_SHADOW_FIXTURE));

    require(worldEditorMap.read("war3map.w3e") == toolMap.read("war3map.w3e"),
            "reference maps do not contain the same terrain");
    require(worldEditorMap.read("war3map.doo") == toolMap.read("war3map.doo"),
            "reference maps do not contain the same doodad placements");

    const auto worldEditorShadow = worldEditorMap.read("war3map.shd");
    const auto toolShadow = toolMap.read("war3map.shd");
    require(worldEditorShadow.size() == 65536U && toolShadow.size() == 65536U,
            "reference SHD dimensions changed");
    const auto shadowedCount = [](const std::vector<std::byte>& bytes) {
        return static_cast<std::size_t>(std::count_if(
            bytes.begin(), bytes.end(), [](const std::byte value) {
                return value != std::byte{0};
            }));
    };
    require(shadowedCount(worldEditorShadow) == 2050U,
            "World Editor reference SHD pixel count changed");
    require(shadowedCount(toolShadow) == 4631U,
            "version-2 Smooth sub-tile reference SHD pixel count changed");
    require(worldEditorShadow != toolShadow,
            "reference SHDs unexpectedly became byte-identical");

    w3shadow::CompositeAssetProvider noObjectAssets;
    w3shadow::GenerationOptions fastOptions;
    fastOptions.shadowSampleGrid = 1U;
    fastOptions.doodads = false;
    fastOptions.destructibles = false;
    const auto fast = w3shadow::generateShadowMap(
        worldEditorMap, noObjectAssets, fastOptions);
    require(fast.stats.shadowSampleGrid == 1U && fast.stats.rays == 65536U,
            "1x edge-quality ray count is wrong");

    auto ultraOptions = fastOptions;
    ultraOptions.shadowSampleGrid = 4U;
    const auto ultra = w3shadow::generateShadowMap(
        worldEditorMap, noObjectAssets, ultraOptions);
    require(ultra.stats.shadowSampleGrid == 4U && ultra.stats.rays == 1048576U,
            "4x edge-quality ray count is wrong");
    require(std::all_of(ultra.shadow.bytes().begin(), ultra.shadow.bytes().end(),
                        [](const std::byte value) {
                            return value == std::byte{0} || value == std::byte{0xFF};
                        }),
            "supersampling produced non-binary SHD values");

    auto invalidOptions = fastOptions;
    invalidOptions.shadowSampleGrid = 3U;
    bool rejectedInvalidGrid = false;
    try {
        static_cast<void>(w3shadow::generateShadowMap(
            worldEditorMap, noObjectAssets, invalidOptions));
    } catch (const std::invalid_argument&) {
        rejectedInvalidGrid = true;
    }
    require(rejectedInvalidGrid, "invalid shadow sample grid was accepted");
#endif
}

} // namespace

int main()
{
    try {
        testShadowMap();
        testPatterns();
        testW3E();
        testGeometryAndFormats();
        testObjectShadowOverride();
        testInstalledWarcraftAssets();
        testRegions();
        testPng();
        testArchiveReplacement();
        testWorldEditorReferencePair();
        std::cout << "All tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}

