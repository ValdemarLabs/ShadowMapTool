#pragma once

#include "archive/MapArchive.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace w3shadow {

class AssetProvider {
public:
    virtual ~AssetProvider() = default;
    [[nodiscard]] virtual std::optional<std::vector<std::byte>> load(
        std::string_view virtualPath) const = 0;
};

class MapAssetProvider final : public AssetProvider {
public:
    explicit MapAssetProvider(const MapArchive& archive) : archive_(archive) {}
    [[nodiscard]] std::optional<std::vector<std::byte>> load(
        std::string_view virtualPath) const override;
private:
    const MapArchive& archive_;
};

class DirectoryAssetProvider final : public AssetProvider {
public:
    explicit DirectoryAssetProvider(std::filesystem::path root) : root_(std::move(root)) {}
    [[nodiscard]] std::optional<std::vector<std::byte>> load(
        std::string_view virtualPath) const override;
private:
    std::filesystem::path root_;
};

class CascAssetProvider final : public AssetProvider {
public:
    explicit CascAssetProvider(
        const std::filesystem::path& warcraftDirectory,
        std::optional<std::filesystem::path> libraryPath = std::nullopt);
    ~CascAssetProvider() override;
    CascAssetProvider(const CascAssetProvider&) = delete;
    CascAssetProvider& operator=(const CascAssetProvider&) = delete;

    [[nodiscard]] std::optional<std::vector<std::byte>> load(
        std::string_view virtualPath) const override;
    [[nodiscard]] bool available() const noexcept { return storage_ != nullptr; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    void* module_ = nullptr;
    void* storage_ = nullptr;
    std::string error_;
};

class CompositeAssetProvider final : public AssetProvider {
public:
    void add(std::shared_ptr<const AssetProvider> provider);
    [[nodiscard]] std::optional<std::vector<std::byte>> load(
        std::string_view virtualPath) const override;
private:
    std::vector<std::shared_ptr<const AssetProvider>> providers_;
};

[[nodiscard]] std::string normalizeAssetPath(std::string_view path);

} // namespace w3shadow
