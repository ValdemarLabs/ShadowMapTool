#pragma once

#include "assets/AssetProvider.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace w3shadow {

struct ObjectDefinition {
    std::string rawcode;
    std::string modelPath;
    std::uint32_t variationCount = 1;
    std::string shadowTexture;
    bool castsShadow = true;
    bool destructible = false;
};

class ObjectDatabase {
public:
    void loadStock(const AssetProvider& assets);
    void applyMapOverrides(
        std::optional<std::vector<std::byte>> doodadData,
        std::optional<std::vector<std::byte>> destructibleData);

    [[nodiscard]] const ObjectDefinition* find(std::string_view rawcode) const;
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }
    [[nodiscard]] std::size_t size() const noexcept { return definitions_.size(); }

private:
    void loadSlk(std::span<const std::byte> bytes, bool destructible);
    void loadProfile(std::span<const std::byte> bytes, bool destructible);
    void applyObjectFile(std::span<const std::byte> bytes, bool destructible);

    std::unordered_map<std::string, ObjectDefinition> definitions_;
    std::vector<std::string> warnings_;
};

[[nodiscard]] std::vector<std::string> modelPathCandidates(
    const ObjectDefinition& definition, std::uint32_t variation);

} // namespace w3shadow
