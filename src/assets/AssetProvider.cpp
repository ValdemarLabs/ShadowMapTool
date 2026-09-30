#include "assets/AssetProvider.hpp"

#include "util/FileIO.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

namespace w3shadow {

std::string normalizeAssetPath(const std::string_view path)
{
    std::string result(path);
    std::replace(result.begin(), result.end(), '/', '\\');
    while (!result.empty() && (result.front() == '\\' || result.front() == '/')) result.erase(result.begin());
    if (result.empty() || result.find("..") != std::string::npos || result.find(':') != std::string::npos) {
        return {};
    }
    return result;
}

std::optional<std::vector<std::byte>> MapAssetProvider::load(const std::string_view virtualPath) const
{
    const auto path = normalizeAssetPath(virtualPath);
    if (path.empty() || !archive_.contains(path)) return std::nullopt;
    return archive_.read(path);
}

std::optional<std::vector<std::byte>> DirectoryAssetProvider::load(
    const std::string_view virtualPath) const
{
    const auto normalized = normalizeAssetPath(virtualPath);
    if (normalized.empty()) return std::nullopt;
    std::filesystem::path relative;
    std::size_t start = 0;
    while (start < normalized.size()) {
        const auto separator = normalized.find('\\', start);
        relative /= normalized.substr(start, separator == std::string::npos ? separator : separator - start);
        if (separator == std::string::npos) break;
        start = separator + 1U;
    }
    const auto candidate = root_ / relative;
    if (!std::filesystem::is_regular_file(candidate)) return std::nullopt;
    return readBinaryFile(candidate);
}

#ifdef _WIN32
namespace {
using CascOpenStorageFunction = bool (WINAPI*)(const wchar_t*, DWORD, HANDLE*);
using CascCloseStorageFunction = bool (WINAPI*)(HANDLE);
using CascOpenFileFunction = bool (WINAPI*)(HANDLE, const void*, DWORD, DWORD, HANDLE*);
using CascGetFileSize64Function = bool (WINAPI*)(HANDLE, unsigned long long*);
using CascReadFileFunction = bool (WINAPI*)(HANDLE, void*, DWORD, DWORD*);
using CascCloseFileFunction = bool (WINAPI*)(HANDLE);

template <typename Function>
Function cascFunction(const HMODULE module, const char* name)
{
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}
} // namespace
#endif

CascAssetProvider::CascAssetProvider(
    const std::filesystem::path& warcraftDirectory,
    const std::optional<std::filesystem::path> libraryPath)
{
#ifdef _WIN32
    const auto module = LoadLibraryW((libraryPath ? *libraryPath : std::filesystem::path(L"CascLib.dll")).c_str());
    if (module == nullptr) {
        error_ = "CascLib.dll was not found; place the Unicode CascLib DLL beside the executable or use --asset-dir";
        return;
    }
    const auto openStorage = cascFunction<CascOpenStorageFunction>(module, "CascOpenStorage");
    if (openStorage == nullptr) {
        error_ = "CascLib.dll does not export CascOpenStorage";
        FreeLibrary(module);
        return;
    }
    HANDLE storage = nullptr;
    if (!openStorage(warcraftDirectory.c_str(), 0xFFFFFFFFU, &storage)) {
        error_ = "CascLib could not open the Warcraft III storage at " + warcraftDirectory.string();
        FreeLibrary(module);
        return;
    }
    module_ = module;
    storage_ = storage;
#else
    static_cast<void>(warcraftDirectory);
    static_cast<void>(libraryPath);
    error_ = "CASC loading is currently available only on Windows";
#endif
}

CascAssetProvider::~CascAssetProvider()
{
#ifdef _WIN32
    const auto module = static_cast<HMODULE>(module_);
    if (module != nullptr && storage_ != nullptr) {
        if (const auto closeStorage = cascFunction<CascCloseStorageFunction>(module, "CascCloseStorage")) {
            closeStorage(static_cast<HANDLE>(storage_));
        }
    }
    if (module != nullptr) FreeLibrary(module);
#endif
}

std::optional<std::vector<std::byte>> CascAssetProvider::load(
    const std::string_view virtualPath) const
{
#ifdef _WIN32
    if (module_ == nullptr || storage_ == nullptr) return std::nullopt;
    const auto path = normalizeAssetPath(virtualPath);
    if (path.empty()) return std::nullopt;
    const auto module = static_cast<HMODULE>(module_);
    const auto openFile = cascFunction<CascOpenFileFunction>(module, "CascOpenFile");
    const auto getSize = cascFunction<CascGetFileSize64Function>(module, "CascGetFileSize64");
    const auto readFile = cascFunction<CascReadFileFunction>(module, "CascReadFile");
    const auto closeFile = cascFunction<CascCloseFileFunction>(module, "CascCloseFile");
    if (!openFile || !getSize || !readFile || !closeFile) return std::nullopt;
    const std::array<std::string, 3> candidates{
        path, "war3.w3mod:" + path, "war3.w3mod\\" + path};
    for (const auto& candidate : candidates) {
        HANDLE file = nullptr;
        if (!openFile(static_cast<HANDLE>(storage_), candidate.c_str(), 0, 0, &file)) continue;
        unsigned long long size = 0;
        if (!getSize(file, &size) || size > 1024ULL * 1024ULL * 1024ULL ||
            size > (std::numeric_limits<std::size_t>::max)()) {
            closeFile(file);
            continue;
        }
        std::vector<std::byte> result(static_cast<std::size_t>(size));
        std::size_t offset = 0;
        bool success = true;
        while (offset < result.size()) {
            const auto count = static_cast<DWORD>(std::min<std::size_t>(result.size() - offset, 16U * 1024U * 1024U));
            DWORD received = 0;
            if (!readFile(file, result.data() + offset, count, &received) || received != count) {
                success = false;
                break;
            }
            offset += received;
        }
        closeFile(file);
        if (success) return result;
    }
#else
    static_cast<void>(virtualPath);
#endif
    return std::nullopt;
}

void CompositeAssetProvider::add(std::shared_ptr<const AssetProvider> provider)
{
    if (provider) providers_.push_back(std::move(provider));
}

std::optional<std::vector<std::byte>> CompositeAssetProvider::load(
    const std::string_view virtualPath) const
{
    for (const auto& provider : providers_) {
        if (auto result = provider->load(virtualPath)) return result;
    }
    return std::nullopt;
}

} // namespace w3shadow
