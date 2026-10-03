#include "formats/MDX.hpp"

#include "util/BinaryReader.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>

namespace w3shadow {
namespace {

constexpr std::uint32_t maximumVertices = 10000000U;
constexpr std::uint32_t maximumIndices = 30000000U;
constexpr std::uint32_t noIndex = std::numeric_limits<std::uint32_t>::max();

struct MaterialLayer {
    std::uint32_t filterMode = 0;
    std::uint32_t textureIndex = noIndex;
    float alpha = 1.0F;
};

using Material = std::vector<MaterialLayer>;

void requireTag(BinaryReader& reader, const std::string_view expected)
{
    const auto actual = reader.readTag(expected);
    if (actual != expected) {
        throw std::runtime_error("MDX geoset parse error: expected " + std::string(expected) +
                                 ", found " + actual);
    }
}

std::optional<std::size_t> findTag(
    const std::span<const std::byte> bytes, const std::size_t start,
    const std::string_view tag)
{
    for (std::size_t offset = start; offset + tag.size() <= bytes.size(); ++offset) {
        if (std::equal(tag.begin(), tag.end(),
                       reinterpret_cast<const char*>(bytes.data() + offset))) {
            return offset;
        }
    }
    return std::nullopt;
}

void parseTextures(BinaryReader& chunk, MDXModel& model)
{
    constexpr std::size_t textureRecordSize = 268U;
    while (chunk.remaining() >= textureRecordSize) {
        MDXTexture texture;
        texture.replaceableId = chunk.readU32("texture replaceable ID");
        const auto pathBytes = chunk.readBytes(260U, "texture path");
        const auto end = std::find(pathBytes.begin(), pathBytes.end(), std::byte{0});
        texture.path.assign(reinterpret_cast<const char*>(pathBytes.data()),
                            static_cast<std::size_t>(end - pathBytes.begin()));
        static_cast<void>(chunk.readU32("texture flags"));
        model.textures.push_back(std::move(texture));
    }
}

void parseMaterials(BinaryReader& chunk, std::vector<Material>& materials)
{
    while (!chunk.empty()) {
        const auto inclusiveSize = chunk.readU32("material size");
        if (inclusiveSize < 12U || inclusiveSize - 4U > chunk.remaining()) {
            throw std::runtime_error("MDX parse error: invalid material size");
        }
        const auto bytes = chunk.readBytes(inclusiveSize - 4U, "material");
        Material material;
        const auto laysOffset = findTag(bytes, 0U, "LAYS");
        if (laysOffset && *laysOffset + 8U <= bytes.size()) {
            BinaryReader layers(bytes.subspan(*laysOffset + 4U), "MDX material layers");
            const auto layerCount = layers.readU32("layer count");
            if (layerCount > 100000U) throw std::runtime_error("MDX parse error: layer count exceeds safety limit");
            material.reserve(layerCount);
            for (std::uint32_t index = 0; index < layerCount; ++index) {
                const auto layerSize = layers.readU32("layer size");
                if (layerSize < 28U || layerSize - 4U > layers.remaining()) {
                    throw std::runtime_error("MDX parse error: invalid material layer size");
                }
                auto layer = layers.subReader(layerSize - 4U, "layer");
                MaterialLayer parsed;
                parsed.filterMode = layer.readU32("filter mode");
                static_cast<void>(layer.readU32("shading flags"));
                parsed.textureIndex = layer.readU32("texture ID");
                static_cast<void>(layer.readU32("texture animation ID"));
                static_cast<void>(layer.readU32("texture coordinate ID"));
                parsed.alpha = layer.readF32("static alpha");
                material.push_back(parsed);
            }
        }
        materials.push_back(std::move(material));
    }
}

void parseGeosets(BinaryReader& chunk, MDXModel& model)
{
    while (!chunk.empty()) {
        const auto inclusiveSize = chunk.readU32("geoset size");
        if (inclusiveSize < 4U || inclusiveSize - 4U > chunk.remaining()) {
            throw std::runtime_error("MDX parse error: invalid geoset size");
        }
        const auto geosetBytes = chunk.readBytes(inclusiveSize - 4U, "geoset");
        BinaryReader geoset(geosetBytes, "MDX geoset");
        requireTag(geoset, "VRTX");
        const auto vertexCount = geoset.readU32("vertex count");
        if (vertexCount > maximumVertices ||
            static_cast<std::uint64_t>(vertexCount) * 12U > geoset.remaining()) {
            throw std::runtime_error("MDX parse error: invalid vertex count");
        }
        std::vector<Vec3> vertices;
        vertices.reserve(vertexCount);
        for (std::uint32_t index = 0; index < vertexCount; ++index) {
            const Vec3 vertex{geoset.readF32("vertex X"), geoset.readF32("vertex Y"),
                              geoset.readF32("vertex Z")};
            if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z)) {
                throw std::runtime_error("MDX parse error: non-finite vertex");
            }
            vertices.push_back(vertex);
        }
        requireTag(geoset, "NRMS");
        const auto normalCount = geoset.readU32("normal count");
        if (normalCount > maximumVertices ||
            static_cast<std::uint64_t>(normalCount) * 12U > geoset.remaining()) {
            throw std::runtime_error("MDX parse error: invalid normal count");
        }
        geoset.skip(static_cast<std::size_t>(normalCount) * 12U, "normals");
        requireTag(geoset, "PTYP");
        const auto primitiveCount = geoset.readU32("primitive type count");
        if (primitiveCount > maximumVertices ||
            static_cast<std::uint64_t>(primitiveCount) * 4U > geoset.remaining()) {
            throw std::runtime_error("MDX parse error: invalid primitive type count");
        }
        geoset.skip(static_cast<std::size_t>(primitiveCount) * 4U, "primitive types");
        requireTag(geoset, "PCNT");
        const auto groupCount = geoset.readU32("face group count");
        if (groupCount > maximumVertices ||
            static_cast<std::uint64_t>(groupCount) * 4U > geoset.remaining()) {
            throw std::runtime_error("MDX parse error: invalid face group count");
        }
        geoset.skip(static_cast<std::size_t>(groupCount) * 4U, "face groups");
        requireTag(geoset, "PVTX");
        const auto indexCount = geoset.readU32("face index count");
        if (indexCount > maximumIndices ||
            static_cast<std::uint64_t>(indexCount) * 2U > geoset.remaining()) {
            throw std::runtime_error("MDX parse error: invalid face index count");
        }
        std::vector<std::uint16_t> indices;
        indices.reserve(indexCount);
        for (std::uint32_t index = 0; index < indexCount; ++index) {
            indices.push_back(geoset.readU16("face index"));
        }

