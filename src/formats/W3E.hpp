#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace w3shadow {

struct W3EInfo {
    std::uint32_t version;
    char tileset;
    std::uint32_t vertexWidth;
    std::uint32_t vertexHeight;
    std::uint32_t tileWidth;
    std::uint32_t tileHeight;
};

struct W3EParseResult {
    W3EInfo info{};
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept { return error.empty(); }
};

[[nodiscard]] W3EParseResult parseW3E(std::span<const std::byte> bytes);

} // namespace w3shadow

