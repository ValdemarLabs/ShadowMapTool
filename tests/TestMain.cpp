#include "archive/MapArchive.hpp"
#include "assets/AssetProvider.hpp"
#include "formats/BLP.hpp"
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
#include "terrain/TerrainAssets.hpp"
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
#include <unordered_map>
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

void writeU32(std::vector<std::byte>& bytes, const std::size_t offset,
              const std::uint32_t value)
{
    require(offset + 4U <= bytes.size(), "test write is out of bounds");
    for (std::size_t index = 0; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
    }
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
    const w3shadow::GenerationOptions defaults;
    require(defaults.lightDirection.x == 1.0F && defaults.lightDirection.y == 1.0F &&
                defaults.lightDirection.z == -2.0F,
            "default light direction does not use the higher sun projection");

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
                1.25F &&
            saddle.sampleHeight(32.0F, 96.0F, w3shadow::TerrainGeometryMode::SmoothSubTile) ==
                1.25F,
            "smooth sub-tile receiver did not match its generated triangle surface");
    require(saddle.terrainTriangles().size() == 2U &&
            saddle.terrainTriangles(w3shadow::TerrainGeometryMode::SmoothSubTile).size() == 8U,
            "terrain geometry modes produced the wrong triangle counts");
    saddle.vertices[1].cliffLayer = 3U;
    saddle.vertices[1].groundHeight = 128.0F;
    require(!saddle.cliffWallTriangles().empty(),
            "cliff-layer transition did not produce occluding wall geometry");

    bytes.resize(8);
    require(!w3shadow::parseW3E(bytes), "truncated W3E was accepted");
}

class MemoryAssets final : public w3shadow::AssetProvider {
public:
    std::unordered_map<std::string, std::vector<std::byte>> files;
    std::optional<std::vector<std::byte>> load(
        const std::string_view path) const override
    {
        const auto found = files.find(std::string(path));
        return found == files.end() ? std::nullopt
                                    : std::optional<std::vector<std::byte>>(found->second);
    }
};

void testTransparentTerrain()
{
    const std::string slk =
        "ID;PWXL;N;E\n"
        "B;Y2;X3\n"
        "C;Y1;X1;K\"tileID\"\n"
        "C;X2;K\"dir\"\n"
        "C;X3;K\"file\"\n"
        "C;Y2;X1;K\"Alph\"\n"
        "C;X2;K\"TerrainArt\"\n"
        "C;X3;K\"Alpha\"\nE\n";
    MemoryAssets assets;
    assets.files["TerrainArt\\Terrain.slk"] = std::vector<std::byte>(
        reinterpret_cast<const std::byte*>(slk.data()),
        reinterpret_cast<const std::byte*>(slk.data() + slk.size()));
    std::vector<std::byte> blp(164U, std::byte{0});
    blp[0] = std::byte{'B'}; blp[1] = std::byte{'L'};
    blp[2] = std::byte{'P'}; blp[3] = std::byte{'1'};
    writeU32(blp, 4U, 1U);   // paletted
    writeU32(blp, 8U, 8U);   // eight-bit alpha
    writeU32(blp, 12U, 2U);  // width
    writeU32(blp, 16U, 2U);  // height
    writeU32(blp, 28U, 156U);
    writeU32(blp, 92U, 8U);
    assets.files["TerrainArt\\Alpha.blp"] = blp;

    w3shadow::W3EMap terrain;
    terrain.info.vertexWidth = terrain.info.vertexHeight = 2U;
    terrain.info.tileWidth = terrain.info.tileHeight = 1U;
    terrain.groundTilesets.push_back("Alph");
    terrain.vertices.resize(4U);
    const auto mask = w3shadow::detectTransparentTerrain(terrain, assets);
    require(mask.detectedTilesets == 1U && mask.transparentAt(terrain, 64.0F, 64.0F),
            "fully transparent BLP terrain was not excluded as a shadow receiver");
}

