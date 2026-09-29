#include "formats/Png.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace w3shadow {
namespace {

void appendU32(std::vector<std::byte>& output, const std::uint32_t value)
{
    output.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
    output.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    output.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    output.push_back(static_cast<std::byte>(value & 0xFFU));
}

std::uint32_t crc32(const std::span<const std::byte> bytes)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : bytes) {
        crc ^= std::to_integer<std::uint8_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

std::uint32_t adler32(const std::span<const std::byte> bytes)
{
    constexpr std::uint32_t Modulus = 65521U;
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (const auto byte : bytes) {
        a = (a + std::to_integer<std::uint8_t>(byte)) % Modulus;
        b = (b + a) % Modulus;
    }
    return (b << 16U) | a;
}

void appendChunk(
    std::vector<std::byte>& png,
    const std::string_view type,
    const std::span<const std::byte> payload)
{
    appendU32(png, static_cast<std::uint32_t>(payload.size()));
    const auto chunkStart = png.size();
    for (const char character : type) png.push_back(static_cast<std::byte>(character));
    png.insert(png.end(), payload.begin(), payload.end());
    appendU32(png, crc32(std::span<const std::byte>(png).subspan(chunkStart)));
}

std::vector<std::byte> makeStoredZlib(const std::span<const std::byte> raw)
{
    std::vector<std::byte> result;
    result.reserve(raw.size() + raw.size() / 65535U * 5U + 16U);
    result.push_back(std::byte{0x78});
    result.push_back(std::byte{0x01});

    std::size_t offset = 0;
    do {
        const auto remaining = raw.size() - offset;
        const auto blockSize = static_cast<std::uint16_t>(std::min<std::size_t>(remaining, 65535U));
        const bool finalBlock = offset + blockSize == raw.size();
        result.push_back(finalBlock ? std::byte{0x01} : std::byte{0x00});
        result.push_back(static_cast<std::byte>(blockSize & 0xFFU));
        result.push_back(static_cast<std::byte>((blockSize >> 8U) & 0xFFU));
        const auto inverse = static_cast<std::uint16_t>(~blockSize);
        result.push_back(static_cast<std::byte>(inverse & 0xFFU));
        result.push_back(static_cast<std::byte>((inverse >> 8U) & 0xFFU));
        result.insert(result.end(), raw.begin() + static_cast<std::ptrdiff_t>(offset),
                      raw.begin() + static_cast<std::ptrdiff_t>(offset + blockSize));
        offset += blockSize;
    } while (offset < raw.size());

    appendU32(result, adler32(raw));
    return result;
}

} // namespace

std::vector<std::byte> makeShadowPreviewPng(
    const std::uint32_t width,
    const std::uint32_t height,
    const std::span<const std::byte> shadowBytes)
{
    if (width == 0 || height == 0) {
        throw std::invalid_argument("PNG dimensions must be greater than zero");
    }
    const auto pixelCount = static_cast<std::uint64_t>(width) * height;
    if (pixelCount != shadowBytes.size()) {
        throw std::invalid_argument("PNG dimensions do not match the SHD byte count");
    }
    const auto rowBytes = static_cast<std::uint64_t>(width) + 1U;
    const auto rawSize = rowBytes * height;
    if (rawSize > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("PNG preview is too large");
    }

    std::vector<std::byte> raw(static_cast<std::size_t>(rawSize));
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto rowOffset = static_cast<std::size_t>(y) * static_cast<std::size_t>(rowBytes);
        raw[rowOffset] = std::byte{0};
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto shadow = std::to_integer<std::uint8_t>(
                shadowBytes[static_cast<std::size_t>(y) * width + x]);
            raw[rowOffset + 1U + x] = static_cast<std::byte>(255U - shadow);
        }
    }

    std::vector<std::byte> png{
        std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'},
        std::byte{0x0D}, std::byte{0x0A}, std::byte{0x1A}, std::byte{0x0A}};

    std::vector<std::byte> header;
    appendU32(header, width);
    appendU32(header, height);
    header.push_back(std::byte{8});
    header.push_back(std::byte{0});
    header.push_back(std::byte{0});
    header.push_back(std::byte{0});
    header.push_back(std::byte{0});
    appendChunk(png, "IHDR", header);

    const auto compressed = makeStoredZlib(raw);
    appendChunk(png, "IDAT", compressed);
    appendChunk(png, "IEND", {});
    return png;
}

} // namespace w3shadow

