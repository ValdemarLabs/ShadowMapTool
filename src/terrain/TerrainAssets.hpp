#pragma once

#include "assets/AssetProvider.hpp"
#include "formats/W3E.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace w3shadow {

struct TerrainReceiverMask {
    std::vector<bool> transparentTilesets;
    std::size_t detectedTilesets = 0;
    std::vector<std::string> warnings;

    [[nodiscard]] bool transparentAt(const W3EMap& terrain,
                                     float worldX, float worldY) const;
};

[[nodiscard]] TerrainReceiverMask detectTransparentTerrain(
    const W3EMap& terrain, const AssetProvider& assets);

} // namespace w3shadow