void testBlpAlphaAndAlphaTestedBvh()
{
    std::vector<std::byte> blp(156U, std::byte{0});
    blp[0] = static_cast<std::byte>('B'); blp[1] = static_cast<std::byte>('L');
    blp[2] = static_cast<std::byte>('P'); blp[3] = static_cast<std::byte>('2');
    blp[8] = std::byte{3}; // raw BGRA
    blp[9] = std::byte{8};
    writeU32(blp, 12U, 2U);
    writeU32(blp, 16U, 1U);
    writeU32(blp, 20U, 148U);
    blp[151] = std::byte{0};
    blp[155] = std::byte{0xFF};
    const auto decoded = w3shadow::decodeBlpAlpha(blp);
    require(static_cast<bool>(decoded), decoded.error);
    require(decoded.mask->sample(0.25F, 0.0F) == 0U &&
            decoded.mask->sample(0.75F, 0.0F) == 0xFFU,
            "BLP2 raw alpha mask was decoded incorrectly");

    w3shadow::Triangle alphaTriangle{{-1.0F, -1.0F, 5.0F},
                                     {1.0F, -1.0F, 5.0F},
                                     {0.0F, 1.0F, 5.0F}};
    alphaTriangle.uvA = {0.25F, 0.0F};
    alphaTriangle.uvB = alphaTriangle.uvA;
    alphaTriangle.uvC = alphaTriangle.uvA;
    alphaTriangle.alphaMask = decoded.mask;
    require(!w3shadow::Bvh(std::array{alphaTriangle}).intersects(
                {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}),
            "transparent texture texel incorrectly blocked a shadow ray");
    alphaTriangle.uvA = {0.75F, 0.0F};
    alphaTriangle.uvB = alphaTriangle.uvA;
    alphaTriangle.uvC = alphaTriangle.uvA;
    require(w3shadow::Bvh(std::array{alphaTriangle}).intersects(
                {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}),
            "opaque texture texel did not block a shadow ray");
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

    std::vector<std::byte> doo7;
    appendTag(doo7, "W3do"); appendU32(doo7, 7U); appendU32(doo7, 9U); appendU32(doo7, 2U);
    for (const auto& rawcode : {"LOsm", "LTlt"}) {
        appendTag(doo7, rawcode); appendU32(doo7, 0U);
        appendF32(doo7, 128.0F); appendF32(doo7, -64.0F); appendF32(doo7, 16.0F);
        appendF32(doo7, 0.5F); appendF32(doo7, 1.0F); appendF32(doo7, 1.0F); appendF32(doo7, 1.0F);
        doo7.push_back(std::byte{0}); doo7.push_back(std::byte{100});
        appendU32(doo7, rawcode[0] == 'L' && rawcode[1] == 'O' ? 11U : 12U);
    }
    const auto parsedDoo7 = w3shadow::parseDOO(doo7);
    require(static_cast<bool>(parsedDoo7), parsedDoo7.error);
    require(parsedDoo7.version == 7U && parsedDoo7.placements.size() == 2U &&
            parsedDoo7.layout == w3shadow::DOOLayout::Classic &&
            parsedDoo7.placements[0].editorId == 11U &&
            parsedDoo7.placements[1].editorId == 12U,
            "DOO v7 compact placement records were not decoded");

    std::vector<std::byte> classicDoo8;
    appendTag(classicDoo8, "W3do"); appendU32(classicDoo8, 8U);
    appendU32(classicDoo8, 11U); appendU32(classicDoo8, 1U);
    appendTag(classicDoo8, "LTlt"); appendU32(classicDoo8, 1U);
    appendF32(classicDoo8, 64.0F); appendF32(classicDoo8, -32.0F);
    appendF32(classicDoo8, 8.0F); appendF32(classicDoo8, 0.25F);
    appendF32(classicDoo8, 1.0F); appendF32(classicDoo8, 1.0F);
    appendF32(classicDoo8, 1.0F);
    classicDoo8.push_back(std::byte{2}); classicDoo8.push_back(std::byte{100});
    appendU32(classicDoo8, 0xFFFFFFFFU); appendU32(classicDoo8, 0U);
    appendU32(classicDoo8, 66U);
    appendU32(classicDoo8, 0U); appendU32(classicDoo8, 1U);
    appendTag(classicDoo8, "CLfa"); appendU32(classicDoo8, 0U);
    appendU32(classicDoo8, 4U); appendU32(classicDoo8, 5U);
    const auto parsedClassicDoo8 = w3shadow::parseDOO(classicDoo8);
    require(static_cast<bool>(parsedClassicDoo8), parsedClassicDoo8.error);
    require(parsedClassicDoo8.layout == w3shadow::DOOLayout::Classic &&
            parsedClassicDoo8.placements.size() == 1U &&
            parsedClassicDoo8.placements[0].skinRawcode == "LTlt" &&
            parsedClassicDoo8.placements[0].editorId == 66U,
            "classic DOO v8 layout was not auto-detected");
    const auto forcedClassicDoo8 = w3shadow::parseDOO(
        classicDoo8, w3shadow::DOOLayoutMode::Classic);
    require(static_cast<bool>(forcedClassicDoo8) &&
            forcedClassicDoo8.layout == w3shadow::DOOLayout::Classic &&
            forcedClassicDoo8.layoutReason.find("forced") != std::string::npos,
            "classic DOO v8 override was not honored");
    require(!w3shadow::parseDOO(classicDoo8, w3shadow::DOOLayoutMode::Modern),
            "modern DOO override incorrectly accepted a classic v8 record");

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
    require(parsedDoo.layout == w3shadow::DOOLayout::Modern &&
            parsedDoo.placements.size() == 1U && parsedDoo.placements[0].rawcode == "LTlt",
            "DOO placement was not decoded");
    require(parsedDoo.placements[0].skinRawcode == "LTlt", "DOO v8 skin rawcode was not decoded");
    require(!w3shadow::parseDOO(doo, w3shadow::DOOLayoutMode::Classic),
            "classic DOO override incorrectly accepted a modern v8 record");
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
    require(parsedDoo13.layout == w3shadow::DOOLayout::Modern,
            "DOO v13 did not select the required modern layout");
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

    const auto scaled = w3shadow::transformTriangles(
        parsedMdx.model.triangles, {10.0F, 20.0F, 30.0F}, 0.0F,
        {2.0F, 3.0F, 4.0F});
    require(scaled.size() == 1U && scaled[0].a.x == 10.0F &&
            scaled[0].a.y == 20.0F && scaled[0].a.z == 30.0F &&
            scaled[0].b.x == 12.0F && scaled[0].b.y == 20.0F &&
            scaled[0].c.x == 10.0F && scaled[0].c.y == 23.0F,
            "placed doodad X/Y/Z scale was not applied before translation");

    auto filteredGeoset = geoset;
    appendTag(filteredGeoset, "GNDX"); appendU32(filteredGeoset, 0U);
    appendTag(filteredGeoset, "MTGC"); appendU32(filteredGeoset, 0U);
    appendTag(filteredGeoset, "MATS"); appendU32(filteredGeoset, 0U);
    appendU32(filteredGeoset, 0U);

    std::vector<std::byte> material;
    appendU32(material, 48U);
    appendU32(material, 0U); // priority plane
    appendU32(material, 0U); // flags
    appendTag(material, "LAYS"); appendU32(material, 1U);
    appendU32(material, 28U);
    appendU32(material, 3U); // additive filter mode
    appendU32(material, 0U); // shading flags
    appendU32(material, 0U); // texture ID
    appendU32(material, 0xFFFFFFFFU); // texture animation ID
    appendU32(material, 0U); // coordinate set
    appendF32(material, 1.0F); // alpha

    std::vector<std::byte> filteredMdx;
    appendTag(filteredMdx, "MDLX");
    appendTag(filteredMdx, "VERS"); appendU32(filteredMdx, 4U);
    appendU32(filteredMdx, 800U);
    appendTag(filteredMdx, "MTLS");
    appendU32(filteredMdx, static_cast<std::uint32_t>(material.size()));
    filteredMdx.insert(filteredMdx.end(), material.begin(), material.end());
    appendTag(filteredMdx, "GEOS");
    appendU32(filteredMdx, static_cast<std::uint32_t>(filteredGeoset.size() + 4U));
    appendU32(filteredMdx, static_cast<std::uint32_t>(filteredGeoset.size() + 4U));
    filteredMdx.insert(filteredMdx.end(), filteredGeoset.begin(), filteredGeoset.end());

    const auto parsedFilteredMdx = w3shadow::parseMDX(filteredMdx);
    require(static_cast<bool>(parsedFilteredMdx), parsedFilteredMdx.error);
    require(parsedFilteredMdx.model.sourceTriangles == 1U &&
            parsedFilteredMdx.model.materialFilteredTriangles == 1U &&
            parsedFilteredMdx.model.triangles.empty(),
            "additive MDX material geometry was not excluded from static shadows");
}

