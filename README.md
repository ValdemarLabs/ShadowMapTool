# ShadowMapTool

ShadowMapTool 1.3 is a native Warcraft III static-shadow generator for `.w3x` and `.w3m` maps. It implements the pipeline described in [`wc3_shadowmap_tool_analysis.md`](docs/wc3_shadowmap_tool_analysis.md): map parsing, terrain and placed-object geometry reconstruction, accelerated ray casting, SHD preview/export, and guarded map output. Version 1.3 adds direct classic MPQ asset loading validated against Warcraft III 1.27b, timestamped session logs, and an expandable in-app About panel. Selectable 1x, 2x, and 4x edge supersampling, smoother terrain reconstruction, and original version-1 triangulation remain independently selectable.

See [`CHANGELOG.md`](CHANGELOG.md) for the version history and current compatibility limitations.

The implementation includes:

- bounds-checked W3E v11/v12 terrain, DOO v7/v8/v13 placement, W3D/W3B v1-v3 custom-object and skin data, W3R v5/v7 region, and MDX geometry readers;
- stock object-data resolution from SLK data plus current Reforged doodad/destructible skin profiles;
- map-imported MDX priority, extracted-directory fallback, runtime Warcraft CASC access, and direct classic MPQ access;
- transformed doodad/destructible geometry, a median-split BVH, model caching, and parallel ray casting;
- MDX material filtering plus UV-aware alpha tests for paletted/JPEG BLP1 and paletted/DXT/raw BLP2 model textures;
- 1x, 2x, or 4x shadow-cell supersampling with coherent coverage filtering for cleaner edges;
- automatic fully transparent terrain-receiver exclusion and optional experimental cliff-wall reconstruction;
- case-insensitive `IgnoreShadow...` region exclusion;
- Object Editor `dshd`/`bshd` shadow filtering, including **Has shadow: False**;
- bottom-to-top Warcraft SHD serialization with normal top-down GUI and PNG previews;
- safe map copies by default and explicit backed-up in-place replacement;
- a high-DPI Windows GUI with Help, Logs, and expandable About panels, plus a scriptable CLI;
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

Create the curated public Windows package after a successful build and test run:

```powershell
.\scripts\build-release.ps1
```

This writes `dist/ShadowMapTool-1.3.1-win64.zip` and `dist/SHA256SUMS.txt`. The package contains only the GUI, CLI, runtime CascLib DLL, README, changelog, and third-party notices. Do not publish the whole `build/Release` directory: it also contains the test executable, static development library, and generated session logs.

## Branches and releases

- `dev` is the integration branch for active work and pull requests.
- `main` contains stable public versions.
- tags named `v*` build, test, and publish a GitHub release automatically.

Normal changes should be committed to `dev`, validated there, and merged into `main`. A release tag such as `v1.3.1` belongs on the corresponding stable `main` commit.

The Visual Studio 2019 CMake distribution uses the C++20 compatibility mode; newer CMake/toolchains select C++23. The implementation currently needs no post-C++20 language feature.

## Warcraft assets

Imported map assets are read directly from the map and take priority. Current Warcraft III installations are read through the bundled Unicode CascLib 3.0 dependency. Classic installations are read directly from `War3Patch.mpq`, `War3xLocal.mpq`, `War3Local.mpq`, `War3x.mpq`, and `War3.mpq` in patch/local/expansion/base priority; CascLib is not required for those installations.

On first GUI start, the tool attempts to find a Warcraft III installation and, for CASC storage, the sibling `CascLib.dll`. If usable assets are not found, the **Assets** panel opens. Use **Browse** for nonstandard locations such as `F:\Pelit\Warcraft III 1.27b`, or retry standard and registry locations with **Auto-detect**. Valid choices are stored for the current Windows user under `HKCU\Software\ShadowMapTool`; they are not tied to a map or the executable folder.

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
2. Open **Assets** if the Warcraft III installation needs changing. CascLib is required only for CASC installations.
3. Choose whether Terrain, Doodads, and Destructibles contribute shadows.
4. Choose **Smooth sub-tile** (default) or **Classic triangles** for terrain geometry.
5. Choose **Ultra 4x** edge quality for the smoothest outline, **Smooth 2x** for a faster compromise, or **Fast 1x** for the original single-ray behavior.
6. Open **Tuning...** to adjust filtering, sub-cell coverage, terrain ray bias, alpha-terrain handling, or performance. The recommended defaults suit most maps.
7. Keep the default light vector `(1, 1, -1)`, or enter custom X/Y/Z values.
8. Select **Calculate shadows**. This renders the complete proposed SHD in memory and does not modify or create a map.
9. Inspect the calculated full-map preview and warning count. Change settings and calculate again if needed.
10. Keep **Save as copy** selected for the first run, then select **Save to map** and choose the output map.
11. Open the copy directly in Warcraft III for validation before saving it in World Editor.