        auto skipArray = [&](const std::string_view tag, const std::size_t elementSize) {
            requireTag(geoset, tag);
            const auto count = geoset.readU32(std::string(tag) + " count");
            if (count > maximumIndices ||
                static_cast<std::uint64_t>(count) * elementSize > geoset.remaining()) {
                throw std::runtime_error("MDX parse error: invalid " + std::string(tag) + " count");
            }
            geoset.skip(static_cast<std::size_t>(count) * elementSize, tag);
        };
        std::uint32_t materialId = noIndex;
        if (!geoset.empty()) {
            skipArray("GNDX", 1U);
            skipArray("MTGC", 4U);
            skipArray("MATS", 4U);
            materialId = geoset.readU32("material ID");
        }

        std::vector<Vec2> textureCoordinates;
        if (const auto uvasOffset = findTag(geosetBytes, geoset.offset(), "UVAS")) {
            BinaryReader uvReader(geosetBytes.subspan(*uvasOffset + 4U), "MDX UV sets");
            const auto setCount = uvReader.readU32("UV set count");
            if (setCount > 0U) {
                requireTag(uvReader, "UVBS");
                const auto coordinateCount = uvReader.readU32("UV coordinate count");
                if (coordinateCount > maximumVertices ||
                    static_cast<std::uint64_t>(coordinateCount) * 8U > uvReader.remaining()) {
                    throw std::runtime_error("MDX parse error: invalid UV coordinate count");
                }
                textureCoordinates.reserve(coordinateCount);
                for (std::uint32_t index = 0; index < coordinateCount; ++index) {
                    textureCoordinates.push_back({uvReader.readF32("texture U"),
                                                  uvReader.readF32("texture V")});
                }
            }
        }

        for (std::size_t index = 0; index + 2U < indices.size(); index += 3U) {
            const auto a = indices[index];
            const auto b = indices[index + 1U];
            const auto c = indices[index + 2U];
            if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) {
                throw std::runtime_error("MDX parse error: face index exceeds vertex count");
            }
            Triangle triangle{vertices[a], vertices[b], vertices[c]};
            triangle.materialId = materialId;
            if (textureCoordinates.size() == vertices.size()) {
                triangle.uvA = textureCoordinates[a];
                triangle.uvB = textureCoordinates[b];
                triangle.uvC = textureCoordinates[c];
            }
            model.triangles.push_back(std::move(triangle));
            ++model.sourceTriangles;
        }
    }
}

bool isTeamGlow(const MDXModel& model, const std::uint32_t textureIndex)
{
    return textureIndex < model.textures.size() &&
           model.textures[textureIndex].replaceableId == 2U;
}

