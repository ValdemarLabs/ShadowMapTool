#pragma once

#include "geometry/Geometry.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace w3shadow {

class Bvh {
public:
    Bvh() = default;
    explicit Bvh(std::span<const Triangle> triangles);

    void rebuild(std::span<const Triangle> triangles);
    [[nodiscard]] bool intersects(
        Vec3 origin, Vec3 direction, float minimumDistance = 0.01F,
        float maximumDistance = 1000000.0F) const;
    [[nodiscard]] std::size_t triangleCount() const noexcept { return triangles_.size(); }

private:
    struct Node {
        Aabb bounds;
        std::uint32_t first = 0;
        std::uint32_t count = 0;
        std::uint32_t right = 0;
    };

    std::uint32_t buildNode(std::uint32_t first, std::uint32_t count);

    std::vector<Triangle> triangles_;
    std::vector<std::uint32_t> indices_;
    std::vector<Node> nodes_;
};

} // namespace w3shadow
