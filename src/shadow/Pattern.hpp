#pragma once

#include "shadow/ShadowMap.hpp"

#include <optional>
#include <string_view>

namespace w3shadow {

enum class Pattern {
    Black,
    White,
    Checker,
    XGradient,
    YGradient,
    Quadrants
};

[[nodiscard]] std::optional<Pattern> parsePattern(std::string_view name) noexcept;
void generatePattern(ShadowMap& shadowMap, Pattern pattern);

} // namespace w3shadow