void applyMaterialFiltering(MDXModel& model, const std::vector<Material>& materials)
{
    auto output = model.triangles.begin();
    for (auto& triangle : model.triangles) {
        bool castsShadow = true;
        bool alphaTested = false;
        std::uint32_t textureIndex = noIndex;
        if (triangle.materialId < materials.size() && !materials[triangle.materialId].empty()) {
            castsShadow = false;
            for (const auto& layer : materials[triangle.materialId]) {
                if (!std::isfinite(layer.alpha) || layer.alpha <= 0.001F ||
                    isTeamGlow(model, layer.textureIndex)) continue;
                if (layer.filterMode == 0U) {
                    castsShadow = true;
                    alphaTested = false;
                    textureIndex = noIndex;
                    break;
                }
                if (layer.filterMode == 1U && !castsShadow) {
                    castsShadow = true;
                    alphaTested = layer.textureIndex < model.textures.size() &&
                                  !model.textures[layer.textureIndex].path.empty();
                    textureIndex = layer.textureIndex;
                }
                // Blend, additive, add-alpha and modulate layers are visual effects
                // and do not provide solid static-shadow geometry.
            }
        }
        if (!castsShadow) {
            ++model.materialFilteredTriangles;
            continue;
        }
        triangle.alphaTested = alphaTested;
        triangle.shadowTextureIndex = alphaTested ? textureIndex : noIndex;
        if (alphaTested) ++model.alphaTestedTriangles;
        if (&*output != &triangle) *output = std::move(triangle);
        ++output;
    }
    model.triangles.erase(output, model.triangles.end());
}

MDXParseResult parseImpl(const std::span<const std::byte> bytes)
{
    BinaryReader reader(bytes, "MDX");
    if (reader.readTag("signature") != "MDLX") {
        throw std::runtime_error("MDX parse error: invalid signature");
    }
    MDXParseResult result;
    std::vector<Material> materials;
    while (!reader.empty()) {
        const auto tag = reader.readTag("chunk tag");
        const auto size = reader.readU32("chunk size");
        auto chunk = reader.subReader(size, tag);
        if (tag == "VERS") {
            if (size < 4U) throw std::runtime_error("MDX parse error: truncated VERS chunk");
            result.model.version = chunk.readU32("model version");
        } else if (tag == "TEXS") {
            parseTextures(chunk, result.model);
        } else if (tag == "MTLS") {
            parseMaterials(chunk, materials);
        } else if (tag == "GEOS") {
            parseGeosets(chunk, result.model);
        }
    }
    applyMaterialFiltering(result.model, materials);
    if (result.model.triangles.empty()) {
        if (result.model.sourceTriangles == 0U) {
            result.warnings.emplace_back("model contains no triangle geosets");
        } else {
            result.warnings.emplace_back("all model triangles were excluded by non-shadow materials");
        }
    }
    return result;
}

Vec3 transformVertex(const Vec3 value, const Vec3 position, const float yawSine,
                     const float yawCosine, const float rollSine, const float rollCosine,
                     const float pitchSine, const float pitchCosine, const Vec3 scale)
{
    const Vec3 scaled{value.x * scale.x, value.y * scale.y, value.z * scale.z};
    const Vec3 rolled{scaled.x,
                      scaled.y * rollCosine - scaled.z * rollSine,
                      scaled.y * rollSine + scaled.z * rollCosine};
    const Vec3 pitched{rolled.x * pitchCosine + rolled.z * pitchSine,
                       rolled.y,
                       -rolled.x * pitchSine + rolled.z * pitchCosine};
    return {pitched.x * yawCosine - pitched.y * yawSine + position.x,
            pitched.x * yawSine + pitched.y * yawCosine + position.y,
            pitched.z + position.z};
}

} // namespace

MDXParseResult parseMDX(const std::span<const std::byte> bytes)
{
    try {
        return parseImpl(bytes);
    } catch (const std::exception& error) {
        MDXParseResult result;
        result.error = error.what();
        return result;
    }
}

std::vector<Triangle> transformTriangles(
    const std::span<const Triangle> triangles, const Vec3 position,
    const float rotation, const Vec3 scale, const float roll, const float pitch)
{
    const auto yawSine = std::sin(rotation);
    const auto yawCosine = std::cos(rotation);
    const auto rollSine = std::sin(roll);
    const auto rollCosine = std::cos(roll);
    const auto pitchSine = std::sin(pitch);
    const auto pitchCosine = std::cos(pitch);
    std::vector<Triangle> result;
    result.reserve(triangles.size());
    for (const auto& triangle : triangles) {
        auto transformed = triangle;
        transformed.a = transformVertex(triangle.a, position, yawSine, yawCosine,
                                        rollSine, rollCosine, pitchSine, pitchCosine, scale);
        transformed.b = transformVertex(triangle.b, position, yawSine, yawCosine,
                                        rollSine, rollCosine, pitchSine, pitchCosine, scale);
        transformed.c = transformVertex(triangle.c, position, yawSine, yawCosine,
                                        rollSine, rollCosine, pitchSine, pitchCosine, scale);
        result.push_back(std::move(transformed));
    }
    return result;
}

} // namespace w3shadow
