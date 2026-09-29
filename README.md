# ShadowMapTool

`w3shadow` is the first validation-stage implementation of the modern Warcraft III ShadowMap Calculator described in [`wc3_shadowmap_tool_analysis.md`](../_developer/Shadowmap/wc3_shadowmap_tool_analysis.md).

This milestone intentionally covers only the lowest-risk format layer:

- deterministic artificial `war3map.shd` patterns;
- grayscale PNG previews without a runtime image dependency;
- bounds-checked `war3map.w3e` dimension parsing;
- MPQ inspection and extraction through StormLib;
- validated `war3map.shd` replacement in a copied map;
- explicit, backed-up in-place replacement.
- a native high-DPI Windows GUI with interactive pattern preview.

Terrain geometry, MDX, CASC, object data, ray casting, and a GUI remain out of scope until the generated patterns are verified in the current Warcraft III client.

## Build

The build pins the official StormLib repository at the immutable `v9.40` release commit and builds its bundled compression dependencies statically. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).
Modern CMake/toolchain combinations compile in C++23 mode. The CMake 3.20 bundled with Visual Studio 2019 uses a C++20 compatibility dialect because that CMake release cannot model `CXX23`; this foundation currently uses no post-C++20 language features.

```powershell
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The build produces two applications:

- `build/Release/w3shadow-gui.exe` — modern desktop interface;
- `build/Release/w3shadow.exe` — scriptable command-line interface.

To use an already installed StormLib CMake package:

```powershell
cmake -S . -B build -DSHADOWMAPTOOL_FETCH_STORMLIB=OFF
```

## Desktop interface

Launch the GUI from PowerShell or Explorer:

```powershell
.\build\Release\w3shadow-gui.exe
```

You can also pass a map directly or drop a `.w3x`/`.w3m` file onto the window:

```powershell
.\build\Release\w3shadow-gui.exe C:\Maps\TestMap.w3x
```

The default workflow is:

1. Browse to or drop a map.
2. Select one of the six validation patterns.
3. Confirm the live SHD preview and dimensions.
4. Keep **Save as copy** selected.
5. Choose **Build pattern test** and select an output path.

The GUI also exports standalone SHD and PNG files. In-place replacement is a separate mode, requires confirmation, and retains a numbered `.w3shadow.bak` copy. Keyboard users can move between controls with `Tab`, activate them with `Enter` or `Space`, open a map with `Ctrl+O`, and build with `Ctrl+S`.

Open the in-app Help section with **? Help** or `F1`; close it with `Esc`.

### Full shadow generation status

Version 0.2 cannot yet calculate a complete gameplay shadowmap from terrain and placed objects. It does not currently reconstruct cliffs, resolve doodad/destructible models, load MDX/CASC geometry, or ray-trace the scene. **Build pattern test** inserts the selected artificial pattern for SHD compatibility testing; it is not a full calculated shadowmap.

The pattern tests must first establish current-client byte semantics, X/Y orientation, row order, and border behavior. Full calculation is the next phase after those results are confirmed in Warcraft III.

## Command-line interface

Generate an orientation test and preview:

```powershell
w3shadow pattern --map-width 64 --map-height 64 --pattern quadrants --output test.shd --png test.png
```

Inspect a map, list known archive files, and report W3E/SHD dimensions:

```powershell
w3shadow inspect MyMap.w3x
w3shadow inspect MyMap.w3x --export-shadow current.shd --png current.png
```

Inspect a standalone SHD:

```powershell
w3shadow inspect-shadow test.shd --map-width 64 --map-height 64 --png test.png
```

Insert a generated SHD into a new map copy:

```powershell
w3shadow replace-shd MyMap.w3x test.shd --output MyMap_shadowtest.w3x
```

Without `--output`, the safe default is `MyMap.shadowed.w3x`.

In-place replacement is never implicit. When requested, the original becomes a numbered `*.w3shadow.bak` file:

```powershell
w3shadow replace-shd MyMap.w3x test.shd --in-place
```

Use `--force` only to replace an existing non-input output file.

## Pattern semantics

The current unverified assumption is `0x00 = lit` and `0xFF = shadowed`. PNG output maps lit to white and shadowed to black.

- `white`: all `0x00`
- `black`: all `0xFF`
- `checker`: alternating terrain-tile-sized blocks
- `x-gradient`: `0x00` to `0xFF` as stored X increases
- `y-gradient`: `0x00` to `0xFF` as stored Y increases
- `quadrants`: white, black, vertical stripes, and checker quadrants

The following analysis questions remain empirical TODOs: `TODO-SHD-001` byte meaning, `TODO-SHD-002` orientation, and `TODO-SHD-003` padding/borders.

