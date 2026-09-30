#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace w3shadow {

struct MapRegion {
    float left = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    float top = 0.0F;
    std::string name;
    std::uint32_t creationNumber = 0;

    [[nodiscard]] bool contains(float x, float y) const noexcept
    {
        return x >= left && x <= right && y >= bottom && y <= top;
    }
};

struct W3RParseResult {
    std::vector<MapRegion> regions;
    std::string error;
    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] W3RParseResult parseW3R(std::span<const std::byte> bytes);
[[nodiscard]] bool isIgnoreShadowRegion(std::string_view name);

} // namespace w3shadow
