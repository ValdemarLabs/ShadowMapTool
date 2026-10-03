#pragma once

#include "geometry/Geometry.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace w3shadow {

struct BlpAlphaResult {
    std::shared_ptr<AlphaMask> mask;
    std::string error;

    [[nodiscard]] explicit operator bool() const noexcept
    {
        return mask != nullptr && error.empty();
    }
};

[[nodiscard]] BlpAlphaResult decodeBlpAlpha(std::span<const std::byte> bytes);

} // namespace w3shadow
