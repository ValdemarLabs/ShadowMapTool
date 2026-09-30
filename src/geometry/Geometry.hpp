#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace w3shadow {

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

struct Triangle {
    Vec3 a;
    Vec3 b;
    Vec3 c;
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

} // namespace w3shadow
