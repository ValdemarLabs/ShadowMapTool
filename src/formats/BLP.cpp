#include "formats/BLP.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace w3shadow {
namespace {

std::uint32_t u32(const std::span<const std::byte> bytes, const std::size_t offset)
{
    if (offset + 4U > bytes.size()) throw std::runtime_error("truncated header");
    const auto* p = reinterpret_cast<const unsigned char*>(bytes.data() + offset);
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8U) |
           (static_cast<std::uint32_t>(p[2]) << 16U) |
           (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::uint8_t byte(const std::span<const std::byte> bytes, const std::size_t offset)
{
    if (offset >= bytes.size()) throw std::runtime_error("truncated pixel data");
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

std::shared_ptr<AlphaMask> makeMask(const std::uint32_t width, const std::uint32_t height)
{
    if (width == 0U || height == 0U) throw std::runtime_error("invalid dimensions");
    const auto pixels64 = static_cast<std::uint64_t>(width) * height;
    if (pixels64 > 268435456ULL) throw std::runtime_error("alpha mask exceeds safety limit");
    auto result = std::make_shared<AlphaMask>();
    result->width = width;
    result->height = height;
    result->alpha.assign(static_cast<std::size_t>(pixels64), 0xFFU);
    return result;
}

void decodePackedAlpha(
    AlphaMask& mask, const std::span<const std::byte> bytes,
    const std::size_t offset, const unsigned bits)
{
    const auto pixels = mask.alpha.size();
    const auto required = (pixels * bits + 7U) / 8U;
    if (offset > bytes.size() || required > bytes.size() - offset) {
        throw std::runtime_error("truncated alpha plane");
    }
    for (std::size_t index = 0; index < pixels; ++index) {
        const auto packed = byte(bytes, offset + index * bits / 8U);
        if (bits == 1U) {
            mask.alpha[index] = (packed & (1U << (index & 7U))) != 0U ? 0xFFU : 0U;
        } else if (bits == 4U) {
            const auto value = (packed >> ((index & 1U) * 4U)) & 0x0FU;
            mask.alpha[index] = static_cast<std::uint8_t>(value * 17U);
        } else if (bits == 8U) {
            mask.alpha[index] = packed;
        } else {
            throw std::runtime_error("unsupported alpha depth");
        }
    }
}

std::array<std::uint8_t, 8> dxt5Palette(const std::uint8_t a0, const std::uint8_t a1)
{
    std::array<std::uint8_t, 8> result{};
    result[0] = a0;
    result[1] = a1;
    if (a0 > a1) {
        for (unsigned index = 1U; index <= 6U; ++index) {
            result[index + 1U] = static_cast<std::uint8_t>(
                ((7U - index) * a0 + index * a1) / 7U);
        }
    } else {
        for (unsigned index = 1U; index <= 4U; ++index) {
            result[index + 1U] = static_cast<std::uint8_t>(
                ((5U - index) * a0 + index * a1) / 5U);
        }
        result[6] = 0U;
        result[7] = 0xFFU;
    }
    return result;
}

void setBlockAlpha(AlphaMask& mask, const std::uint32_t blockX,
                   const std::uint32_t blockY,
                   const std::array<std::uint8_t, 16>& values)
{
    for (std::uint32_t y = 0; y < 4U; ++y) {
        const auto destinationY = blockY * 4U + y;
        if (destinationY >= mask.height) continue;
        for (std::uint32_t x = 0; x < 4U; ++x) {
            const auto destinationX = blockX * 4U + x;
            if (destinationX >= mask.width) continue;
            mask.alpha[static_cast<std::size_t>(destinationY) * mask.width + destinationX] =
                values[static_cast<std::size_t>(y) * 4U + x];
        }
    }
}

void decodeDxtAlpha(AlphaMask& mask, const std::span<const std::byte> bytes,
                    const std::size_t offset, const unsigned alphaBits)
{
    const auto blocksWide = (mask.width + 3U) / 4U;
    const auto blocksHigh = (mask.height + 3U) / 4U;
    const auto blockSize = alphaBits <= 1U ? 8U : 16U;
    const auto blockCount = static_cast<std::size_t>(blocksWide) * blocksHigh;
    if (offset > bytes.size() || blockCount > (bytes.size() - offset) / blockSize) {
        throw std::runtime_error("truncated DXT alpha data");
    }
    for (std::uint32_t by = 0; by < blocksHigh; ++by) {
        for (std::uint32_t bx = 0; bx < blocksWide; ++bx) {
            const auto base = offset +
                (static_cast<std::size_t>(by) * blocksWide + bx) * blockSize;
            std::array<std::uint8_t, 16> values{};
            values.fill(0xFFU);
            if (alphaBits == 1U) {
                const auto color0 = static_cast<std::uint16_t>(byte(bytes, base)) |
                    (static_cast<std::uint16_t>(byte(bytes, base + 1U)) << 8U);
                const auto color1 = static_cast<std::uint16_t>(byte(bytes, base + 2U)) |
                    (static_cast<std::uint16_t>(byte(bytes, base + 3U)) << 8U);
                if (color0 <= color1) {
                    const auto selectors = u32(bytes, base + 4U);
                    for (unsigned pixel = 0; pixel < 16U; ++pixel) {
                        if (((selectors >> (pixel * 2U)) & 3U) == 3U) values[pixel] = 0U;
                    }
                }
            } else if (alphaBits == 4U) {
                for (unsigned pixel = 0; pixel < 16U; ++pixel) {
                    const auto packed = byte(bytes, base + pixel / 2U);
                    values[pixel] = static_cast<std::uint8_t>(
                        ((packed >> ((pixel & 1U) * 4U)) & 0x0FU) * 17U);
                }
            } else if (alphaBits == 8U) {
                const auto palette = dxt5Palette(byte(bytes, base), byte(bytes, base + 1U));
                std::uint64_t selectors = 0U;
                for (unsigned index = 0; index < 6U; ++index) {
                    selectors |= static_cast<std::uint64_t>(byte(bytes, base + 2U + index)) <<
                                 (index * 8U);
                }
                for (unsigned pixel = 0; pixel < 16U; ++pixel) {
                    values[pixel] = palette[(selectors >> (pixel * 3U)) & 7U];
                }
            } else if (alphaBits != 0U) {
                throw std::runtime_error("unsupported DXT alpha depth");
            }
            setBlockAlpha(mask, bx, by, values);
        }
    }
}

void decodeBlp1JpegAlpha(
    AlphaMask& mask, const std::span<const std::byte> bytes,
    const std::size_t mipOffset, const std::size_t mipSize)
{
#ifdef _WIN32
    if (bytes.size() < 160U) throw std::runtime_error("truncated BLP1 JPEG header");
    const auto sharedHeaderSize = static_cast<std::size_t>(u32(bytes, 156U));
    if (sharedHeaderSize > bytes.size() - 160U || mipOffset > bytes.size() ||
        mipSize > bytes.size() - mipOffset ||
        sharedHeaderSize > (std::numeric_limits<std::size_t>::max)() - mipSize) {
        throw std::runtime_error("invalid BLP1 JPEG ranges");
    }
    std::vector<std::byte> jpeg(sharedHeaderSize + mipSize);
    std::memcpy(jpeg.data(), bytes.data() + 160U, sharedHeaderSize);
    std::memcpy(jpeg.data() + sharedHeaderSize, bytes.data() + mipOffset, mipSize);

    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
        throw std::runtime_error("could not initialize COM for BLP1 JPEG alpha");
    }
    struct Apartment {
        bool owned = false;
        ~Apartment() { if (owned) CoUninitialize(); }
    } apartment{SUCCEEDED(initialized)};

    const auto allocation = GlobalAlloc(GMEM_MOVEABLE, jpeg.size());
    if (allocation == nullptr) throw std::runtime_error("BLP1 JPEG allocation failed");
    auto* destination = GlobalLock(allocation);
    if (destination == nullptr) {
        GlobalFree(allocation);
        throw std::runtime_error("BLP1 JPEG allocation lock failed");
    }
    std::memcpy(destination, jpeg.data(), jpeg.size());
    GlobalUnlock(allocation);

    Microsoft::WRL::ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(allocation, TRUE, &stream))) {
        GlobalFree(allocation);
        throw std::runtime_error("could not create BLP1 JPEG stream");
    }
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        throw std::runtime_error("Windows Imaging Component is unavailable");
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(
            stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder))) {
        throw std::runtime_error("Windows could not decode the BLP1 JPEG stream");
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0U, &frame))) {
        throw std::runtime_error("BLP1 JPEG frame is unavailable");
    }
    UINT width = 0U;
    UINT height = 0U;
    WICPixelFormatGUID pixelFormat{};
    if (FAILED(frame->GetSize(&width, &height)) ||
        FAILED(frame->GetPixelFormat(&pixelFormat)) || width != mask.width ||
        height != mask.height || !IsEqualGUID(pixelFormat, GUID_WICPixelFormat32bppCMYK)) {
        throw std::runtime_error("BLP1 JPEG is not a four-component CMYK/BGRA stream");
    }
    const auto byteCount64 = static_cast<std::uint64_t>(width) * height * 4U;
    if (byteCount64 > (std::numeric_limits<UINT>::max)()) {
        throw std::runtime_error("BLP1 JPEG alpha exceeds Windows decoder limits");
    }
    std::vector<std::uint8_t> components(static_cast<std::size_t>(byteCount64));
    if (FAILED(frame->CopyPixels(nullptr, width * 4U,
                                 static_cast<UINT>(components.size()),
                                 components.data()))) {
        throw std::runtime_error("could not read BLP1 JPEG components");
    }
    for (std::size_t index = 0; index < mask.alpha.size(); ++index) {
        // Warcraft stores linear B,G,R,A components in a JPEG stream that generic
        // codecs identify as CMYK. The fourth component is the material alpha.
        mask.alpha[index] = components[index * 4U + 3U];
    }