// Small self-contained models keep visibility regressions independent of game assets.
std::vector<std::byte> visibilityTrack(const std::string_view tag,
    const std::vector<std::pair<std::uint32_t, float>>& keys,
    const std::uint32_t interpolation = 0U, const std::uint32_t globalSequence = 0xFFFFFFFFU)
{
    std::vector<std::byte> bytes;
    appendTag(bytes, tag.data()); appendU32(bytes, static_cast<std::uint32_t>(keys.size()));
    appendU32(bytes, interpolation); appendU32(bytes, globalSequence);
    for (const auto& [frame, value] : keys) {
        appendU32(bytes, frame); appendF32(bytes, value);
        if (interpolation > 1U) {
            appendF32(bytes, value); appendF32(bytes, value);
        }
    }
    return bytes;
}

std::vector<std::byte> visibilityModel(const std::uint32_t filterMode,
    const float materialAlpha, const float geosetAlpha,
    const std::vector<std::byte>& geosetTrack = {},
    const std::vector<std::byte>& materialTrack = {}, const bool hasStand = true,
    const std::uint32_t version = 800U)
{
    std::vector<std::byte> mdx;
    appendTag(mdx, "MDLX");
    const auto chunk = [&](const std::string_view tag, const std::vector<std::byte>& data) {
        appendTag(mdx, tag.data()); appendU32(mdx, static_cast<std::uint32_t>(data.size()));
        mdx.insert(mdx.end(), data.begin(), data.end());
    };
    std::vector<std::byte> versions;
    appendU32(versions, version); chunk("VERS", versions);
    std::vector<std::byte> sequences;
    const auto sequence = [&](const std::string& name, std::uint32_t start, std::uint32_t end) {
        const auto offset = sequences.size();
        sequences.resize(offset + 80U, std::byte{0});
        for (std::size_t i = 0; i < name.size(); ++i) sequences[offset + i] = std::byte(name[i]);
        appendU32(sequences, start); appendU32(sequences, end);
        sequences.resize(sequences.size() + 44U, std::byte{0});
    };
    // Portrait comes first intentionally: file order must not choose the display pose.
    sequence("Portrait", 0U, 100U);
    if (hasStand) {
        sequence("Stand Work", 300U, 500U);
        sequence("Stand", 1000U, 2000U);
        sequence("Stand Alternate", 2500U, 3000U);
    }
    chunk("SEQS", sequences);
    std::vector<std::byte> globals;
    appendU32(globals, 100U); chunk("GLBS", globals);

    std::vector<std::byte> material;
    std::vector<std::byte> extension(version > 900U ? 24U : (version > 800U ? 4U : 0U), std::byte{0});
    if (version >= 1100U) {
        appendU32(extension, 0U); // SD shader
        appendU32(extension, 2U); // Deliberately put a non-diffuse slot first.
        appendU32(extension, 0U); appendU32(extension, 1U);
        appendTag(extension, "KMTF"); appendU32(extension, 1U);
        appendU32(extension, 0U); appendU32(extension, 0xFFFFFFFFU);
        appendU32(extension, 1000U); appendU32(extension, 0U);
        appendU32(extension, 1U); appendU32(extension, 0U);
    }
    const auto extensionSize = static_cast<std::uint32_t>(extension.size());
    const auto shaderSize = version >= 900U && version < 1100U ? 80U : 0U;
    appendU32(material, 48U + extensionSize + shaderSize + static_cast<std::uint32_t>(materialTrack.size()));
    appendU32(material, 0U); appendU32(material, 0U);
    material.resize(material.size() + shaderSize, std::byte{0});
    appendTag(material, "LAYS"); appendU32(material, 1U);
    appendU32(material, 28U + extensionSize + static_cast<std::uint32_t>(materialTrack.size()));
    appendU32(material, filterMode); appendU32(material, 0U); appendU32(material, 0U);
    appendU32(material, 0xFFFFFFFFU); appendU32(material, 0U); appendF32(material, materialAlpha);
    material.insert(material.end(), extension.begin(), extension.end());
    material.insert(material.end(), materialTrack.begin(), materialTrack.end());
    chunk("MTLS", material);
    std::vector<std::byte> textures(268U, std::byte{0});
    const std::string path = "Test.blp";
    for (std::size_t i = 0; i < path.size(); ++i) textures[4U + i] = std::byte(path[i]);
    if (version >= 1100U) {
        // The obsolete header texture ID points to glow, while diffuse slot 0
        // points to the real texture. Reading the placeholder would hide the prop.
        std::vector<std::byte> glow(268U, std::byte{0});
        writeU32(glow, 0U, 2U);
        textures.insert(textures.begin(), glow.begin(), glow.end());
    }
    chunk("TEXS", textures);

    std::vector<std::byte> geosets;
    for (std::uint32_t index = 0; index < 2U; ++index) {
        std::vector<std::byte> geoset;
        appendU32(geoset, 0U);
        appendTag(geoset, "VRTX"); appendU32(geoset, 3U);
        const auto x = static_cast<float>(index) * 2.0F;
        for (const auto vertex : std::array<w3shadow::Vec3, 3>{{{x, 0, 0}, {x + 1, 0, 0}, {x, 1, 0}}}) {
            appendF32(geoset, vertex.x); appendF32(geoset, vertex.y); appendF32(geoset, vertex.z);
        }
        appendTag(geoset, "NRMS"); appendU32(geoset, 0U);
        appendTag(geoset, "PTYP"); appendU32(geoset, 0U);
        appendTag(geoset, "PCNT"); appendU32(geoset, 0U);
        appendTag(geoset, "PVTX"); appendU32(geoset, 3U);
        appendU16(geoset, 0U); appendU16(geoset, 1U); appendU16(geoset, 2U);
        appendTag(geoset, "GNDX"); appendU32(geoset, 0U);
        appendTag(geoset, "MTGC"); appendU32(geoset, 0U);
        appendTag(geoset, "MATS"); appendU32(geoset, 0U);
        appendU32(geoset, 0U);
        appendTag(geoset, "UVAS"); appendU32(geoset, 1U);
        appendTag(geoset, "UVBS"); appendU32(geoset, 3U);
        for (const auto uv : std::array<w3shadow::Vec2, 3>{{{0, 0}, {1, 0}, {0, 1}}}) {
            appendF32(geoset, uv.x); appendF32(geoset, uv.y);
        }
        writeU32(geoset, 0U, static_cast<std::uint32_t>(geoset.size()));
        geosets.insert(geosets.end(), geoset.begin(), geoset.end());
    }
    // GEOA before GEOS must also work.
    std::vector<std::byte> animation;
    appendU32(animation, 28U + static_cast<std::uint32_t>(geosetTrack.size()));
    appendF32(animation, geosetAlpha); appendU32(animation, 0U);
    for (int i = 0; i < 3; ++i) appendF32(animation, 1.0F);
    appendU32(animation, 1U);
    animation.insert(animation.end(), geosetTrack.begin(), geosetTrack.end());
    chunk("GEOA", animation); chunk("GEOS", geosets);
    return mdx;
}

