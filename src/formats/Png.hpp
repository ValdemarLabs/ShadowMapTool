#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace w3shadow {

[[nodiscard]] std::vector<std::byte> makeShadowPreviewPng(
    std::uint32_t width,
    std::uint32_t height,
    std::span<const std::byte> shadowBytes);

} // namespace w3shadow

