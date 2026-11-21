#pragma once

#include "objects/ShadowExclusions.hpp"
#include <windows.h>

namespace w3shadow::gui {
// Returns true only after Apply has successfully persisted the pending selection.
bool editShadowExclusions(HWND owner, const std::filesystem::path& map,
    const std::vector<ShadowObjectRow>& rows, ShadowExclusions& exclusions);
void smokeTestExclusionsDialog(HWND owner);
} // namespace w3shadow::gui
