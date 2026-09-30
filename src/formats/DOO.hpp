#pragma once

#include "geometry/Geometry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace w3shadow {

struct DoodadPlacement {
    std::string rawcode;
    std::string skinRawcode;
    std::uint32_t variation = 0;
    Vec3 position;
    float rotation = 0.0F;
    float roll = 0.0F;
    float pitch = 0.0F;
    Vec3 scale{1.0F, 1.0F, 1.0F};
    std::int32_t groupId = 0;
    std::uint8_t flags = 0;
    std::uint8_t life = 100;
    std::uint32_t color = 0;
    std::uint32_t lightCount = 0;
    std::uint32_t editorId = 0;
};

struct SpecialDoodadPlacement {
    std::string rawcode;
    std::uint32_t variation = 0;
    std::int32_t tileX = 0;
    std::int32_t tileY = 0;
};

struct DOOParseResult {
    std::uint32_t version = 0;
    std::uint32_t subversion = 0;
    std::vector<DoodadPlacement> placements;
    std::uint32_t specialVersion = 0;
    std::vector<SpecialDoodadPlacement> specialPlacements;
    std::size_t trailingBytes = 0;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] DOOParseResult parseDOO(std::span<const std::byte> bytes);

} // namespace w3shadow
