#include "formats/MDX.hpp"

#include "util/BinaryReader.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace w3shadow {
namespace {

constexpr std::uint32_t maximumVertices = 10000000U;
constexpr std::uint32_t maximumIndices = 30000000U;

void requireTag(BinaryReader& reader, const std::string_view expected)
{
    const auto actual = reader.readTag(expected);
    if (actual != expected) {
        throw std::runtime_error("MDX geoset parse error: expected " + std::string(expected) +
                                 ", found " + actual);
    }
}

void parseGeosets(BinaryReader& chunk, MDXModel& model)
{
    while (!chunk.empty()) {
        const auto inclusiveSize = chunk.readU32("geoset size");
        if (inclusiveSize < 4U || inclusiveSize - 4U > chunk.remaining()) {
            throw std::runtime_error("MDX parse error: invalid geoset size");
        }
        auto geoset = chunk.subReader(inclusiveSize - 4U, "geoset");
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
        for (std::size_t index = 0; index + 2U < indices.size(); index += 3U) {
            const auto a = indices[index];
            const auto b = indices[index + 1U];
            const auto c = indices[index + 2U];
            if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) {
                throw std::runtime_error("MDX parse error: face index exceeds vertex count");
            }
            model.triangles.push_back({vertices[a], vertices[b], vertices[c]});
        }
    }
}

MDXParseResult parseImpl(const std::span<const std::byte> bytes)
{
    BinaryReader reader(bytes, "MDX");
    if (reader.readTag("signature") != "MDLX") {
        throw std::runtime_error("MDX parse error: invalid signature");
    }
    MDXParseResult result;
    while (!reader.empty()) {
        const auto tag = reader.readTag("chunk tag");
        const auto size = reader.readU32("chunk size");
        auto chunk = reader.subReader(size, tag);
        if (tag == "VERS") {
            if (size < 4U) throw std::runtime_error("MDX parse error: truncated VERS chunk");
            result.model.version = chunk.readU32("model version");
        } else if (tag == "GEOS") {
            parseGeosets(chunk, result.model);
        }
    }
    if (result.model.triangles.empty()) {
        result.warnings.emplace_back("model contains no triangle geosets");
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
        result.push_back({transformVertex(triangle.a, position, yawSine, yawCosine,
                                          rollSine, rollCosine, pitchSine, pitchCosine, scale),
                          transformVertex(triangle.b, position, yawSine, yawCosine,
                                          rollSine, rollCosine, pitchSine, pitchCosine, scale),
                          transformVertex(triangle.c, position, yawSine, yawCosine,
                                          rollSine, rollCosine, pitchSine, pitchCosine, scale)});
    }
    return result;
}

} // namespace w3shadow
