#include "archive/MapArchive.hpp"

#include "util/FileIO.hpp"

#include <StormLib.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace w3shadow {
namespace {

std::runtime_error stormError(const std::string& action)
{
    return std::runtime_error(action + " (StormLib error " + std::to_string(GetLastError()) + ")");
}

std::string archiveName(const std::string_view path)
{
    if (path.empty() || path.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("archive path is empty or contains a NUL byte");
    }
    return std::string(path);
}

void writeShadow(void* archiveHandle, const std::span<const std::byte> shadowBytes)
{
    if (shadowBytes.size() > std::numeric_limits<DWORD>::max()) {
        throw std::length_error("war3map.shd is too large for the MPQ API");
    }

    HANDLE file = nullptr;
    const DWORD flags = MPQ_FILE_REPLACEEXISTING | MPQ_FILE_COMPRESS;
    if (!SFileCreateFile(archiveHandle, "war3map.shd", 0,
                         static_cast<DWORD>(shadowBytes.size()), 0, flags, &file)) {
        throw stormError("unable to create war3map.shd in the staged map");
    }

    if (!shadowBytes.empty() &&
        !SFileWriteFile(file, shadowBytes.data(), static_cast<DWORD>(shadowBytes.size()),
                        MPQ_COMPRESSION_ZLIB)) {
        SFileFinishFile(file);
        throw stormError("unable to write war3map.shd to the staged map");
    }
    if (!SFileFinishFile(file)) {
        throw stormError("unable to finish war3map.shd in the staged map");
    }
}

void createValidatedCopy(
    const std::filesystem::path& input,
    const std::filesystem::path& staged,
    const std::span<const std::byte> shadowBytes)
{
    std::filesystem::copy_file(input, staged, std::filesystem::copy_options::none);

    HANDLE archive = nullptr;
    if (!SFileOpenArchive(staged.c_str(), 0, 0, &archive)) {
        throw stormError("unable to open staged map for writing: " + staged.string());
    }
    try {
        writeShadow(archive, shadowBytes);
        if (!SFileCloseArchive(archive)) {
            archive = nullptr;
            throw stormError("unable to close staged map after writing");
        }
        archive = nullptr;
    } catch (...) {
        if (archive != nullptr) SFileCloseArchive(archive);
        throw;
    }

    MapArchive validation(staged);
    if (validation.read("war3map.shd") !=
        std::vector<std::byte>(shadowBytes.begin(), shadowBytes.end())) {
        throw std::runtime_error("staged map validation failed: war3map.shd differs after round trip");
    }
}

} // namespace

MapArchive::MapArchive(const std::filesystem::path& path)
{
    HANDLE archive = nullptr;
    if (!SFileOpenArchive(path.c_str(), 0, MPQ_OPEN_READ_ONLY, &archive)) {
        throw stormError("unable to open map archive: " + path.string());
    }
    handle_ = archive;
}

MapArchive::~MapArchive() { close(); }

MapArchive::MapArchive(MapArchive&& other) noexcept : handle_(other.handle_)
{
    other.handle_ = nullptr;
}

MapArchive& MapArchive::operator=(MapArchive&& other) noexcept
{
    if (this != &other) {
        close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

void MapArchive::close() noexcept
{
    if (handle_ != nullptr) {
        SFileCloseArchive(handle_);
        handle_ = nullptr;
    }
}

bool MapArchive::contains(const std::string_view archivePath) const
{
    const auto name = archiveName(archivePath);
    return SFileHasFile(handle_, name.c_str()) != FALSE;
}

std::vector<std::byte> MapArchive::read(const std::string_view archivePath) const
{
    const auto name = archiveName(archivePath);
    HANDLE file = nullptr;
    if (!SFileOpenFileEx(handle_, name.c_str(), SFILE_OPEN_FROM_MPQ, &file)) {
        throw stormError("unable to open archive file: " + name);
    }

    try {
        DWORD highSize = 0;
        const DWORD lowSize = SFileGetFileSize(file, &highSize);
        if (lowSize == SFILE_INVALID_SIZE && GetLastError() != ERROR_SUCCESS) {
            throw stormError("unable to determine archive file size: " + name);
        }
        const auto size = (static_cast<std::uint64_t>(highSize) << 32U) | lowSize;
        if (size > 1024ULL * 1024ULL * 1024ULL || size > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("archive file exceeds the 1 GiB safety limit: " + name);
        }

        std::vector<std::byte> result(static_cast<std::size_t>(size));
        DWORD bytesRead = 0;
        if (!result.empty() &&
            !SFileReadFile(file, result.data(), static_cast<DWORD>(result.size()), &bytesRead, nullptr)) {
            throw stormError("unable to read archive file: " + name);
        }
        if (bytesRead != result.size()) {
            throw std::runtime_error("short read from archive file: " + name);
        }
        SFileCloseFile(file);
        return result;
    } catch (...) {
        SFileCloseFile(file);
        throw;
    }
}

std::vector<std::string> MapArchive::listFiles() const
{
    std::vector<std::string> result;
    SFILE_FIND_DATA data{};
    HANDLE find = SFileFindFirstFile(handle_, "*", &data, nullptr);
    if (find == nullptr) return result;

    do {
        result.emplace_back(data.cFileName);
    } while (SFileFindNextFile(find, &data));
    SFileFindClose(find);
    std::sort(result.begin(), result.end());
    return result;
}

void MapArchive::replaceShadowInCopy(
    const std::filesystem::path& input,
    const std::filesystem::path& output,
    const std::span<const std::byte> shadowBytes,
    const bool overwriteOutput)
{
    std::error_code inputError;
    std::error_code outputError;
    const auto normalizedInput = std::filesystem::weakly_canonical(input, inputError);
    const auto normalizedOutput = std::filesystem::weakly_canonical(output, outputError);
    if (!inputError && !outputError && normalizedInput == normalizedOutput) {
        throw std::invalid_argument("input and output are the same; use --in-place explicitly");
    }
    if (std::filesystem::exists(output) && !overwriteOutput) {
        throw std::runtime_error("output map already exists (use --force): " + output.string());
    }

    const auto staged = uniqueSiblingPath(output, ".w3shadow.tmp");
    try {
        createValidatedCopy(input, staged, shadowBytes);
        if (std::filesystem::exists(output)) std::filesystem::remove(output);
        std::filesystem::rename(staged, output);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(staged, ignored);
        throw;
    }
}

std::filesystem::path MapArchive::replaceShadowInPlace(
    const std::filesystem::path& input,
    const std::span<const std::byte> shadowBytes)
{
    const auto staged = uniqueSiblingPath(input, ".w3shadow.tmp");
    const auto backup = uniqueSiblingPath(input, ".w3shadow.bak");
    try {
        createValidatedCopy(input, staged, shadowBytes);
        std::filesystem::rename(input, backup);
        try {
            std::filesystem::rename(staged, input);
        } catch (...) {
            std::filesystem::rename(backup, input);
            throw;
        }
        return backup;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(staged, ignored);
        throw;
    }
}

} // namespace w3shadow

