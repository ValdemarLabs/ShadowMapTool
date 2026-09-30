# ShadowMapTool Changelog

This changelog records notable ShadowMapTool releases and development changes.

## [1.3.0] - 1.10.2026

ShadowMapTool 1.3.0 is the first standalone public release.

### Release and repository

- Published ShadowMapTool in its dedicated public repository with its tool-specific commit history preserved.
- Established `main` as the stable branch and `dev` as the integration branch.
- Added Windows build validation for `main`, `dev`, and pull requests, plus tagged-release automation.
- Added a curated release packager that includes the GUI, CLI, `CascLib.dll`, README, changelog, and third-party notices while excluding build libraries, tests, and generated logs.
- Included the technical analysis, application branding, and World Editor comparison maps in the standalone repository.
- Added application screenshots and this dedicated changelog, which is also included in curated release packages.

### Added

- Added direct classic Warcraft III MPQ access using patch, local, expansion, and base archive priority.
- Added timestamped UTF-8 session logs for lifecycle events, selected asset backends, loaded maps, successes, warnings, and failures.
- Added a **Logs** button and an expandable **About** panel covering purpose, origin, compatibility, limitations, and credits.

### Compatibility

- Added compact DOO v7 placement parsing and validated classic assets, object databases, and MDX models against Warcraft III 1.27b.
- Retained current Warcraft III CASC support through the bundled Unicode CascLib 3.0 runtime.
- Confirmed that ShadowMapTool replaces only `war3map.shd`; it does not convert a map between Warcraft III versions.

## [1.2.0] - 30.9.2026

### Added

- Added **Fast 1x**, **Smooth 2x**, and default **Ultra 4x** edge-quality modes to the GUI and CLI.
- Added strict-majority sub-cell sampling using 1, 4, or 16 rays per Warcraft shadow cell.

### Changed

- Improved binary SHD outlines without changing Warcraft III's fixed four-shadow-cells-per-terrain-tile format.
- Kept edge quality independent from the selected terrain reconstruction mode.

## [1.1.0] - 30.9.2026

### Added

- Added the default **Smooth sub-tile** terrain mode with a 2 x 2 bilinear sub-grid.
- Added selectable **Classic triangles** terrain geometry for version-1-compatible results and lower calculation cost.
- Added first-start Warcraft III and CascLib auto-detection, persistent per-user asset locations, and an **Assets** panel for manual selection.
- Embedded the supplied favicon in the executable, application window, taskbar, and in-app header.
- Added bounds-checked special DOO cliff-doodad parsing, inspect diagnostics, and automated World Editor/tool reference-map comparison.

### Changed

- Removed the obsolete **Full Generator** label and the internal SHD row-orientation caption from the preview.
- Documented that Smooth sub-tile reduces heightfield facet artifacts but does not reproduce Warcraft cliff-art geometry.

### Fixed

- Fixed Assets-panel descriptions overflowing at narrower window sizes and higher display scaling.

## [1.0.0] - 30.9.2026

### Added

- Added bounds-checked W3E v11/v12 terrain, DOO v7/v8/v13 placement, W3D/W3B v1-v3 object-data, W3R v5/v7 region, and MDX geometry parsing.
- Added imported-map, extracted-directory, and Warcraft installation asset resolution.
- Added transformed doodad and destructible geometry, model caching, a median-split BVH, and parallel ray casting.
- Added complete terrain, doodad, and destructible shadow calculation with independently selectable geometry categories.
- Added editable light-vector values with the World Editor-like default `(1, 1, -1)`.
- Added case-insensitive `IgnoreShadow...` region exclusion.
- Added Object Editor `dshd` and `bshd` filtering, including **Has shadow: False**.
- Added complete existing/calculated map previews and SHD, PNG, and OBJ diagnostic exports.
- Added safe map-copy output by default and explicit backed-up in-place replacement.
- Added Reforged skin-profile resolution, version-3 map object/skin overrides, W3R v7 regions, numbered model variations, and unresolved-object summaries.
- Added a diagnostic **Test mode** for deterministic SHD patterns.

### Changed

- Renamed the full-generation action to **Calculate shadows**.
- Split calculation from saving: calculation now produces a complete non-destructive in-memory preview, while **Save to map** writes only a current inspected result.
- Stopped generation before writing when a map contains placed objects but no placed-object model can be resolved.

### Validation

- Confirmed exact SHD file size and border behavior without a header, offset, or padding.
- Validated a 480 x 480 map with 50,049 shadow-casting placements and zero unresolved models.
- Added automated format, geometry, archive replacement, ignore-region, Object Editor override, terrain-only, and GUI smoke coverage.

## Pre-1.0 development - 29.9.2026

### Added

- Added the initial C++ command-line foundation for deterministic SHD patterns, map inspection, preview export, and guarded `war3map.shd` replacement.
- Added a high-DPI native Windows interface with drag-and-drop map loading, Unicode paths, keyboard navigation, live previews, and safe output controls.
- Added an in-app Help overlay and clearer validation-mode actions.

### Fixed

- Corrected Warcraft III SHD serialization after in-game validation established that `0x00` is lit, `0xFF` is shadowed, X runs left-to-right, and stored rows run bottom-to-top.
- Kept GUI and PNG previews top-down while reversing rows only at the Warcraft SHD boundary.
- Enabled UTF-8 source compilation so multiplication signs and status symbols render correctly.

### Compatibility note

- Diagnostic pattern maps created before the row-order correction are vertically inverted and should be rebuilt.

## Current limitations

- Model shadows use parsed MDX bind/default-pose geometry and treat parsed geoset triangles as opaque.
- Animated visibility and texture-alpha/material filtering are not reproduced yet.
- Automatic alpha-terrain-tile exclusion is not implemented.
- Exact cliff-art model faces and World Editor shadow post-processing are not reproduced.
- World Editor and ShadowMapTool results can therefore differ even when SHD orientation and byte polarity are correct.

[1.3.0]: https://github.com/ValdemarLabs/ShadowMapTool/tree/v1.3.0
