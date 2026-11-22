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

struct AlphaKey {
    std::uint32_t frame = 0;
    float value = 1.0F;
    float inTangent = 0.0F;
    float outTangent = 0.0F;
};

struct AlphaTrack {
    std::uint32_t interpolation = 0;
    std::uint32_t globalSequence = noIndex;
    std::vector<AlphaKey> keys;
};

struct Sequence {
    std::string name;
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

struct GeosetVisibility {
    std::uint32_t geosetId = noIndex;
    float alpha = 1.0F;
    AlphaTrack track;
};

// Returns only the requested alpha track, but validates/skips the other tracks
// structurally so tag-like bytes inside key values cannot be mistaken for tags.
AlphaTrack parseAlphaTracks(BinaryReader& reader, const std::string_view alphaTag)
{
    AlphaTrack result;
    while (!reader.empty()) {
        const auto tag = reader.readTag("animation tag");
        std::uint32_t components = 1U;
        if (tag == "KGAC" || tag == "KFC3") components = 3U;
        else if (tag != "KGAO" && tag != "KMTA" && tag != "KMTF" &&
                 tag != "KMTE" && tag != "KFCA" && tag != "KFTC") {
            throw std::runtime_error("MDX parse error: unknown visibility/material track " + tag);
        }
        const auto count = reader.readU32("animation key count");
        AlphaTrack track;
        track.interpolation = reader.readU32("interpolation type");
        track.globalSequence = reader.readU32("global sequence ID");
        if (track.interpolation > 3U) {
            throw std::runtime_error("MDX parse error: invalid animation interpolation");
        }
        const auto keySize = 4U + components * 4U * (track.interpolation > 1U ? 3U : 1U);
        if (count > 1000000U || static_cast<std::uint64_t>(count) * keySize > reader.remaining()) {
            throw std::runtime_error("MDX parse error: invalid animation key count");
        }
        if (tag != alphaTag) {
            reader.skip(static_cast<std::size_t>(count) * keySize, "animation keys");
            continue;
        }
        track.keys.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            AlphaKey key;
            key.frame = reader.readU32("key frame");
            key.value = reader.readF32("key alpha");
            if (track.interpolation > 1U) {
                key.inTangent = reader.readF32("alpha in tangent");
                key.outTangent = reader.readF32("alpha out tangent");
            }
            if (!std::isfinite(key.value) || !std::isfinite(key.inTangent) ||
                !std::isfinite(key.outTangent) ||
                (!track.keys.empty() && key.frame < track.keys.back().frame)) {
                throw std::runtime_error("MDX parse error: invalid alpha animation key");
            }
            track.keys.push_back(key);
        }
        result = std::move(track);
    }
    return result;
}

float sampleAlpha(const AlphaTrack& track, const float fallback,
                  const std::optional<Sequence>& stand, const std::vector<std::uint32_t>& globals)
{
    if (track.keys.empty()) return fallback;
    std::uint32_t start = 0U;
    std::uint32_t end = 0U;
    if (track.globalSequence != noIndex) {
        if (track.globalSequence >= globals.size()) {
            throw std::runtime_error("MDX parse error: invalid alpha global sequence ID");
        }
        end = globals[track.globalSequence];
    } else if (stand) {
        start = stand->start;
        end = stand->end;
    } else {
        return fallback;
    }
    const auto first = std::lower_bound(track.keys.begin(), track.keys.end(), start,
        [](const AlphaKey& key, const std::uint32_t frame) { return key.frame < frame; });
    const auto after = std::upper_bound(first, track.keys.end(), end,
        [](const std::uint32_t frame, const AlphaKey& key) { return frame < key.frame; });
    // An animated alpha with no keys in this sequence has the alpha default (1),
    // not the values from a different sequence such as Portrait or Death.
    if (first == after) return 1.0F;
    const auto next = std::upper_bound(first, after, start,
        [](const std::uint32_t frame, const AlphaKey& key) { return frame < key.frame; });
    if (next != first) return (next - 1)->value; // Exact start key (last wins on duplicates).
    if (after - first == 1 || start == end) return first->value;

    // At the interval start, interpolate across the loop seam when no start key
    // exists. Only keys inside the chosen interval take part.
    const auto& left = *(after - 1);
    const auto& right = *first;
    if (track.interpolation == 0U) return left.value;
    const auto leftTime = static_cast<double>(left.frame) - (end - start);
    const auto span = static_cast<double>(right.frame) - leftTime;
    if (span <= 0.0) return right.value;
    const auto t = static_cast<float>((start - leftTime) / span);
    const auto u = 1.0F - t;
    if (track.interpolation == 1U) return left.value * u + right.value * t;
    if (track.interpolation == 2U) {
        const auto t2 = t * t;
        const auto t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * left.value + (t3 - 2 * t2 + t) * left.outTangent +
               (-2 * t3 + 3 * t2) * right.value + (t3 - t2) * right.inTangent;
    }
    return u * u * u * left.value + 3 * u * u * t * left.outTangent +
           3 * u * t * t * right.inTangent + t * t * t * right.value;
}

