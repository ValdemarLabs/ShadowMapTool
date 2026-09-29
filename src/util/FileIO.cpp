#include "util/FileIO.hpp"

#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

namespace w3shadow {

std::filesystem::path uniqueSiblingPath(const std::filesystem::path& path, const char* suffix)
{
    for (unsigned int index = 0; index < 1000U; ++index) {
        auto candidate = path;
        candidate += suffix;
        if (index != 0U) candidate += "." + std::to_string(index);
        if (!std::filesystem::exists(candidate)) return candidate;
    }
    throw std::runtime_error("unable to allocate a unique sibling path for " + path.string());
}

std::vector<std::byte> readBinaryFile(
    const std::filesystem::path& path,
    const std::uintmax_t maximumBytes)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error) throw std::runtime_error("unable to determine file size: " + path.string());
    if (size > maximumBytes || size > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("file exceeds the configured safety limit: " + path.string());
    }

    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open file for reading: " + path.string());
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!input) throw std::runtime_error("unable to read complete file: " + path.string());
    return bytes;
}

void writeBinaryFileAtomic(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes,
    const bool overwrite)
{
    if (std::filesystem::exists(path) && !overwrite) {
        throw std::runtime_error("output already exists (use --force): " + path.string());
    }

    const auto temporary = uniqueSiblingPath(path, ".w3shadow.tmp");
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("unable to open temporary output: " + temporary.string());
        if (!bytes.empty()) {
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
        output.close();
        if (!output) throw std::runtime_error("unable to finish temporary output: " + temporary.string());

        if (std::filesystem::exists(path)) std::filesystem::remove(path);
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace w3shadow

