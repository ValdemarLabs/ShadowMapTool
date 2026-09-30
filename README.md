# ShadowMapTool

ShadowMapTool 1.1 is a native Warcraft III static-shadow generator for `.w3x` and `.w3m` maps. It implements the pipeline described in [`wc3_shadowmap_tool_analysis.md`](../_developer/Shadowmap/wc3_shadowmap_tool_analysis.md): map parsing, terrain and placed-object geometry reconstruction, accelerated ray casting, SHD preview/export, and guarded map output. Version 1.1 adds a smoother terrain reconstruction while retaining the original version-1 triangulation as a selectable compatibility mode.

The implementation includes:

- bounds-checked W3E v11/v12 terrain, DOO v7/v8/v13 placement, W3D/W3B v1-v3 custom-object and skin data, W3R v5/v7 region, and MDX geometry readers;
- stock object-data resolution from SLK data plus current Reforged doodad/destructible skin profiles;
- map-imported MDX priority, extracted-directory fallback, and runtime Warcraft CASC access;
- transformed doodad/destructible geometry, a median-split BVH, model caching, and parallel ray casting;
- case-insensitive `IgnoreShadow...` region exclusion;
- Object Editor `dshd`/`bshd` shadow filtering, including **Has shadow: False**;
- bottom-to-top Warcraft SHD serialization with normal top-down GUI and PNG previews;
- safe map copies by default and explicit backed-up in-place replacement;
- a high-DPI Windows GUI and a scriptable CLI;
- deterministic pattern tools retained for format diagnostics.

## Build

From the `ShadowMapTool` directory:

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The build pins StormLib v9.40 and normally obtains it through CMake. To use an installed StormLib package instead:

```powershell
cmake -S . -B build -DSHADOWMAPTOOL_FETCH_STORMLIB=OFF
```

Outputs:

- `build/Release/w3shadow-gui.exe` — Windows desktop application;
- `build/Release/w3shadow.exe` — command-line application.

The Visual Studio 2019 CMake distribution uses the C++20 compatibility mode; newer CMake/toolchains select C++23. The implementation currently needs no post-C++20 language feature.

## Warcraft assets

Imported map assets are read directly from the map and take priority. The standard Windows build fetches the pinned Unicode CascLib 3.0 dependency and copies `CascLib.dll` beside both executables. Stock doodad/destructible data and models can then be read from an installed Warcraft III directory.

On first GUI start, the tool attempts to find the Warcraft III installation and the sibling `CascLib.dll`. If either is not found, the **Assets** panel opens. Both locations can be changed with **Browse** or retried with **Auto-detect**. Valid choices are stored for the current Windows user under `HKCU\Software\ShadowMapTool`; they are not tied to a map or the executable folder.

Alternative asset configurations are:

1. On the CLI, provide a nonstandard Warcraft installation or DLL explicitly with `--war3-dir` and `--casc-lib`.
2. Export the Warcraft virtual asset tree to a directory and use `--asset-dir`.

If a map has live placed objects and no object model can be resolved, the tool stops before writing output instead of silently creating a terrain-only map. Individually unresolved object types are summarized as warnings.

## Desktop workflow

Launch from Explorer or PowerShell:

```powershell
.\build\Release\w3shadow-gui.exe
.\build\Release\w3shadow-gui.exe C:\Maps\MyMap.w3x
```

To calculate the complete shadowmap:

1. Browse to a map or drop it onto the window.
2. Open **Assets** if the Warcraft III installation or `CascLib.dll` location needs changing.
3. Choose whether Terrain, Doodads, and Destructibles contribute shadows.
4. Choose **Smooth sub-tile** (default) or **Classic triangles** for terrain geometry.
5. Keep the default light vector `(1, 1, -1)`, or enter custom X/Y/Z values.
6. Select **Calculate shadows**. This renders the complete proposed SHD in memory and does not modify or create a map.
7. Inspect the calculated full-map preview and warning count. Change settings and calculate again if needed.
8. Keep **Save as copy** selected for the first run, then select **Save to map** and choose the output map.
9. Open the copy directly in Warcraft III for validation before saving it in World Editor.

**Smooth sub-tile** bilinearly reconstructs each terrain tile on a 2 × 2 sub-grid. This reduces visible diagonal facet/ridge artifacts while keeping the Warcraft heightfield and costs four times as many terrain triangles. **Classic triangles** uses the original two triangles per tile and is provided for version-1 result compatibility and lower geometry cost. Neither option reconstructs Warcraft cliff-art model faces.