void parseSequences(BinaryReader& chunk, std::optional<Sequence>& stand)
{
    while (!chunk.empty()) {
        auto record = chunk.subReader(132U, "sequence");
        const auto nameBytes = record.readBytes(80U, "sequence name");
        const auto terminator = std::find(nameBytes.begin(), nameBytes.end(), std::byte{0});
        Sequence sequence;
        sequence.name.assign(reinterpret_cast<const char*>(nameBytes.data()),
                             static_cast<std::size_t>(terminator - nameBytes.begin()));
        auto name = sequence.name;
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        sequence.start = record.readU32("sequence start");
        sequence.end = record.readU32("sequence end");
        if (sequence.end < sequence.start) throw std::runtime_error("MDX parse error: reversed sequence interval");
        const auto isStand = name == "stand" || name.starts_with("stand ") || name.starts_with("stand-");
        if (isStand && (!stand || name == "stand")) stand = std::move(sequence);
    }
}

void parseGeosetVisibility(BinaryReader& chunk, std::vector<GeosetVisibility>& animations)
{
    while (!chunk.empty()) {
        const auto size = chunk.readU32("geoset animation size");
        if (size < 28U || size - 4U > chunk.remaining()) {
            throw std::runtime_error("MDX parse error: invalid geoset animation size");
        }
        auto record = chunk.subReader(size - 4U, "geoset animation");
        GeosetVisibility animation;
        animation.alpha = record.readF32("geoset alpha");
        record.skip(16U, "geoset animation flags and color");
        animation.geosetId = record.readU32("animated geoset ID");
        animation.track = parseAlphaTracks(record, "KGAO");
        animations.push_back(std::move(animation));
    }
}

struct MaterialLayer {
    std::uint32_t filterMode = 0;
    std::uint32_t textureIndex = noIndex;
    float alpha = 1.0F;
    AlphaTrack track;
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

void parseMaterials(BinaryReader& chunk, std::vector<Material>& materials, const std::uint32_t version)
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
                if (version <= 1800U) {
                    if (version > 800U) layer.skip(4U, "emissive gain");
                    if (version > 900U) layer.skip(20U, "fresnel fields");
                    if (version >= 1100U) {
                        static_cast<void>(layer.readU32("shader type"));
                        const auto textureCount = layer.readU32("subtexture count");
                        if (textureCount > 100000U ||
                            static_cast<std::uint64_t>(textureCount) * 8U > layer.remaining()) {
                            throw std::runtime_error("MDX parse error: invalid subtexture count");
                        }
                        // Modern files keep a placeholder in the old texture ID
                        // field. Slot zero supplies the actual diffuse/alpha texture.
                        parsed.textureIndex = noIndex;
                        for (std::uint32_t texture = 0; texture < textureCount; ++texture) {
                            const auto textureId = layer.readU32("subtexture ID");
                            const auto slot = layer.readU32("subtexture slot");
                            if (slot == 0U) parsed.textureIndex = textureId;
                            auto probe = layer;
                            if (probe.remaining() >= 4U && probe.readTag("optional texture track") == "KMTF") {
                                const auto count = probe.readU32("texture key count");
                                const auto mode = probe.readU32("texture interpolation");
                                static_cast<void>(probe.readU32("texture global sequence"));
                                if (count > 1000000U || mode > 3U) {
                                    throw std::runtime_error("MDX parse error: invalid texture animation track");
                                }
                                const auto keySize = 4U + 4U * (mode > 1U ? 3U : 1U);
                                layer.skip(16U + static_cast<std::size_t>(count) * keySize, "texture animation");
                            }
                        }
                    }
                    parsed.track = parseAlphaTracks(layer, "KMTA");
                }
                material.push_back(std::move(parsed));
            }
        }
        materials.push_back(std::move(material));
    }
}

