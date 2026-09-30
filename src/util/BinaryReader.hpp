#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace w3shadow {

class BinaryReader {
public:
    BinaryReader(std::span<const std::byte> bytes, std::string context)
        : bytes_(bytes), context_(std::move(context)) {}

    [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    [[nodiscard]] bool empty() const noexcept { return offset_ == bytes_.size(); }

    void require(std::size_t count, std::string_view field) const
    {
        if (count > bytes_.size() - offset_) {
            throw std::runtime_error(context_ + " parse error at offset " +
                                     std::to_string(offset_) + ": truncated " +
                                     std::string(field));
        }
    }

    [[nodiscard]] std::span<const std::byte> readBytes(
        std::size_t count, std::string_view field)
    {
        require(count, field);
        const auto result = bytes_.subspan(offset_, count);
        offset_ += count;
        return result;
    }

    void skip(std::size_t count, std::string_view field)
    {
        static_cast<void>(readBytes(count, field));
    }

    [[nodiscard]] std::uint8_t readU8(std::string_view field)
    {
        return std::to_integer<std::uint8_t>(readBytes(1, field).front());
    }

    [[nodiscard]] std::uint16_t readU16(std::string_view field)
    {
        const auto data = readBytes(2, field);
        return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[0])) |
               static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(data[1]) << 8U);
    }

    [[nodiscard]] std::uint32_t readU32(std::string_view field)
    {
        const auto data = readBytes(4, field);
        return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[0])) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[1])) << 8U) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[2])) << 16U) |
               (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(data[3])) << 24U);
    }

    [[nodiscard]] std::int32_t readI32(std::string_view field)
    {
        return std::bit_cast<std::int32_t>(readU32(field));
    }

    [[nodiscard]] float readF32(std::string_view field)
    {
        return std::bit_cast<float>(readU32(field));
    }

    [[nodiscard]] std::string readTag(std::string_view field)
    {
        const auto data = readBytes(4, field);
        return std::string(reinterpret_cast<const char*>(data.data()), data.size());
    }

    [[nodiscard]] std::string readCString(std::string_view field)
    {
        const auto start = offset_;
        while (offset_ < bytes_.size() && bytes_[offset_] != std::byte{0}) ++offset_;
        if (offset_ == bytes_.size()) {
            throw std::runtime_error(context_ + " parse error at offset " +
                                     std::to_string(start) + ": unterminated " +
                                     std::string(field));
        }
        const auto count = offset_ - start;
        ++offset_;
        return std::string(reinterpret_cast<const char*>(bytes_.data() + start), count);
    }

    [[nodiscard]] BinaryReader subReader(std::size_t count, std::string_view field)
    {
        return BinaryReader(readBytes(count, field), context_ + " " + std::string(field));
    }

private:
    std::span<const std::byte> bytes_;
    std::string context_;
    std::size_t offset_ = 0;
};

} // namespace w3shadow