void testModelVisibility()
{
    const auto parse = [](const std::vector<std::byte>& bytes) {
        auto result = w3shadow::parseMDX(bytes);
        require(static_cast<bool>(result), result.error);
        return result;
    };
    const auto portrait = visibilityTrack("KGAO", {{0U, 1.0F}, {100U, 1.0F},
                                                 {1000U, 0.0F}, {2000U, 0.0F}});
    auto result = parse(visibilityModel(0U, 1.0F, 1.0F, portrait));
    require(result.model.triangles.size() == 1U && result.model.triangles[0].a.x == 0.0F,
            "portrait-only geoset casts a shadow during Stand");
    w3shadow::Bvh bvh(result.model.triangles);
    require(bvh.intersects({0.2F, 0.2F, 1.0F}, {0, 0, -1}, 0, 2) &&
            !bvh.intersects({2.2F, 0.2F, 1.0F}, {0, 0, -1}, 0, 2),
            "Stand mesh did not preserve the visible prop and remove its portrait background");
    require(parse(visibilityModel(0U, 1.0F, 0.0F)).model.triangles.size() == 1U,
            "static geoset alpha zero was ignored");
    const auto visible = visibilityTrack("KGAO", {{1000U, 1.0F}, {2000U, 1.0F}});
    require(parse(visibilityModel(0U, 1.0F, 0.0F, visible)).model.triangles.size() == 2U,
            "animated visible geoset was removed using its static alpha");
    require(parse(visibilityModel(0U, 1.0F, 1.0F, portrait, {}, false)).model.triangles.size() == 2U,
            "model without Stand incorrectly selected Portrait instead of static visibility");
    const auto portraitOnlyKeys = visibilityTrack("KGAO", {{0U, 0.0F}, {100U, 0.0F}});
    require(parse(visibilityModel(0U, 1.0F, 1.0F, portraitOnlyKeys)).model.triangles.size() == 2U,
            "visibility keys outside Stand leaked into the selected sequence");
    const auto globalHidden = visibilityTrack("KGAO", {{0U, 0.0F}, {100U, 0.0F}}, 0U, 0U);
    require(parse(visibilityModel(0U, 1.0F, 1.0F, globalHidden)).model.triangles.size() == 1U,
            "global visibility was ignored");
    for (const auto interpolation : {0U, 1U, 2U, 3U}) {
        // At frame 1000, the wrap spans 900..1100. Non-step modes fall below the cutoff.
        const auto seam = visibilityTrack("KMTA", {{1100U, 0.0F}, {1900U, 1.0F}}, interpolation);
        const auto parsed = parse(visibilityModel(2U, 1.0F, 1.0F, {}, seam));
        require(parsed.model.triangles.size() == (interpolation == 0U ? 2U : 0U),
                "alpha interpolation across the Stand interval boundary is wrong");
    }
    for (const auto version : {800U, 900U, 1000U, 1100U, 1200U, 1800U}) {
        const auto alpha = visibilityTrack("KMTA", {{0U, 0.0F}, {1000U, 1.0F}, {2000U, 1.0F}});
        require(parse(visibilityModel(2U, 0.0F, 1.0F, {}, alpha, true, version)).model.triangles.size() == 2U,
                "visible Blend material was removed by static alpha or filter mode");
    }
    require(parse(visibilityModel(2U, 0.5F, 1.0F)).model.triangles.empty(),
            "faint Blend effect became an opaque shadow caster");
    for (const auto filter : {3U, 4U, 5U, 6U}) {
        require(parse(visibilityModel(filter, 1.0F, 1.0F)).model.triangles.empty(),
                "visual-effect filter became a shadow caster");
    }
    result = parse(visibilityModel(2U, 1.0F, 1.0F));
    require(result.model.alphaTestedTriangles == 2U, "Blend texture alpha was not enabled");
    auto mask = std::make_shared<w3shadow::AlphaMask>();
    mask->width = 2U; mask->height = 1U; mask->alpha = {0U, 255U};
    for (auto& triangle : result.model.triangles) triangle.alphaMask = mask;
    bvh.rebuild(result.model.triangles);
    require(!bvh.intersects({0.1F, 0.1F, 1.0F}, {0, 0, -1}, 0, 2) &&
            bvh.intersects({0.7F, 0.1F, 1.0F}, {0, 0, -1}, 0, 2),
            "Blend cutout transparency did not reach shadow ray tests");
    result = parse(visibilityModel(2U, 0.8F, 1.0F));
    mask->alpha = {200U, 255U};
    for (auto& triangle : result.model.triangles) triangle.alphaMask = mask;
    bvh.rebuild(result.model.triangles);
    require(!bvh.intersects({0.1F, 0.1F, 1.0F}, {0, 0, -1}, 0, 2) &&
            bvh.intersects({0.7F, 0.1F, 1.0F}, {0, 0, -1}, 0, 2),
            "Blend ray tests ignored the sampled material opacity");
    const auto hiddenMaterial = visibilityTrack("KMTA", {{1000U, 0.0F}, {2000U, 0.0F}});
    require(parse(visibilityModel(0U, 1.0F, 1.0F, {}, hiddenMaterial)).model.triangles.empty(),
            "animated invisible material still casts a shadow");
    const auto invalidGlobal = visibilityTrack("KGAO", {{0U, 1.0F}}, 0U, 5U);
    require(!w3shadow::parseMDX(visibilityModel(0U, 1.0F, 1.0F, invalidGlobal)),
            "out-of-range global sequence was accepted");
    const auto reversed = visibilityTrack("KGAO", {{2000U, 1.0F}, {1000U, 0.0F}});
    require(!w3shadow::parseMDX(visibilityModel(0U, 1.0F, 1.0F, reversed)),
            "out-of-order visibility keys were accepted");
    auto truncated = portrait;
    truncated.pop_back();
    require(!w3shadow::parseMDX(visibilityModel(0U, 1.0F, 1.0F, truncated)),
            "truncated visibility track was accepted");
    const auto invalidMode = visibilityTrack("KGAO", {{1000U, 0.0F}}, 4U);
    require(!w3shadow::parseMDX(visibilityModel(0U, 1.0F, 1.0F, invalidMode)),
            "invalid visibility interpolation was accepted");
}