void parseGeosets(BinaryReader& chunk, MDXModel& model,
                  std::vector<std::pair<std::size_t, std::size_t>>& ranges)
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

        const auto firstTriangle = model.triangles.size();
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
        ranges.emplace_back(firstTriangle, model.triangles.size());
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
        if (!std::isfinite(triangle.shadowOpacity) || triangle.shadowOpacity <= 0.001F) {
            ++model.visibilityFilteredTriangles;
            continue;
        }
        bool castsShadow = true;
        bool alphaTested = false;
        std::uint32_t textureIndex = noIndex;
        float opacity = 1.0F;
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
                const auto effectiveAlpha = triangle.shadowOpacity * layer.alpha;
                if ((layer.filterMode == 1U || layer.filterMode == 2U) && !castsShadow &&
                    effectiveAlpha >= 192.0F / 255.0F) {
                    castsShadow = true;
                    alphaTested = layer.textureIndex < model.textures.size() &&
                                  !model.textures[layer.textureIndex].path.empty();
                    textureIndex = layer.textureIndex;
                    opacity = std::clamp(effectiveAlpha, 0.0F, 1.0F);
                }
                // Blend can be an opaque prop surface with cutout texels. Use the
                // same binary cutoff as Transparent; additive/modulate stay excluded.
            }
        }
        if (!castsShadow) {
            ++model.materialFilteredTriangles;
            continue;
        }
        triangle.shadowOpacity = opacity;
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
    std::vector<std::span<const std::byte>> materialChunks;
    std::vector<std::pair<std::size_t, std::size_t>> geosetRanges;
    std::vector<GeosetVisibility> visibility;
    std::vector<std::uint32_t> globals;
    std::optional<Sequence> stand;
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
            materialChunks.push_back(chunk.readBytes(chunk.remaining(), "materials"));
        } else if (tag == "GEOS") {
            parseGeosets(chunk, result.model, geosetRanges);
        } else if (tag == "SEQS") {
            parseSequences(chunk, stand);
        } else if (tag == "GLBS") {
            while (!chunk.empty()) globals.push_back(chunk.readU32("global sequence duration"));
        } else if (tag == "GEOA") {
            parseGeosetVisibility(chunk, visibility);
        }
    }
    for (const auto materialBytes : materialChunks) {
        BinaryReader chunk(materialBytes, "MDX materials");
        parseMaterials(chunk, materials, result.model.version);
    }
    if (!materials.empty() && result.model.version > 1800U) {
        result.warnings.emplace_back("animated material visibility is unsupported for MDX version " +
                                     std::to_string(result.model.version) + "; using static material alpha");
    }
    for (auto& material : materials) {
        for (auto& layer : material) layer.alpha = sampleAlpha(layer.track, layer.alpha, stand, globals);
    }
    for (const auto& animation : visibility) {
        if (animation.geosetId == noIndex) continue;
        if (animation.geosetId >= geosetRanges.size()) {
            throw std::runtime_error("MDX parse error: animated geoset ID exceeds geoset count");
        }
        const auto alpha = sampleAlpha(animation.track, animation.alpha, stand, globals);
        const auto [first, after] = geosetRanges[animation.geosetId];
        for (auto index = first; index < after; ++index) {
            result.model.triangles[index].shadowOpacity = std::clamp(alpha, 0.0F, 1.0F);
        }
    }
    applyMaterialFiltering(result.model, materials);
    if (result.model.visibilityFilteredTriangles != 0U) {
        const auto sample = stand ? "start of " + stand->name + " (frame " + std::to_string(stand->start) + ")"
                                  : "static/global visibility (no Stand sequence)";
        result.warnings.push_back("visibility excluded " + std::to_string(result.model.visibilityFilteredTriangles) +
                                  " of " + std::to_string(result.model.sourceTriangles) + " triangles at " + sample);
    }
    if (result.model.triangles.empty()) {
        if (result.model.sourceTriangles == 0U) {
            result.warnings.emplace_back("model contains no triangle geosets");
        } else {
            result.warnings.emplace_back("all model triangles were excluded by visibility or non-shadow materials");
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
