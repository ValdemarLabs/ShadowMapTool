#include "formats/W3E.hpp"

#include <cstring>
#include <limits>
#include <sstream>

namespace w3shadow {
namespace {

class Reader {
public:
    explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

    bool readU8(std::uint8_t& result) { return readRaw(&result, sizeof(result)); }

    bool readU32(std::uint32_t& result)
    {
        std::uint8_t raw[4]{};
        if (!readRaw(raw, sizeof(raw))) return false;
        result = static_cast<std::uint32_t>(raw[0]) |
                 (static_cast<std::uint32_t>(raw[1]) << 8U) |
                 (static_cast<std::uint32_t>(raw[2]) << 16U) |
                 (static_cast<std::uint32_t>(raw[3]) << 24U);
        return true;
    }

    bool skip(const std::size_t count)
    {
        if (count > bytes_.size() - offset_) return false;
        offset_ += count;
        return true;
    }

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

private:
    bool readRaw(void* destination, const std::size_t count)
    {
        if (count > bytes_.size() - offset_) return false;
        std::memcpy(destination, bytes_.data() + offset_, count);
        offset_ += count;
        return true;
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_ = 0;
};

W3EParseResult failure(const Reader& reader, const std::string& message)
{
    std::ostringstream stream;
    stream << "W3E parse error at offset 0x" << std::hex << reader.offset() << ": " << message;
    return W3EParseResult{{}, stream.str()};
}

bool checkedIdBytes(const std::uint32_t count, std::size_t& byteCount)
{
    if (count > 4096U) return false;
    const auto wide = static_cast<std::uint64_t>(count) * 4U;
    if (wide > std::numeric_limits<std::size_t>::max()) return false;
    byteCount = static_cast<std::size_t>(wide);
    return true;
}

} // namespace

W3EParseResult parseW3E(const std::span<const std::byte> bytes)
{
    Reader reader(bytes);
    std::uint8_t signature[4]{};
    for (auto& byte : signature) {
        if (!reader.readU8(byte)) return failure(reader, "missing W3E signature");
    }
    if (signature[0] != 'W' || signature[1] != '3' || signature[2] != 'E' || signature[3] != '!') {
        return failure(reader, "invalid W3E signature");
    }

    W3EInfo info{};
    if (!reader.readU32(info.version)) return failure(reader, "missing format version");

    std::uint8_t tileset = 0;
    if (!reader.readU8(tileset)) return failure(reader, "missing tileset");
    info.tileset = static_cast<char>(tileset);

    std::uint32_t customTilesets = 0;
    std::uint32_t groundCount = 0;
    if (!reader.readU32(customTilesets) || !reader.readU32(groundCount)) {
        return failure(reader, "truncated tileset header");
    }
    if (customTilesets > 1U) return failure(reader, "invalid custom-tileset flag");

    std::size_t idBytes = 0;
    if (!checkedIdBytes(groundCount, idBytes) || !reader.skip(idBytes)) {
        return failure(reader, "invalid or truncated ground tile list");
    }

    std::uint32_t cliffCount = 0;
    if (!reader.readU32(cliffCount) || !checkedIdBytes(cliffCount, idBytes) || !reader.skip(idBytes)) {
        return failure(reader, "invalid or truncated cliff tile list");
    }

    if (!reader.readU32(info.vertexWidth) || !reader.readU32(info.vertexHeight)) {
        return failure(reader, "missing terrain dimensions");
    }
    if (info.vertexWidth < 2U || info.vertexHeight < 2U) {
        return failure(reader, "terrain vertex dimensions must be at least 2 by 2");
    }
    if (info.vertexWidth > 8193U || info.vertexHeight > 8193U) {
        return failure(reader, "terrain dimensions exceed the parser safety limit");
    }

    info.tileWidth = info.vertexWidth - 1U;
    info.tileHeight = info.vertexHeight - 1U;

    if (!reader.skip(8U)) {
        return failure(reader, "missing terrain center offsets");
    }
    const auto vertexBytes = static_cast<std::uint64_t>(info.vertexWidth) *
                             info.vertexHeight * 7U;
    if (vertexBytes > std::numeric_limits<std::size_t>::max() ||
        !reader.skip(static_cast<std::size_t>(vertexBytes))) {
        return failure(reader, "truncated terrain vertex data");
    }
    return W3EParseResult{info, {}};
}

} // namespace w3shadow

