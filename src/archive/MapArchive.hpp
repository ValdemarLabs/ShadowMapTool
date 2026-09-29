#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace w3shadow {

class MapArchive {
public:
    explicit MapArchive(const std::filesystem::path& path);
    ~MapArchive();

    MapArchive(const MapArchive&) = delete;
    MapArchive& operator=(const MapArchive&) = delete;
    MapArchive(MapArchive&& other) noexcept;
    MapArchive& operator=(MapArchive&& other) noexcept;

    [[nodiscard]] bool contains(std::string_view archivePath) const;
    [[nodiscard]] std::vector<std::byte> read(std::string_view archivePath) const;
    [[nodiscard]] std::vector<std::string> listFiles() const;

    static void replaceShadowInCopy(
        const std::filesystem::path& input,
        const std::filesystem::path& output,
        std::span<const std::byte> shadowBytes,
        bool overwriteOutput);

    [[nodiscard]] static std::filesystem::path replaceShadowInPlace(
        const std::filesystem::path& input,
        std::span<const std::byte> shadowBytes);

private:
    explicit MapArchive(void* handle) : handle_(handle) {}
    void close() noexcept;

    void* handle_ = nullptr;
};

} // namespace w3shadow

