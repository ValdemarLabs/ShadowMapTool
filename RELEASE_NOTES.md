# ShadowMapTool v1.3.4

This release fixes inherited Object Editor shadow settings and adds an optional per-map exclusion list for troublesome doodads and destructibles.

## Highlights

- Fixed objects casting shadows when their inherited **Has shadow** setting is False. Explicit False overrides already worked; stock numeric defaults are now handled correctly too. Thanks to **HerrDave** for finding and reporting the bug.
- Added **Exclusions**, a searchable list of placed object types with names, IDs, model paths, placement counts, and resolved shadow settings.
- Nothing is excluded by default. Checking a type excludes all its placements; unchecking it restores normal Object Editor behavior and never forces False objects to cast shadows.
- Exclusions are saved beside each map, shared by the GUI and CLI, and preserved with generated map copies. Global tuning presets do not change them.
- Added clearer filtering diagnostics and a reproducible guide for investigating custom-model shadows. Malformed object overrides now stop calculation instead of continuing with incomplete settings.

## Using exclusions

Open a map, select **Exclusions**, check the types to suppress, and select **Apply**. Calculate shadows again before saving. **Cancel** discards pending changes; **Clear exclusions** returns to Object Editor settings.

Keep the `<map filename>.w3shadow-exclusions` file with its map when moving or renaming it. No map object data is changed by the exclusion list.

The Windows download includes the GUI, CLI, CascLib runtime, documentation, and [diagnostic guide](https://github.com/ValdemarLabs/ShadowMapTool/blob/v1.3.4/docs/doodad-shadow-diagnostics.md). See the [changelog](https://github.com/ValdemarLabs/ShadowMapTool/blob/v1.3.4/CHANGELOG.md) for full details.

Build, regression, GUI smoke, and GUI/CLI exclusion-persistence checks passed locally. The original reported map/model still requires in-game confirmation. **Save as copy** remains the recommended first run.
