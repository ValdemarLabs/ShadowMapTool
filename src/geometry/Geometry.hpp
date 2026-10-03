#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace w3shadow {

struct Vec2 {
    float x = 0.0F;
    float y = 0.0F;
};

struct Vec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

[[nodiscard]] inline Vec3 operator+(const Vec3 a, const Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] inline Vec3 operator-(const Vec3 a, const Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] inline Vec3 operator*(const Vec3 value, const float scalar)
{
    return {value.x * scalar, value.y * scalar, value.z * scalar};
}

[[nodiscard]] inline float dot(const Vec3 a, const Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] inline Vec3 cross(const Vec3 a, const Vec3 b)
{
    return {a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x};
}

[[nodiscard]] inline Vec3 normalized(const Vec3 value)
{
    const auto length = std::sqrt(dot(value, value));
    if (!(length > 0.0F) || !std::isfinite(length)) return {};
    return value * (1.0F / length);
}

struct AlphaMask {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> alpha;

    [[nodiscard]] std::uint8_t sample(float u, float v) const noexcept;
};

struct Triangle {
    Vec3 a;
    Vec3 b;
    Vec3 c;
    Vec2 uvA;
    Vec2 uvB;
    Vec2 uvC;
    std::uint32_t materialId = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t shadowTextureIndex = std::numeric_limits<std::uint32_t>::max();
    bool alphaTested = false;
    std::shared_ptr<const AlphaMask> alphaMask;
};

struct Aabb {
    Vec3 minimum{std::numeric_limits<float>::infinity(),
                 std::numeric_limits<float>::infinity(),
                 std::numeric_limits<float>::infinity()};
    Vec3 maximum{-std::numeric_limits<float>::infinity(),
                 -std::numeric_limits<float>::infinity(),
                 -std::numeric_limits<float>::infinity()};

    void expand(const Vec3 value)
    {
        minimum.x = std::min(minimum.x, value.x);
        minimum.y = std::min(minimum.y, value.y);
        minimum.z = std::min(minimum.z, value.z);
        maximum.x = std::max(maximum.x, value.x);
        maximum.y = std::max(maximum.y, value.y);
        maximum.z = std::max(maximum.z, value.z);
    }

    void expand(const Aabb& value)
    {
        expand(value.minimum);
        expand(value.maximum);
    }

    [[nodiscard]] Vec3 center() const { return (minimum + maximum) * 0.5F; }
    [[nodiscard]] Vec3 extent() const { return maximum - minimum; }
};

[[nodiscard]] inline Aabb bounds(const Triangle& triangle)
{
    Aabb result;
    result.expand(triangle.a);
    result.expand(triangle.b);
    result.expand(triangle.c);
    return result;
}

inline std::uint8_t AlphaMask::sample(float u, float v) const noexcept
{
    if (width == 0U || height == 0U || alpha.size() !=
            static_cast<std::size_t>(width) * height || !std::isfinite(u) ||
            !std::isfinite(v)) {
        return 0xFFU;
    }
    u -= std::floor(u);
    v -= std::floor(v);
    if (u < 0.0F) u += 1.0F;
    if (v < 0.0F) v += 1.0F;
    const auto x = std::min(static_cast<std::uint32_t>(u * static_cast<float>(width)),
                            width - 1U);
    const auto y = std::min(static_cast<std::uint32_t>(v * static_cast<float>(height)),
                            height - 1U);
    return alpha[static_cast<std::size_t>(y) * width + x];
}

} // namespace w3shadow
