#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace w3shadow {

class ShadowMap {
public:
    static constexpr std::uint32_t SamplesPerTile = 4;

    ShadowMap(std::uint32_t mapWidthTiles, std::uint32_t mapHeightTiles);

    [[nodiscard]] std::uint32_t widthTiles() const noexcept;
    [[nodiscard]] std::uint32_t heightTiles() const noexcept;
    [[nodiscard]] std::uint32_t widthPixels() const noexcept;
    [[nodiscard]] std::uint32_t heightPixels() const noexcept;

    void clear(bool shadowed) noexcept;
    void set(std::uint32_t x, std::uint32_t y, bool shadowed);
    [[nodiscard]] bool get(std::uint32_t x, std::uint32_t y) const;

    void setValue(std::uint32_t x, std::uint32_t y, std::uint8_t value);
    [[nodiscard]] std::uint8_t value(std::uint32_t x, std::uint32_t y) const;

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

private:
    [[nodiscard]] std::size_t checkedIndex(std::uint32_t x, std::uint32_t y) const;

    std::uint32_t widthTiles_;
    std::uint32_t heightTiles_;
    std::uint32_t widthPixels_;
    std::uint32_t heightPixels_;
    std::vector<std::byte> pixels_;
};

} // namespace w3shadow

