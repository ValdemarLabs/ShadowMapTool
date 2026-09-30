#pragma once

#include "geometry/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace w3shadow {

struct MDXModel {
    std::uint32_t version = 800;
    std::vector<Triangle> triangles;
};

struct MDXParseResult {
    MDXModel model;
    std::vector<std::string> warnings;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] MDXParseResult parseMDX(std::span<const std::byte> bytes);
[[nodiscard]] std::vector<Triangle> transformTriangles(
    std::span<const Triangle> triangles, Vec3 position, float rotation, Vec3 scale,
    float roll = 0.0F, float pitch = 0.0F);

} // namespace w3shadow
