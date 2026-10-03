#include "geometry/BVH.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace w3shadow {
namespace {

float component(const Vec3 value, const int axis)
{
    if (axis == 0) return value.x;
    if (axis == 1) return value.y;
    return value.z;
}

bool intersectsBounds(
    const Aabb& bounds, const Vec3 origin, const Vec3 direction,
    float minimumDistance, float maximumDistance)
{
    for (int axis = 0; axis < 3; ++axis) {
        const auto originValue = component(origin, axis);
        const auto directionValue = component(direction, axis);
        const auto minimumValue = component(bounds.minimum, axis);
        const auto maximumValue = component(bounds.maximum, axis);
        if (std::abs(directionValue) < 1.0e-8F) {
            if (originValue < minimumValue || originValue > maximumValue) return false;
            continue;
        }
        const auto inverse = 1.0F / directionValue;
        auto nearDistance = (minimumValue - originValue) * inverse;
        auto farDistance = (maximumValue - originValue) * inverse;
        if (nearDistance > farDistance) std::swap(nearDistance, farDistance);
        minimumDistance = std::max(minimumDistance, nearDistance);
        maximumDistance = std::min(maximumDistance, farDistance);
        if (maximumDistance < minimumDistance) return false;
    }
    return true;
}

bool intersectsOpaqueTriangle(
    const Triangle& triangle, const Vec3 origin, const Vec3 direction,
    const float minimumDistance, const float maximumDistance)
{
    constexpr float epsilon = 1.0e-7F;
    const auto edge1 = triangle.b - triangle.a;
    const auto edge2 = triangle.c - triangle.a;
    const auto p = cross(direction, edge2);
    const auto determinant = dot(edge1, p);
    if (std::abs(determinant) < epsilon) return false;
    const auto inverse = 1.0F / determinant;
    const auto translated = origin - triangle.a;
    const auto u = dot(translated, p) * inverse;
    if (u < 0.0F || u > 1.0F) return false;
    const auto q = cross(translated, edge1);
    const auto v = dot(direction, q) * inverse;
    if (v < 0.0F || u + v > 1.0F) return false;
    const auto distance = dot(edge2, q) * inverse;
    if (distance < minimumDistance || distance > maximumDistance) return false;
    if (triangle.alphaMask) {
        const auto w = 1.0F - u - v;
        const auto textureU = triangle.uvA.x * w + triangle.uvB.x * u + triangle.uvC.x * v;
        const auto textureV = triangle.uvA.y * w + triangle.uvB.y * u + triangle.uvC.y * v;
        // Warcraft's Transparent filter is an alpha test near 0.75.
        if (triangle.alphaMask->sample(textureU, textureV) < 192U) return false;
    }
    return true;
}

} // namespace

Bvh::Bvh(const std::span<const Triangle> triangles) { rebuild(triangles); }

void Bvh::rebuild(const std::span<const Triangle> triangles)
{
    triangles_.assign(triangles.begin(), triangles.end());
    indices_.resize(triangles_.size());
    std::iota(indices_.begin(), indices_.end(), 0U);
    nodes_.clear();
    nodes_.reserve(triangles_.empty() ? 0U : triangles_.size() * 2U);
    if (!triangles_.empty()) static_cast<void>(buildNode(0, static_cast<std::uint32_t>(triangles_.size())));
}

std::uint32_t Bvh::buildNode(const std::uint32_t first, const std::uint32_t count)
{
    const auto nodeIndex = static_cast<std::uint32_t>(nodes_.size());
    nodes_.push_back({});
    Aabb nodeBounds;
    Aabb centroidBounds;
    for (std::uint32_t offset = 0; offset < count; ++offset) {
        const auto triangleBounds = bounds(triangles_[indices_[first + offset]]);
        nodeBounds.expand(triangleBounds);
        centroidBounds.expand(triangleBounds.center());
    }
    nodes_[nodeIndex].bounds = nodeBounds;
    nodes_[nodeIndex].first = first;
    nodes_[nodeIndex].count = count;
    if (count <= 8U) return nodeIndex;

    const auto extents = centroidBounds.extent();
    int axis = 0;
    if (extents.y > extents.x) axis = 1;
    if (component(extents, 2) > component(extents, axis)) axis = 2;
    if (component(extents, axis) <= 1.0e-5F) return nodeIndex;

    const auto middle = first + count / 2U;
    std::nth_element(indices_.begin() + first, indices_.begin() + middle,
                     indices_.begin() + first + count,
                     [&](const std::uint32_t left, const std::uint32_t right) {
                         return component(bounds(triangles_[left]).center(), axis) <
                                component(bounds(triangles_[right]).center(), axis);
                     });
    nodes_[nodeIndex].count = 0;
    static_cast<void>(buildNode(first, middle - first));
    nodes_[nodeIndex].right = buildNode(middle, first + count - middle);
    return nodeIndex;
}

bool Bvh::intersects(
    const Vec3 origin, const Vec3 direction, const float minimumDistance,
    const float maximumDistance) const
{
    if (nodes_.empty()) return false;
    std::vector<std::uint32_t> stack;
    stack.reserve(64);
    stack.push_back(0);
    while (!stack.empty()) {
        const auto index = stack.back();
        stack.pop_back();
        const auto& node = nodes_[index];
        if (!intersectsBounds(node.bounds, origin, direction, minimumDistance, maximumDistance)) continue;
        if (node.count != 0U) {
            for (std::uint32_t offset = 0; offset < node.count; ++offset) {
                if (intersectsOpaqueTriangle(triangles_[indices_[node.first + offset]], origin, direction,
                                       minimumDistance, maximumDistance)) return true;
            }
        } else {
            stack.push_back(node.right);
            stack.push_back(index + 1U);
        }
    }
    return false;
}

} // namespace w3shadow
