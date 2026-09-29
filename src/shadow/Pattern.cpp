#include "shadow/Pattern.hpp"

#include <cstdint>

namespace w3shadow {
namespace {

std::uint8_t gradientValue(const std::uint32_t position, const std::uint32_t extent)
{
    if (extent <= 1) {
        return 0;
    }
    return static_cast<std::uint8_t>((static_cast<std::uint64_t>(position) * 255U) / (extent - 1U));
}

} // namespace

std::optional<Pattern> parsePattern(const std::string_view name) noexcept
{
    if (name == "black") return Pattern::Black;
    if (name == "white") return Pattern::White;
    if (name == "checker") return Pattern::Checker;
    if (name == "x-gradient") return Pattern::XGradient;
    if (name == "y-gradient") return Pattern::YGradient;
    if (name == "quadrants") return Pattern::Quadrants;
    return std::nullopt;
}

void generatePattern(ShadowMap& shadowMap, const Pattern pattern)
{
    if (pattern == Pattern::Black) {
        shadowMap.clear(true);
        return;
    }
    if (pattern == Pattern::White) {
        shadowMap.clear(false);
        return;
    }

    for (std::uint32_t y = 0; y < shadowMap.heightPixels(); ++y) {
        for (std::uint32_t x = 0; x < shadowMap.widthPixels(); ++x) {
            switch (pattern) {
            case Pattern::Checker:
                shadowMap.set(x, y, ((x / ShadowMap::SamplesPerTile) +
                                     (y / ShadowMap::SamplesPerTile)) % 2U != 0U);
                break;
            case Pattern::XGradient:
                shadowMap.setValue(x, y, gradientValue(x, shadowMap.widthPixels()));
                break;
            case Pattern::YGradient:
                shadowMap.setValue(x, y, gradientValue(y, shadowMap.heightPixels()));
                break;
            case Pattern::Quadrants: {
                const bool right = x >= shadowMap.widthPixels() / 2U;
                const bool bottom = y >= shadowMap.heightPixels() / 2U;
                if (!right && !bottom) {
                    shadowMap.set(x, y, false);
                } else if (right && !bottom) {
                    shadowMap.set(x, y, true);
                } else if (!right) {
                    shadowMap.set(x, y, (x / ShadowMap::SamplesPerTile) % 2U != 0U);
                } else {
                    shadowMap.set(x, y, ((x / ShadowMap::SamplesPerTile) +
                                         (y / ShadowMap::SamplesPerTile)) % 2U != 0U);
                }
                break;
            }
            case Pattern::Black:
            case Pattern::White:
                break;
            }
        }
    }
}

} // namespace w3shadow