void testStockShadowSettings()
{
    const auto bytes = [](const std::string& text) {
        return std::vector<std::byte>(reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size()));
    };
    MemoryAssets assets;
    assets.files["Doodads\\Doodads.slk"] = bytes(
        "ID;PWXL;N;E\nC;X1;Y1;K\"doodID\"\nC;X2;K\"file\"\nC;X3;K\"shadow\"\n"
        "C;X1;Y2;K\"S000\"\nC;X2;K\"Same.mdx\"\nC;X3;K0\n"
        "C;X1;Y3;K\"S001\"\nC;X2;K\"Same.mdx\"\nC;X3;K1\nE\n");
    assets.files["Doodads\\DoodadSkins.txt"] = bytes(
        "[D000]\nfile=Same.mdx\nshadow=0\ncomment=Disabled doodad\n"
        "[D001]\nfile=Same.mdx\nshadow=1\n"
        "[D002]\nfile=Same.mdx\nshadow=_\n"
        "[D003]\nfile=Same.mdx\nshadow=\n");
    assets.files["Units\\DestructableSkin.txt"] = bytes(
        "[B000]\nfile=Tree.mdx\nshadow=TreeShadow\n"
        "[B001]\nfile=Tree.mdx\nshadow=_\n");
    w3shadow::ObjectDatabase objects;
    objects.loadStock(assets);
    for (const auto* id : {"S000", "D000", "D002", "D003", "B001"}) {
        require(objects.find(id) && !objects.find(id)->castsShadow,
            std::string("stock False shadow value was enabled: ") + id);
    }
    for (const auto* id : {"S001", "D001", "B000"}) {
        require(objects.find(id) && objects.find(id)->castsShadow,
            std::string("stock True/texture shadow value was disabled: ") + id);
    }
    // A copied custom object can inherit False without serializing dshd at all.
    std::vector<std::byte> custom;
    appendU32(custom, 2U); appendU32(custom, 0U); appendU32(custom, 1U);
    appendTag(custom, "D000"); appendTag(custom, "D100"); appendU32(custom, 1U);
    appendTag(custom, "dfil"); appendU32(custom, 3U);
    appendU32(custom, 0U); appendU32(custom, 0U);
    appendCString(custom, "Imported.mdx"); appendTag(custom, "D100");
    objects.applyMapOverrides(custom, std::nullopt);
    require(objects.find("D100") && !objects.find("D100")->castsShadow &&
        objects.find("D100")->modelPath == "Imported.mdx",
        "custom model did not inherit stock False");
    std::vector<std::byte> overrideData;
    appendU32(overrideData, 2U); appendU32(overrideData, 1U);
    appendTag(overrideData, "D100"); appendU32(overrideData, 0U); appendU32(overrideData, 1U);
    appendTag(overrideData, "dshd"); appendU32(overrideData, 0U);
    appendU32(overrideData, 0U); appendU32(overrideData, 0U);
    appendU32(overrideData, 1U); appendTag(overrideData, "D100"); appendU32(overrideData, 0U);
    objects.applyMapOverrides(overrideData, std::nullopt);
    require(objects.find("D100")->castsShadow, "explicit True did not override inherited False");
}