#else
    static_cast<void>(mask);
    static_cast<void>(bytes);
    static_cast<void>(mipOffset);
    static_cast<void>(mipSize);
    throw std::runtime_error("BLP1 JPEG alpha decoding requires Windows");
#endif
}

BlpAlphaResult decode(const std::span<const std::byte> bytes)
{
    if (bytes.size() < 148U) throw std::runtime_error("truncated BLP header");
    const std::string magic(reinterpret_cast<const char*>(bytes.data()), 4U);
    if (magic == "BLP1") {
        if (bytes.size() < 156U) throw std::runtime_error("truncated BLP1 header");
        const auto compression = u32(bytes, 4U);
        const auto alphaBits = u32(bytes, 8U);
        const auto width = u32(bytes, 12U);
        const auto height = u32(bytes, 16U);
        const auto mipOffset = static_cast<std::size_t>(u32(bytes, 28U));
        const auto mipSize = static_cast<std::size_t>(u32(bytes, 92U));
        auto mask = makeMask(width, height);
        if (alphaBits == 0U) return {std::move(mask), {}};
        if (compression == 1U) {
            decodePackedAlpha(*mask, bytes, mipOffset + mask->alpha.size(), alphaBits);
        } else if (compression == 0U && alphaBits == 8U) {
            decodeBlp1JpegAlpha(*mask, bytes, mipOffset, mipSize);
        } else {
            throw std::runtime_error(
                "unsupported BLP1 alpha layout (compression=" +
                std::to_string(compression) + ", alpha-bits=" +
                std::to_string(alphaBits) + ", mip-size=" + std::to_string(mipSize) + ")");
        }
        return {std::move(mask), {}};
    }
    if (magic == "BLP2") {
        const auto encoding = byte(bytes, 8U);
        const auto alphaBits = byte(bytes, 9U);
        const auto width = u32(bytes, 12U);
        const auto height = u32(bytes, 16U);
        const auto mipOffset = static_cast<std::size_t>(u32(bytes, 20U));
        auto mask = makeMask(width, height);
        if (encoding == 1U) {
            if (alphaBits != 0U) {
                decodePackedAlpha(*mask, bytes, mipOffset + mask->alpha.size(), alphaBits);
            }
        } else if (encoding == 2U) {
            decodeDxtAlpha(*mask, bytes, mipOffset, alphaBits);
        } else if (encoding == 3U) {
            if (mipOffset > bytes.size() || mask->alpha.size() >
                    (bytes.size() - mipOffset) / 4U) {
                throw std::runtime_error("truncated raw BGRA pixels");
            }
            for (std::size_t index = 0; index < mask->alpha.size(); ++index) {
                mask->alpha[index] = byte(bytes, mipOffset + index * 4U + 3U);
            }
        } else {
            throw std::runtime_error("unsupported BLP2 encoding");
        }
        return {std::move(mask), {}};
    }
    throw std::runtime_error("unsupported texture format (expected BLP1 or BLP2)");
}

} // namespace

BlpAlphaResult decodeBlpAlpha(const std::span<const std::byte> bytes)
{
    try {
        return decode(bytes);
    } catch (const std::exception& error) {
        return {nullptr, error.what()};
    }
}

} // namespace w3shadow