**Smooth sub-tile** reconstructs each terrain tile on a 2 × 2 bilinear-derived triangle grid. Receiver heights now use those exact triangles, preventing rays from starting inside a slightly different mathematical surface. **Classic triangles** uses the original two triangles per tile and is provided for version-1-compatible results and lower geometry cost. Experimental contour-based cliff walls are available under **Tuning...** or from the CLI with `--cliff-walls`, but remain off by default because the approximate walls can over-darken raised terrain compared with World Editor.

Edge quality is separate from terrain geometry. **Fast 1x**, **Smooth 2x**, and **Ultra 4x** cast 1, 4, or 16 regularly spaced rays inside every fixed Warcraft SHD cell. Coherent filtering passes that coverage through a configurable Gaussian radius before the final binary cutoff, producing connected, rounded boundaries without visible dither dots. **Tuning...** exposes radius `0..3`, coverage cutoff `20..80%`, and minimum island size `0..16`; lower cutoffs retain more partial shadow while larger radii join and soften shapes more aggressively. The SHD still contains only native `0x00`/`0xFF` values and cannot store true opacity or additional resolution. Ultra 4x can take roughly sixteen times the ray-casting work of Fast 1x on a large map. Turn off **Coherent filter** (or use `--hard-edges`) for strict-majority collapse.

Terrain receivers use a slope-aware origin bias of 2 world units by default. This prevents raised or curved terrain from immediately intersecting its own caster triangles, which otherwise appears as scattered dark patches. **Tuning...** exposes `0..32` in 0.5-unit steps; very large values can detach contact shadows and are intended only for diagnosis. Worker threads can also be set from Auto to 32 and affect performance only.

Placed doodad/destructible scale is applied independently on X, Y, and Z before rotation and translation. **Maximum caster span** defaults to 16,384 world units and excludes unusually large enclosing domes, sky shells, and backdrop models before ray casting; these models otherwise create map-sized dark regions even when their visible surface is mostly outside the camera. The session log names every excluded rawcode/model and its transformed scale/span. Set the control to **Unlimited** or pass `--max-caster-span 0` when a deliberately enormous model should cast a shadow. Nearby ordinary shadows are not unioned during geometry processing, and coherent Gaussian filtering keeps disconnected raw shadow components separate so blur alone cannot bridge them.

The **In place + backup** mode asks for confirmation and preserves a numbered `.w3shadow.bak` copy. Opening a map previews its existing SHD, which can be empty; **Calculate shadows** replaces that view with the newly rendered complete SHD before anything is saved. Changing a calculation option marks the result stale and disables saving until it is recalculated. Diagnostic patterns are only available while **Test mode** is on. **Export SHD** and **Export PNG** export whichever full-map preview is currently shown.

Regions whose names start with `IgnoreShadow` clear their exact World Editor rectangle contents when **IgnoreShadow rects** is enabled. Version 1.3 parsed the W3R coordinate fields in the wrong order; development builds after 1.3 correct that displaced/transposed exclusion. To suppress an unwanted object shadow, set that doodad/destructible's **Has shadow** field to **False** in Object Editor before calculating.

Fully transparent alpha terrain tiles are automatically detected through `TerrainArt\\Terrain.slk` and the effective map-imported or installed BLP, then excluded as shadow receivers. **Ignore alpha terrain** is enabled by default in **Tuning...** and can be disabled for maps that intentionally want those cells to receive static shadow. The detector intentionally requires a completely transparent top mip so ordinary terrain textures with an alpha channel are not accidentally removed. The session log warns if terrain metadata is unavailable. Partially transparent terrain and non-BLP terrain replacements should be covered by an `IgnoreShadow` region when they must remain shadow-free.