void testShadowExclusions()
{
    const auto directory = std::filesystem::temp_directory_path() /
        ("w3shadow-exclusions-" + std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    try {
        const auto map = directory / "first.w3x";
        require(w3shadow::loadShadowExclusions(map).empty(), "new maps must have no exclusions");
        const w3shadow::ShadowExclusions selected{"D000", "D001"};
        w3shadow::saveShadowExclusions(map, selected);
        require(w3shadow::loadShadowExclusions(map) == selected, "exclusions did not persist");
        require(w3shadow::loadShadowExclusions(directory / "second.w3x").empty(),
            "exclusions leaked between maps");
        w3shadow::saveShadowExclusions(map, {});
        require(w3shadow::loadShadowExclusions(map).empty(), "cleared exclusions returned on reload");
        const std::string invalid = "W3SHADOW-EXCLUSIONS 1\nnot-hex!\n";
        w3shadow::writeBinaryFileAtomic(w3shadow::shadowExclusionsPath(map),
            {reinterpret_cast<const std::byte*>(invalid.data()), invalid.size()}, true);
        bool rejected = false;
        try { static_cast<void>(w3shadow::loadShadowExclusions(map)); }
        catch (const std::exception&) { rejected = true; }
        require(rejected, "invalid exclusions silently accepted");

        const w3shadow::MapArchive archive(std::filesystem::path(W3SHADOW_FIXTURE_MAP));
        const auto placements = w3shadow::parseDOO(archive.read("war3map.doo"));
        require(static_cast<bool>(placements), placements.error);
        MemoryAssets assets;
        w3shadow::ObjectDatabase emptyDatabase;
        const auto rows = w3shadow::shadowObjectRows(archive, emptyDatabase, w3shadow::DOOLayoutMode::Automatic);
        std::size_t rowCount = 0;
        w3shadow::GenerationOptions options;
        require(options.excludedObjectIds.empty(), "generation excluded objects by default");
        for (const auto& row : rows) {
            rowCount += row.placements;
            options.excludedObjectIds.insert(row.rawcode);
        }
        require(rowCount == placements.placements.size(), "exclusion list lost or duplicated placements");
        options.terrain = false;
        options.shadowSampleGrid = 1;
        options.ignoreTransparentTerrain = false;
        options.honorIgnoreShadowRegions = false;
        const auto excluded = w3shadow::generateShadowMap(archive, assets, options);
        const auto live = std::count_if(placements.placements.begin(), placements.placements.end(),
            [](const auto& placement) { return placement.life != 0U; });
        require(excluded.stats.excludedPlacements == static_cast<std::size_t>(live) &&
            excluded.stats.triangles == 0U && excluded.stats.shadowedSamples == 0U &&
            excluded.stats.unresolvedPlacements == 0U,
            "excluded objects still required models or contributed geometry/shadows");

        // All fixture types share one model; excluding one must not evict that cached model.
        std::string profiles;
        std::set<std::string> definitionIds;
        for (const auto& placement : placements.placements) {
            definitionIds.insert(placement.rawcode);
            definitionIds.insert(placement.skinRawcode);
        }
        for (const auto& id : definitionIds) {
            if (id.size() == 4U && id != std::string(4U, '\0')) {
                profiles += "[" + id + "]\nfile=Shared.mdx\nshadow=1\n";
            }
        }
        assets.files["Doodads\\DoodadSkins.txt"] = std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(profiles.data()),
            reinterpret_cast<const std::byte*>(profiles.data() + profiles.size()));
        std::vector<std::byte> geoset;
        appendTag(geoset, "VRTX"); appendU32(geoset, 3U);
        for (const auto& vertex : std::array<w3shadow::Vec3, 3>{
            w3shadow::Vec3{0, 0, 128}, {256, 0, 128}, {0, 256, 128}}) {
            appendF32(geoset, vertex.x); appendF32(geoset, vertex.y); appendF32(geoset, vertex.z);
        }
        appendTag(geoset, "NRMS"); appendU32(geoset, 0U);
        appendTag(geoset, "PTYP"); appendU32(geoset, 0U);
        appendTag(geoset, "PCNT"); appendU32(geoset, 0U);
        appendTag(geoset, "PVTX"); appendU32(geoset, 3U);
        appendU16(geoset, 0U); appendU16(geoset, 1U); appendU16(geoset, 2U);
        auto& model = assets.files["Shared.mdx"];
        appendTag(model, "MDLX");
        appendTag(model, "VERS"); appendU32(model, 4U); appendU32(model, 800U);
        appendTag(model, "GEOS"); appendU32(model, static_cast<std::uint32_t>(geoset.size() + 4U));
        appendU32(model, static_cast<std::uint32_t>(geoset.size() + 4U));
        model.insert(model.end(), geoset.begin(), geoset.end());
        options.excludedObjectIds.clear();
        const auto baseline = w3shadow::generateShadowMap(archive, assets, options);
        require(baseline.stats.resolvedPlacements == static_cast<std::size_t>(live) &&
            baseline.stats.uniqueModels == 1U, "shared-model fixture did not resolve all placements");
        require(rows.size() > 1U, "fixture requires multiple object types");
        options.excludedObjectIds.insert(rows.front().rawcode);
        const auto partial = w3shadow::generateShadowMap(archive, assets, options);
        require(partial.stats.uniqueModels == 1U && partial.stats.resolvedPlacements > 0U &&
            partial.stats.triangles < baseline.stats.triangles &&
            partial.stats.resolvedPlacements + partial.stats.excludedPlacements == baseline.stats.resolvedPlacements,
            "excluding one type removed other types sharing its model");
        options.excludedObjectIds.clear();
        const auto restored = w3shadow::generateShadowMap(archive, assets, options);
        require(std::equal(restored.shadow.bytes().begin(), restored.shadow.bytes().end(),
            baseline.shadow.bytes().begin(), baseline.shadow.bytes().end()),
            "clearing exclusions changed the original SHD");
        std::size_t setting = 0;
        while ((setting = profiles.find("shadow=1", setting)) != std::string::npos) {
            profiles[setting + 7U] = '0';
        }
        assets.files["Doodads\\DoodadSkins.txt"] = std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(profiles.data()),
            reinterpret_cast<const std::byte*>(profiles.data() + profiles.size()));
        const auto disabled = w3shadow::generateShadowMap(archive, assets, options);
        require(disabled.stats.noShadowPlacements == static_cast<std::size_t>(live) &&
            disabled.stats.uniqueModels == 0U && disabled.stats.triangles == 0U &&
            disabled.stats.shadowedSamples == 0U,
            "empty exclusions allowed stock False objects to load cached models or cast shadows");

        std::filesystem::remove_all(directory);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        throw;
    }
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

    std::vector<std::byte> doodadData;
    appendU32(doodadData, 3U);
    appendU32(doodadData, 0U);
    appendU32(doodadData, 1U);
    appendTag(doodadData, "Dofv");
    appendTag(doodadData, "D123");
    appendU32(doodadData, 1U);
    appendU32(doodadData, 0U);
    appendU32(doodadData, 1U);
    appendTag(doodadData, "dfil"); appendU32(doodadData, 3U);
    appendU32(doodadData, 0U); appendU32(doodadData, 0U);
    appendCString(doodadData, "Custom\\Doodad.mdx");
    appendTag(doodadData, "D123");

    std::vector<std::byte> doodadSkinData;
    appendU32(doodadSkinData, 3U);
    appendU32(doodadSkinData, 0U);
    appendU32(doodadSkinData, 1U);
    appendTag(doodadSkinData, "Dofv");
    appendTag(doodadSkinData, "D123");
    appendU32(doodadSkinData, 1U);
    appendU32(doodadSkinData, 0U);
    appendU32(doodadSkinData, 1U);
    appendTag(doodadSkinData, "dnam"); appendU32(doodadSkinData, 3U);
    appendU32(doodadSkinData, 0U); appendU32(doodadSkinData, 0U);
    appendCString(doodadSkinData, "Custom name");
    appendTag(doodadSkinData, "D123");

    w3shadow::ObjectDatabase doodadObjects;
    doodadObjects.applyMapOverrides(doodadData, std::nullopt);
    doodadObjects.applyMapOverrides(doodadSkinData, std::nullopt);
    const auto* doodadDefinition = doodadObjects.find("D123");
    require(doodadDefinition != nullptr &&
            doodadDefinition->modelPath == "Custom\\Doodad.mdx",
            "skin overlay discarded the custom doodad model override");

    std::vector<std::byte> noShadowData;
    appendU32(noShadowData, 3U);
    appendU32(noShadowData, 0U);
    appendU32(noShadowData, 1U);
    appendTag(noShadowData, "Dofv");
    appendTag(noShadowData, "D124");
    appendU32(noShadowData, 1U);
    appendU32(noShadowData, 0U);
    appendU32(noShadowData, 2U);
    appendTag(noShadowData, "dfil"); appendU32(noShadowData, 3U);
    appendU32(noShadowData, 0U); appendU32(noShadowData, 0U);
    appendCString(noShadowData, "Custom\\NoShadow.mdx");
    appendTag(noShadowData, "D124");
    appendTag(noShadowData, "dshd"); appendU32(noShadowData, 0U);
    appendU32(noShadowData, 0U); appendU32(noShadowData, 0U);
    appendU32(noShadowData, 0U);
    appendTag(noShadowData, "D124");

    doodadObjects.applyMapOverrides(noShadowData, std::nullopt);
    const auto* noShadowDefinition = doodadObjects.find("D124");
    require(noShadowDefinition != nullptr && !noShadowDefinition->castsShadow,
            "integer Has a Shadow override did not disable doodad shadow casting");

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