The **In place + backup** mode asks for confirmation and preserves a numbered `.w3shadow.bak` copy. Opening a map previews its existing SHD, which can be empty; **Calculate shadows** replaces that view with the newly rendered complete SHD before anything is saved. Changing a calculation option marks the result stale and disables saving until it is recalculated. Diagnostic patterns are only available while **Test mode** is on. **Export SHD** and **Export PNG** export whichever full-map preview is currently shown.

Regions whose names start with `IgnoreShadow` clear their contents when **IgnoreShadow rects** is enabled. To suppress an unwanted object shadow, set that doodad/destructible's **Has shadow** field to **False** in Object Editor before calculating.

The historical calculator skipped alpha terrain tiles. Automatic alpha-BLP detection is not implemented yet. Until it is, temporarily replace alpha tiles before calculating and restore them afterward, or cover them with an `IgnoreShadow` region.

Open in-app instructions with **? Help** or `F1`. Keyboard users can navigate with `Tab`, activate controls with `Enter` or `Space`, open a map with `Ctrl+O`, calculate with `Ctrl+S`, and close Help with `Esc`.

## Command-line workflow

The safe default writes `<name>.shadowed.w3x` or `<name>.shadowed.w3m`:

```powershell
w3shadow generate MyMap.w3x
```

Generate a map copy plus diagnostics:

```powershell
w3shadow generate MyMap.w3x `
    --output MyMap.shadowed.w3x `
    --png shadow.png `
    --dump-shadow war3map.shd `
    --dump-scene scene.obj
```

Select asset sources and performance settings:

```powershell
w3shadow generate MyMap.w3x `
    --war3-dir "C:\Program Files (x86)\Warcraft III" `
    --casc-lib C:\Tools\CascLib.dll `
    --threads 8
```

Useful generation options:

- `--asset-dir DIR` supplies an extracted Warcraft-style virtual asset tree;
- `--light-x N --light-y N --light-z N` changes the default `(1, 1, -1)` light direction;
- `--smooth-terrain` selects the improved 2 × 2 sub-tile terrain reconstruction (default);
- `--classic-terrain` selects the original version-1 two-triangles-per-tile reconstruction;
- `--no-terrain`, `--no-doodads`, and `--no-destructibles` isolate geometry categories;
- `--no-honor-ignore-shadow` disables `IgnoreShadow...` region clearing;
- `--in-place` modifies the input only after creating a backup;
- `--force` permits overwriting an existing copy/diagnostic file, never the input map.

The output summary reports placements, resolved and unresolved models, unique models, triangle and ray counts, timings, and warnings.

## Inspection and SHD diagnostics

```powershell
w3shadow inspect MyMap.w3x
w3shadow inspect MyMap.w3x --export-shadow current.shd --png current.png
w3shadow inspect-shadow current.shd --map-width 64 --map-height 64 --png current.png
```

Generate a deterministic orientation pattern and insert it into a copy:

```powershell
w3shadow pattern --map-width 64 --map-height 64 --pattern quadrants --output test.shd --png test.png
w3shadow replace-shd MyMap.w3x test.shd --output MyMap.shadowtest.w3x
```

Available patterns are `black`, `white`, `checker`, `x-gradient`, `y-gradient`, and `quadrants`.

## Confirmed SHD contract

In-game tests on 29 September 2026 confirmed:

- `0x00` is lit and `0xFF` is fully shadowed;
- X is stored left-to-right;
- SHD rows are stored bottom-to-top;
- the file contains exactly `tileWidth * tileHeight * 16` bytes;
- tested map borders align without a header, offset, or padding.

The tool stores working previews top-to-bottom and reverses rows only at the Warcraft SHD boundary. Pattern maps created by version 0.2 before this correction are vertically inverted and should be rebuilt.

## Current limitations

The generator uses the MDX bind/default pose and treats parsed geoset triangles as opaque. Animated visibility, transparent-material filtering, automatic alpha-tile exclusion, exact cliff-model faces, and World Editor shadow dilation remain compatibility work that requires isolated in-game reference maps. Terrain cliff-layer elevations are included. Smooth sub-tile mode improves the heightfield surface but does not reconstruct cliff art models.

The format, archive, parser, BVH, GUI-smoke, production 50,118-placement DOO fixture, current-map DOO/W3B/W3D/W3R, Object Editor shadow override, and terrain-only end-to-end paths are automated. The installed-Warcraft integration test validates stock SLK/profile and MDX resolution when a Warcraft III installation is available.
