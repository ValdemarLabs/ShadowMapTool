#pragma once

#include "archive/MapArchive.hpp"
#include "formats/DOO.hpp"
#include "objects/ObjectDatabase.hpp"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace w3shadow {

using ShadowExclusions = std::set<std::string>;

struct ShadowObjectRow {
    std::string rawcode;
    std::string name;
    std::string modelPath;
    std::size_t placements = 0;
    bool anyEnabled = false;
    bool anyDisabled = false;
    bool unresolved = false;
};

// One exclusion covers every placed instance/skin of this object type, never a model path.
[[nodiscard]] ObjectDatabase loadMapObjectDatabase(const MapArchive& archive, const AssetProvider& assets);
[[nodiscard]] const ObjectDefinition* placementDefinition(
    const ObjectDatabase& objects, const DoodadPlacement& placement);
[[nodiscard]] std::vector<ShadowObjectRow> shadowObjectRows(
    const MapArchive& archive, const ObjectDatabase& objects, DOOLayoutMode layout);
[[nodiscard]] std::filesystem::path shadowExclusionsPath(const std::filesystem::path& map);
[[nodiscard]] ShadowExclusions loadShadowExclusions(const std::filesystem::path& map);
void saveShadowExclusions(const std::filesystem::path& map, const ShadowExclusions& exclusions);

} // namespace w3shadow