void testLegacyWarcraftAssets()
{
#ifdef _WIN32
    const std::filesystem::path directory(L"F:\\Pelit\\Warcraft III 1.27b");
    if (!w3shadow::isLegacyWarcraftDirectory(directory)) return;

    const w3shadow::LegacyMpqAssetProvider assets(directory);
    require(assets.available(), assets.error());
    require(assets.load("Doodads\\Doodads.slk").has_value(),
            "legacy MPQ could not load Doodads\\Doodads.slk");
    require(assets.load("Units\\DestructableData.slk").has_value(),
            "legacy MPQ could not load Units\\DestructableData.slk");

    w3shadow::ObjectDatabase objects;
    objects.loadStock(assets);
    const auto* stockTree = objects.find("LTlt");
    require(stockTree != nullptr, "legacy MPQ stock LTlt definition was not loaded");
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
    require(resolvedModel, "legacy MPQ could not resolve stock LTlt MDX; tried " + attempted);

    const auto legacyMapPath = directory / L"Maps" / L"Scenario" / L"(1)TheDeathSheep.w3m";
    if (std::filesystem::is_regular_file(legacyMapPath)) {
        const w3shadow::MapArchive legacyMap(legacyMapPath);
        const auto placements = w3shadow::parseDOO(legacyMap.read("war3map.doo"));
        require(static_cast<bool>(placements), placements.error);
        require(placements.version == 7U && placements.subversion == 9U &&
                placements.placements.size() == 65U,
                "genuine 1.27b DOO v7.9 map was not decoded");

        w3shadow::CompositeAssetProvider generationAssets;
        generationAssets.add(std::make_shared<w3shadow::MapAssetProvider>(legacyMap));
        generationAssets.add(std::make_shared<w3shadow::LegacyMpqAssetProvider>(directory));
        w3shadow::GenerationOptions options;
        options.shadowSampleGrid = 1U;
        const auto generated = w3shadow::generateShadowMap(legacyMap, generationAssets, options);
        require(generated.stats.resolvedPlacements + generated.stats.noShadowPlacements == 65U &&
                generated.stats.unresolvedPlacements == 0U,
                "genuine 1.27b map did not account for every placement after shadow filtering");
        const auto database = w3shadow::loadMapObjectDatabase(legacyMap, generationAssets);
        const auto rows = w3shadow::shadowObjectRows(legacyMap, database, options.dooLayoutMode);
        const auto chosen = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
            return row.anyEnabled && !row.anyDisabled && !row.unresolved;
        });
        require(chosen != rows.end(), "legacy fixture has no eligible exclusion control");
        options.excludedObjectIds.insert(chosen->rawcode);
        const auto filtered = w3shadow::generateShadowMap(legacyMap, generationAssets, options);
        require(filtered.stats.resolvedPlacements + filtered.stats.excludedPlacements ==
                generated.stats.resolvedPlacements && filtered.stats.excludedPlacements > 0U &&
                filtered.stats.triangles < generated.stats.triangles &&
                filtered.stats.noShadowPlacements == generated.stats.noShadowPlacements,
                "selective exclusion affected unrelated types or re-enabled False objects");
        options.excludedObjectIds.clear();
        const auto restored = w3shadow::generateShadowMap(legacyMap, generationAssets, options);
        require(std::equal(restored.shadow.bytes().begin(), restored.shadow.bytes().end(),
                generated.shadow.bytes().begin(), generated.shadow.bytes().end()),
                "clearing exclusions did not restore the original shadow calculation");
    }
