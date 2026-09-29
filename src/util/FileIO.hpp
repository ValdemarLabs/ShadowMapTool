#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace w3shadow {

[[nodiscard]] std::vector<std::byte> readBinaryFile(
    const std::filesystem::path& path,
    std::uintmax_t maximumBytes = 1024ULL * 1024ULL * 1024ULL);

void writeBinaryFileAtomic(
    const std::filesystem::path& path,
    std::span<const std::byte> bytes,
    bool overwrite);

[[nodiscard]] std::filesystem::path uniqueSiblingPath(
    const std::filesystem::path& path,
    const char* suffix);

} // namespace w3shadow