Open in-app instructions with **? Help** or `F1`. Keyboard users can navigate with `Tab`, activate controls with `Enter` or `Space`, open a map with `Ctrl+O`, calculate with `Ctrl+S`, and close Help with `Esc`.

The **Logs** button opens the `logs` directory beside the executable. Every GUI run creates a timestamped UTF-8 log containing session lifecycle events, selected asset backend, loaded maps, successes, warnings, and failures. If the executable directory is not writable, logs fall back to `%LOCALAPPDATA%\ShadowMapTool\logs`. The **About** panel contains expandable purpose, origin, compatibility, limitation, and credit sections.

## Warcraft version compatibility

The intended target is current Warcraft III 3.0. Direct legacy asset access has also been validated against the supplied Warcraft III 1.27b installation: its stock SLK databases and classic MDX models resolve from MPQ, and a complete 64 × 64 reference-map calculation resolved all 50 placed objects with zero unresolved models. The parser covers legacy W3E v11, DOO v7/v8, W3R v5, object-data v1-v3, and MDX 800 inputs.

The tool replaces only `war3map.shd`; it does not convert the rest of a map for an older client. A map must already be compatible with the Warcraft version used to open it. For an unsupported installation layout, terrain-only mode requires no game assets, map-imported models remain usable, and the CLI can consume an extracted Warcraft asset tree through `--asset-dir`.

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
    --edge-samples 4 `
    --threads 8
```

Useful generation options:

- `--asset-dir DIR` supplies an extracted Warcraft-style virtual asset tree;
- `--light-x N --light-y N --light-z N` changes the default `(1, 1, -1)` light direction;
- `--edge-samples 1|2|4` selects Fast, Smooth, or Ultra edge supersampling (default `4`);
- `--smooth-terrain` selects the improved 2 × 2 sub-tile terrain reconstruction (default);
- `--classic-terrain` selects the original version-1 two-triangles-per-tile reconstruction;
- `--hard-edges` bypasses coherent coverage filtering and uses strict-majority sub-cell collapse;
- `--gaussian-radius 0..3` sets the coherent spatial-filter radius (default `1`; `0` disables spatial blur);
- `--coverage-threshold 0..1` sets the final filled-cell cutoff (default `0.45`);
- `--min-island-size 0..64` removes smaller connected shadow islands (default `4`; `0` disables cleanup);
- `--max-caster-span 0..131072` excludes a placed model whose transformed X/Y span exceeds the limit (default `16384`; `0` disables the limit);
- `--ray-bias N` adjusts slope-aware self-shadow protection from `0` to `32` (default `2`);
- `--cliff-walls` enables experimental discrete cliff-layer wall occluders;
- `--no-alpha-terrain-mask` disables fully transparent terrain receiver detection;
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

The generator uses the MDX bind/default pose. It excludes non-shadow visual-effect materials and alpha-tests Transparent layers when their BLP texture resolves, but animated visibility, texture animation, and every replaceable/dynamic texture behavior are not reconstructed. A missing or unsupported alpha texture is reported and safely falls back to opaque geometry. Fully transparent terrain receivers are excluded automatically. Approximate cliff-layer walls are optional and disabled by default; exact decorative cliff-model protrusions and World Editor's undocumented post-processing remain compatibility work that needs isolated in-game reference maps.

The paired 64 x 64 reference maps in `tests/fixtures/reference-maps/` establish that SHD orientation and byte polarity are correct, but also quantify the older version-2 rendering difference: World Editor writes 2,050 shadowed samples while the stored Smooth sub-tile/Fast 1x fixture writes 4,631. Their intersection-over-union is 29.4%. Classic triangles at Fast 1x was somewhat closer on that mixed scene (33.0%). Those fixtures predate material/alpha filtering and should not be treated as current quality benchmarks; further World Editor matching work still needs isolated terrain, cliff, opaque-model, alpha-tested-model, and animated-visibility reference pairs.

The format, archive, parser, BVH, GUI-smoke, production 50,118-placement DOO fixture, current-map DOO/W3B/W3D/W3R, Object Editor shadow override, and terrain-only end-to-end paths are automated. The installed-Warcraft integration test validates stock SLK/profile and MDX resolution when a Warcraft III installation is available.