#endif
}

void testRegions()
{
    std::vector<std::byte> bytes;
    appendU32(bytes, 5U);
    appendU32(bytes, 1U);
    appendF32(bytes, 512.0F);
    appendF32(bytes, -64.0F);
    appendF32(bytes, -128.0F);
    appendF32(bytes, 256.0F);
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
    std::size_t isolatedShadowCells = 0U;
    for (std::uint32_t y = 0; y < ultra.shadow.heightPixels(); ++y) {
        for (std::uint32_t x = 0; x < ultra.shadow.widthPixels(); ++x) {
            if (!ultra.shadow.get(x, y)) continue;
            std::size_t neighbors = 0U;
            for (int offsetY = -1; offsetY <= 1; ++offsetY) {
                for (int offsetX = -1; offsetX <= 1; ++offsetX) {
                    if (offsetX == 0 && offsetY == 0) continue;
                    const auto neighborX = static_cast<int>(x) + offsetX;
                    const auto neighborY = static_cast<int>(y) + offsetY;
                    if (neighborX >= 0 && neighborY >= 0 &&
                        neighborX < static_cast<int>(ultra.shadow.widthPixels()) &&
                        neighborY < static_cast<int>(ultra.shadow.heightPixels()) &&
                        ultra.shadow.get(static_cast<std::uint32_t>(neighborX),
                                         static_cast<std::uint32_t>(neighborY))) {
                        ++neighbors;
                    }
                }
            }
            if (neighbors == 0U) ++isolatedShadowCells;
        }
    }
    require(isolatedShadowCells == 0U,
            "coherent edge filtering produced isolated shadow dots");

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

    const auto rejects = [&](const w3shadow::GenerationOptions& options) {
        try {
            static_cast<void>(w3shadow::generateShadowMap(
                worldEditorMap, noObjectAssets, options));
            return false;
        } catch (const std::invalid_argument&) {
            return true;
        }
    };
    auto invalidTuning = fastOptions;
    invalidTuning.gaussianRadius = 4U;
    require(rejects(invalidTuning), "invalid Gaussian radius was accepted");

    invalidTuning = fastOptions;
    invalidTuning.coverageThreshold = 1.01F;
    require(rejects(invalidTuning), "invalid coverage threshold was accepted");

    invalidTuning = fastOptions;
    invalidTuning.rayOriginOffset = -0.5F;
    require(rejects(invalidTuning), "invalid terrain ray bias was accepted");

    invalidTuning = fastOptions;
    invalidTuning.sunAngularRadiusDegrees = 5.01F;
    require(rejects(invalidTuning), "invalid sun softness was accepted");

    invalidTuning = fastOptions;
    invalidTuning.minimumShadowIslandPixels = 65U;
    require(rejects(invalidTuning), "invalid minimum island size was accepted");

    invalidTuning = fastOptions;
    invalidTuning.maximumCasterHorizontalSpan = -1.0F;
    require(rejects(invalidTuning), "invalid maximum caster span was accepted");

    auto unfilteredOptions = ultraOptions;
    unfilteredOptions.gaussianRadius = 0U;
    unfilteredOptions.minimumShadowIslandPixels = 0U;
    const auto unfiltered = w3shadow::generateShadowMap(
        worldEditorMap, noObjectAssets, unfilteredOptions);
    require(unfiltered.stats.rays == ultra.stats.rays,
            "filter tuning unexpectedly changed ray coverage work");
#endif
}

} // namespace

int main()
{
    try {
        testShadowMap();
        testPatterns();
        testW3E();
        testTransparentTerrain();
        testBlpAlphaAndAlphaTestedBvh();
        testGeometryAndFormats();
        testModelVisibility();
        testStockShadowSettings();
        testObjectShadowOverride();
        testShadowExclusions();
        testInstalledWarcraftAssets();
        testLegacyWarcraftAssets();
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

