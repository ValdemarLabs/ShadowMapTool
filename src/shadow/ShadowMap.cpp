#include "shadow/ShadowMap.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace w3shadow {
namespace {

constexpr std::uint64_t MaxShadowBytes = 1024ULL * 1024ULL * 1024ULL;

} // namespace

ShadowMap::ShadowMap(const std::uint32_t mapWidthTiles, const std::uint32_t mapHeightTiles)
    : widthTiles_(mapWidthTiles),
      heightTiles_(mapHeightTiles),
      widthPixels_(0),
      heightPixels_(0)
{
    if (mapWidthTiles == 0 || mapHeightTiles == 0) {
        throw std::invalid_argument("map dimensions must be greater than zero");
    }
    if (mapWidthTiles > std::numeric_limits<std::uint32_t>::max() / SamplesPerTile ||
        mapHeightTiles > std::numeric_limits<std::uint32_t>::max() / SamplesPerTile) {
        throw std::overflow_error("shadow-map pixel dimensions overflow uint32");
    }

    widthPixels_ = mapWidthTiles * SamplesPerTile;
    heightPixels_ = mapHeightTiles * SamplesPerTile;
    const auto byteCount = static_cast<std::uint64_t>(widthPixels_) * heightPixels_;
    if (byteCount > MaxShadowBytes || byteCount > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("shadow map exceeds the 1 GiB safety limit");
    }

    pixels_.resize(static_cast<std::size_t>(byteCount), std::byte{0});
}

std::uint32_t ShadowMap::widthTiles() const noexcept { return widthTiles_; }
std::uint32_t ShadowMap::heightTiles() const noexcept { return heightTiles_; }
std::uint32_t ShadowMap::widthPixels() const noexcept { return widthPixels_; }
std::uint32_t ShadowMap::heightPixels() const noexcept { return heightPixels_; }

void ShadowMap::clear(const bool shadowed) noexcept
{
    std::fill(pixels_.begin(), pixels_.end(), shadowed ? std::byte{0xFF} : std::byte{0x00});
}

void ShadowMap::set(const std::uint32_t x, const std::uint32_t y, const bool shadowed)
{
    pixels_[checkedIndex(x, y)] = shadowed ? std::byte{0xFF} : std::byte{0x00};
}

bool ShadowMap::get(const std::uint32_t x, const std::uint32_t y) const
{
    return pixels_[checkedIndex(x, y)] != std::byte{0x00};
}

void ShadowMap::setValue(const std::uint32_t x, const std::uint32_t y, const std::uint8_t value)
{
    pixels_[checkedIndex(x, y)] = static_cast<std::byte>(value);
}

std::uint8_t ShadowMap::value(const std::uint32_t x, const std::uint32_t y) const
{
    return std::to_integer<std::uint8_t>(pixels_[checkedIndex(x, y)]);
}

std::span<const std::byte> ShadowMap::bytes() const noexcept
{
    return pixels_;
}

std::vector<std::byte> ShadowMap::warcraftBytes() const
{
    std::vector<std::byte> result(pixels_.size());
    const auto rowSize = static_cast<std::size_t>(widthPixels_);
    for (std::uint32_t y = 0; y < heightPixels_; ++y) {
        const auto sourceOffset = static_cast<std::size_t>(y) * rowSize;
        const auto destinationOffset = static_cast<std::size_t>(heightPixels_ - 1U - y) * rowSize;
        std::copy_n(pixels_.begin() + static_cast<std::ptrdiff_t>(sourceOffset), rowSize,
                    result.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
    }
    return result;
}

void ShadowMap::loadWarcraftBytes(const std::span<const std::byte> bytes)
{
    if (bytes.size() != pixels_.size()) {
        throw std::invalid_argument("SHD byte count does not match the shadow-map dimensions");
    }

    const auto rowSize = static_cast<std::size_t>(widthPixels_);
    for (std::uint32_t y = 0; y < heightPixels_; ++y) {
        const auto sourceOffset = static_cast<std::size_t>(heightPixels_ - 1U - y) * rowSize;
        const auto destinationOffset = static_cast<std::size_t>(y) * rowSize;
        std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(sourceOffset), rowSize,
                    pixels_.begin() + static_cast<std::ptrdiff_t>(destinationOffset));
    }
}

std::size_t ShadowMap::checkedIndex(const std::uint32_t x, const std::uint32_t y) const
{
    if (x >= widthPixels_ || y >= heightPixels_) {
        throw std::out_of_range("shadow-map coordinate is outside the image");
    }
    return static_cast<std::size_t>(y) * widthPixels_ + x;
}

} // namespace w3shadow

