# Warcraft III Modern ShadowMap Tool — Implementation Analysis

## 1. Purpose

This document defines the technical direction for creating a modern replacement for Oger-Lord's Warcraft III ShadowMap Calculator.

The target is a standalone tool that can:

1. Open current Warcraft III `.w3x` / `.w3m` maps.
2. Read terrain and placed world geometry.
3. Load Warcraft III game assets and imported custom assets.
4. Calculate static map shadows.
5. Generate a valid `war3map.shd`.
6. Replace or insert that file into the map archive.
7. Work with current Warcraft III / Warcraft III: Reforged installations.
8. Be substantially faster and more robust than the World Editor shadow calculation.

The preferred implementation should avoid process injection, memory modification, editor hooks, or dependence on undocumented live editor APIs.

The design should be modular enough that Warcraft III asset loading can evolve independently from the shadow-generation algorithm.

## Implementation status — 30 September 2026

`ShadowMapTool/` now implements the version-1 MVP pipeline: W3E v11/v12 terrain, DOO placements, stock/custom object-data resolution, map/directory/runtime-CASC assets, MDX bind-pose geometry, world transforms, BVH-accelerated parallel ray casting, W3R `IgnoreShadow...` clearing, SHD/PNG/OBJ diagnostics, guarded archive output, and a native Windows GUI.

The confirmed SHD writer contract is closed. Remaining TODOs in section 57 concern exact World Editor/historical-calculator compatibility rules, not the ability to construct and write a complete static shadowmap. Current deliberate limits are recorded in `ShadowMapTool/README.md`.

---

# 2. Core Technical Finding

The old ShadowMap Calculator does not fundamentally depend on a special Warcraft III rendering API.

Its important job is approximately:

```text
Warcraft III map
    ↓
read terrain
read doodads/destructibles
read model geometry
    ↓
construct world geometry
    ↓
project / ray-test geometry against light direction
    ↓
generate binary shadow grid
    ↓
write war3map.shd
```

This means the correct modernization strategy is:

> Reimplement the shadow-generation pipeline around current Warcraft III file and asset formats rather than patching the old executable.

The principal incompatibilities in the old utility were related to historical Warcraft III installation assumptions such as:

- legacy registry installation paths
- MPQ-based game data
- `war3Patch.mpq`
- older MDX expectations
- pre-Reforged asset layout

The shadow-map concept itself remains suitable for a modern implementation.

---

# 3. Scope

## 3.1 Initial scope

The first useful release should support:

- `.w3x`
- `.w3m`
- terrain
- cliffs
- doodads
- destructibles
- custom imported models
- Classic/SD assets
- Reforged/HD assets where required
- `war3map.shd` generation
- reinsertion into the map
- configurable shadow/light direction
- multithreaded calculation
- command-line operation

GUI should be treated as a second-stage deliverable.

---

## 3.2 Desired later features

Possible later additions:

- graphical preview
- shadow exclusion regions
- alpha-tile exclusion
- per-object shadow inclusion/exclusion
- support for object editor fields affecting shadow behavior
- comparison against current World Editor output
- configurable shadow softness/dilation
- caching of parsed models
- incremental rebuild
- project settings file
- batch processing
- before/after preview
- map backup and restore
- diagnostics mode

---

# 4. Recommended Implementation Language

## Preferred

### C++23

Reasons:

- good interoperability with existing Warcraft III native libraries
- strong performance for geometry processing
- efficient memory control
- suitable for BVH acceleration
- easy use of SIMD
- good support for multithreading
- straightforward Windows standalone executable deployment
- Qt 6 available if GUI is added later

Potential stack:

```text
Language:        C++23
Build system:    CMake
GUI:             Qt 6, optional
Math:            GLM or equivalent
Parallelism:     std::jthread / std::execution / TBB
Ray intersection:
    - custom BVH
    - or Embree if dependency size is acceptable
Map archive:
    - StormLib or equivalent MPQ library
CASC:
    - CascLib or another maintained Warcraft III-compatible reader
Testing:
    - Catch2 / GoogleTest
Logging:
    - spdlog or lightweight internal logger
```

---

## Alternative

### Rust

Rust is viable and attractive for:

- parsing reliability
- memory safety
- deterministic error handling
- standalone CLI utilities

However, integration with existing Warcraft III C/C++ tooling may require more adaptation.

---

# 5. High-Level Architecture

```text
                          ┌────────────────────┐
                          │ Warcraft III Map   │
                          │ .w3x / .w3m        │
                          └─────────┬──────────┘
                                    │
           ┌────────────────────────┼────────────────────────┐
           │                        │                        │
           ▼                        ▼                        ▼
   ┌──────────────┐         ┌──────────────┐        ┌──────────────┐
   │ war3map.w3e  │         │ war3map.doo  │        │ object data  │
   │ terrain      │         │ placements   │        │ w3d/w3b/etc. │
   └──────┬───────┘         └──────┬───────┘        └──────┬───────┘
          │                        │                        │
          └────────────┬───────────┴─────────────┬─────────┘
                       │                         │
                       ▼                         ▼
              ┌────────────────┐       ┌────────────────────┐
              │ Terrain Builder│       │ Asset Resolver     │
              └───────┬────────┘       └─────────┬──────────┘
                      │                          │
                      │                ┌─────────┴─────────┐
                      │                │                   │
                      │                ▼                   ▼
                      │          Map imports          WC3 CASC
                      │                │                   │
                      │                └─────────┬─────────┘
                      │                          ▼
                      │                 ┌────────────────┐
                      │                 │ MDX/MDL Parser │
                      │                 └───────┬────────┘
                      │                         │
                      └──────────────┬──────────┘
                                     ▼
                           ┌────────────────────┐
                           │ World Geometry     │
                           │ / Shadow Casters   │
                           └─────────┬──────────┘
                                     ▼
                           ┌────────────────────┐
                           │ Spatial Index      │
                           │ BVH / AABB tree    │
                           └─────────┬──────────┘
                                     ▼
                           ┌────────────────────┐
                           │ Shadow Calculator  │
                           │ parallel ray tests │
                           └─────────┬──────────┘
                                     ▼
                           ┌────────────────────┐
                           │ war3map.shd Writer │
                           └─────────┬──────────┘
                                     ▼
                           ┌────────────────────┐
                           │ Map Archive Writer │
                           └────────────────────┘
```

---

# 6. Core Modules

## 6.1 MapArchive

Responsibilities:

- open `.w3x` / `.w3m`
- enumerate contained files
- extract files
- insert/replace files
- preserve unrelated map data
- optionally create backup

Suggested interface:

```cpp
class IMapArchive {
public:
    virtual ~IMapArchive() = default;

    virtual bool contains(std::string_view path) const = 0;
    virtual std::vector<std::byte> read(std::string_view path) const = 0;
    virtual void write(
        std::string_view path,
        std::span<const std::byte> data
    ) = 0;
    virtual void save(const std::filesystem::path& destination) = 0;
};
```

Requirements:

- never silently corrupt original map
- atomic or temporary-file save strategy
- optional `--in-place`
- default should produce a new output map during early development

---

# 7. Asset Provider Abstraction

Game data loading must not be tightly coupled to the shadow calculation.

Recommended interface:

```cpp
class IAssetProvider {
public:
    virtual ~IAssetProvider() = default;

    virtual std::optional<std::vector<std::byte>>
    load(std::string_view virtualPath) = 0;
};
```

Implementations:

```text
MapAssetProvider
    Loads imported map assets.

CascAssetProvider
    Loads files from current Warcraft III installation.

DirectoryAssetProvider
    Useful for tests, extracted assets, and development.

CompositeAssetProvider
    Searches providers in priority order.
```

Recommended priority:

```text
1. Map-imported asset
2. Explicit override directory
3. Warcraft III CASC
```

This allows custom imported models to override stock game data correctly.

---

# 8. Graphics Mode Abstraction

Do not hard-code one renderer asset set.

Suggested enum:

```cpp
enum class GraphicsMode {
    Classic,
    Reforged,
    Definitive,
    Auto
};
```

`Auto` should attempt to infer the asset set required by the map/current game configuration.

The shadow system should consume resolved geometry only.

It should not care where the model came from.

---

# 9. Terrain Parsing

The terrain source is primarily:

```text
war3map.w3e
```

Required information:

- map width
- map height
- terrain vertex grid
- ground height
- cliff height
- tile type
- tile flags where relevant
- world-space origin
- water information if it affects shadow calculation
- boundary dimensions

The implementation must reconstruct accurate world-space terrain geometry.

Important test:

```text
single flat map
```

Expected result:

- terrain sample positions exactly match Warcraft III tile coordinates
- generated `.shd` dimensions match map dimensions

---

# 10. Shadow Map Resolution

The known Warcraft III shadow grid should be treated as:

```text
4 x 4 samples per terrain tile
```

For a map of:

```text
width  = W
height = H
```

shadow-map resolution becomes approximately:

```text
shadowWidth  = W * 4
shadowHeight = H * 4
```

Expected byte count:

```text
W * H * 16
```

Example:

```text
480 × 480 map

1920 × 1920 shadow samples

3,686,400 bytes
```

In-game testing on 29 September 2026 confirmed that X is stored left-to-right and SHD rows are stored bottom-to-top. Tool previews use conventional top-to-bottom screen order, so the file writer reverses rows during serialization and the reader reverses them during preview decoding.

---

# 11. war3map.shd Writer

Create a dedicated module:

```cpp
class ShadowMap {
public:
    ShadowMap(uint32_t widthTiles, uint32_t heightTiles);

    void set(uint32_t x, uint32_t y, bool shadowed);
    bool get(uint32_t x, uint32_t y) const;

    std::span<const std::byte> bytes() const;
};
```

Confirmed current-client representation (29 September 2026):

```text
lit      = 0x00
shadowed = 0xFF
```

These values were validated in a controlled quadrant-pattern test map.

Tests must verify:

- exact file length
- row order
- X/Y orientation
- map edge handling
- all-white test
- all-black test
- checkerboard test
- vertical gradient test
- horizontal gradient test

These patterns will allow visual determination of coordinate orientation inside WC3.

---

# 12. World Geometry

Use a normalized world-space representation.

Example:

```cpp
struct Triangle {
    glm::vec3 a;
    glm::vec3 b;
    glm::vec3 c;
};

struct MeshInstance {
    std::shared_ptr<const Mesh> mesh;
    glm::mat4 transform;
};
```

Avoid immediately transforming every triangle for every instance if memory use becomes large.

Possible strategies:

### Option A

Transform all triangles to world space.

Pros:

- simplest
- fastest traversal later

Cons:

- higher memory use

### Option B

Store local mesh BVH + per-instance transform.

Pros:

- excellent for repeated doodad models
- lower memory use
- faster map loading

Cons:

- more complex ray traversal

Preferred long-term solution:

```text
two-level BVH
```

```text
Top-level BVH
    ↓
model instances
    ↓
per-model BVH
    ↓
triangles
```

This is ideal because Warcraft III maps frequently contain many repeated doodads.

---

# 13. Doodad and Destructible Placement

Primary source:

```text
war3map.doo
```

Required placement fields may include:

- raw object ID
- variation
- X
- Y
- Z
- rotation
- scale X
- scale Y
- scale Z
- state
- item table data not relevant to shadows
- editor-generated identity fields not relevant to shadows

The geometry transform must correctly reproduce Warcraft III placement.

Transformation order must be verified.

Likely conceptual form:

```text
model-space vertex
    ↓
variation model selection
    ↓
scale
    ↓
rotation
    ↓
translation
    ↓
world space
```

Pitch/roll behavior must be investigated if present in modern map data or object configuration.

---

# 14. Object Data Resolution

A doodad rawcode is not directly a model path.

The tool needs an object-data resolution layer.

Possible sources:

- stock Warcraft III SLK/object data
- custom map object definitions
- `war3map.w3d`
- `war3map.w3b`
- other object files as required
- custom object modifications

Resolve at minimum:

```text
rawcode
    ↓
model path
    ↓
variation behavior
    ↓
shadow behavior flags
```

The implementation should explicitly record unresolved objects.

Example warning:

```text
WARN Doodad LTlt at (1024, -512):
     model path could not be resolved
```

Do not silently ignore parsing failures.

---

# 15. MDX / MDL Geometry

The shadow calculator primarily needs collision-like visible geometry, not rendering fidelity.

Required from MDX:

- vertices
- triangle indices
- geosets
- node hierarchy if geometry depends on it
- static transforms
- geoset visibility rules
- model extents where useful
- geoset animations where relevant
- alpha / visibility rules if needed
- material information if transparent surfaces should not cast shadows

Initially ignore:

- particle emitters
- ribbon emitters
- sound emitters
- event objects
- texture animation
- lighting
- normals except for debugging
- UVs
- shaders

Question requiring empirical validation:

> Which geosets did Oger-Lord and/or World Editor consider valid shadow-casting geometry?

Possible rules:

- all visible geosets
- only non-transparent geosets
- only geosets visible in stand animation
- all static geometry regardless of animation
- special object-editor shadow flags

Do not guess permanently.

Add compatibility tests.

---

# 16. Animation Handling

Static doodads can still use animated models.

Possible approaches:

### Phase 1

Use bind pose / default pose.

### Phase 2

Evaluate a deterministic reference sequence, probably `Stand`.

### Phase 3

Replicate World Editor's exact shadow pose behavior.

Need test models with:

- rotating parts
- animated gates
- trees
- destructibles
- billboard geometry

Avoid implementing a full Warcraft III animation engine before determining whether it is necessary.

---

# 17. Shadow Calculation

Default light/shadow vector:

```text
(1, 1, -1)
```

Make configurable.

Normalize before use:

```cpp
glm::vec3 direction = glm::normalize(inputDirection);
```

For every shadow sample:

```text
1. Convert shadow pixel to world-space terrain location.
2. Determine terrain surface Z at that XY.
3. Offset ray origin slightly above the surface.
4. Cast ray opposite the incoming light direction.
5. If a valid occluder is intersected:
       shadow = true
   else:
       shadow = false
```

Pseudo-code:

```cpp
for each shadowSample in parallel:
{
    Vec3 origin = terrain.samplePosition(shadowSample.xy);
    origin.z += epsilon;

    Ray ray {
        .origin = origin,
        .direction = -lightDirection
    };

    bool occluded = scene.intersects(ray);

    shadowMap.set(
        shadowSample.x,
        shadowSample.y,
        occluded
    );
}
```

---

# 18. Self-Intersection

Ray origins must be offset.

Example:

```cpp
constexpr float RayOriginEpsilon = 0.5f;
```

The correct value must be scaled to Warcraft III world units.

Too small:

- terrain self-intersection
- acne artifacts

Too large:

- detached shadows
- missed short objects

This needs visual regression testing.

---

# 19. Terrain as Occluder

Determine whether terrain itself contributes projected shadows.

Test cases:

1. flat map
2. isolated hill
3. cliff
4. trench
5. stepped cliff
6. terrain with no doodads

Compare:

- World Editor
- old calculator if still executable
- new implementation

Terrain shadows may require terrain triangles to be part of the BVH.

---

# 20. Spatial Acceleration

Never test every ray against every triangle.

That would scale approximately as:

```text
O(samples × triangles)
```

For large maps this is unacceptable.

Use BVH.

Recommended structure:

```text
TLAS
    instance bounds

BLAS
    model triangles
```

Where:

```text
TLAS = top-level acceleration structure
BLAS = bottom-level acceleration structure
```

This is analogous to modern ray-tracing architecture and is particularly suitable for repeated Warcraft III doodads.

---

# 21. BVH Requirements

Start with:

- AABB nodes
- median split or SAH
- iterative traversal
- compact node storage

Later optimize using:

- SIMD ray/AABB tests
- packet traversal if beneficial
- BVH4/BVH8
- Embree

The first implementation should prioritize correctness over micro-optimization.

---

# 22. Ray-Triangle Intersection

Use a numerically stable implementation.

Possible algorithm:

```text
Möller–Trumbore
```

Requirements:

- epsilon handling
- one-sided vs two-sided behavior must be configurable
- triangle winding must not unintentionally remove shadows

Most Warcraft III geometry should probably be treated as double-sided for shadow testing unless empirical testing shows otherwise.

---

# 23. Multithreading

The shadow grid is embarrassingly parallel.

Preferred partition:

```text
rows or rectangular tiles
```

Example:

```text
Tile size:
64 × 64 shadow samples
```

Use worker scheduling rather than assigning one contiguous block per CPU if workload can vary.

Possible implementation:

```cpp
std::atomic<uint32_t> nextTile;
```

or a task scheduler.

Performance metrics should include:

```text
map dimensions
shadow samples
scene instances
unique models
triangles
BVH build time
ray-cast time
archive write time
total time
threads
rays / second
```

---

# 24. Model Caching

Repeated models are extremely common.

Cache by resolved virtual path.

Example:

```cpp
std::unordered_map<std::string, std::shared_ptr<Mesh>>
```

Better:

```text
normalized path
    ↓
asset content hash
    ↓
parsed mesh
    ↓
BLAS
```

Do not parse or build a BVH for the same model repeatedly.

---

# 25. Ignore-Shadow Regions

The historical tool supported regions named similar to:

```text
ignoreshadow
```

Implement as a post-generation mask or sample-skip mechanism.

Recommended behavior:

```text
if sample ∈ ignoreShadowRegion:
    write unshadowed
```

Region matching rules need to be recovered experimentally.

Potential matching:

```text
case-insensitive name starts with "ignoreshadow"
```

Do not finalize this without testing against the historical tool.

---

# 26. Alpha Tile Handling

The historical utility reportedly included support related to ignoring alpha tiles.

Possible interpretation:

- tiles made invisible using alpha texture techniques should not receive static shadow
- certain boundary tiles should stay clear
- imported terrain alpha behavior

This is not required for Milestone 1.

Implement only after the basic generator works.

---

# 27. CLI Design

Initial executable:

```text
w3shadow
```

Example:

```text
w3shadow generate MyMap.w3x --output MyMap_shadowed.w3x
```

Options:

```text
--war3-dir <path>

--graphics auto|classic|reforged|definitive

--light-x <float>
--light-y <float>
--light-z <float>

--threads <n>

--terrain
--no-terrain

--doodads
--no-doodads

--destructibles
--no-destructibles

--ignore-alpha
--no-ignore-alpha

--honor-ignore-shadow
--no-honor-ignore-shadow

--output <file>

--in-place

--backup

--verbose

--dump-shadow <file.shd>

--dump-scene <file.obj>
```

Debugging export to `.obj` is strongly recommended.

It gives an easy way to inspect whether geometry transforms are correct.

---

# 28. Suggested Repository Layout

```text
w3shadow/
├─ CMakeLists.txt
├─ README.md
├─ LICENSE
│
├─ src/
│  ├─ main.cpp
│  │
│  ├─ cli/
│  │  ├─ Cli.cpp
│  │  └─ Cli.hpp
│  │
│  ├─ archive/
│  │  ├─ MapArchive.cpp
│  │  └─ MapArchive.hpp
│  │
│  ├─ assets/
│  │  ├─ AssetProvider.hpp
│  │  ├─ CompositeAssetProvider.cpp
│  │  ├─ MapAssetProvider.cpp
│  │  ├─ CascAssetProvider.cpp
│  │  └─ DirectoryAssetProvider.cpp
│  │
│  ├─ formats/
│  │  ├─ W3E.cpp
│  │  ├─ W3E.hpp
│  │  ├─ DOO.cpp
│  │  ├─ DOO.hpp
│  │  ├─ SHD.cpp
│  │  ├─ SHD.hpp
│  │  ├─ MDX.cpp
│  │  └─ MDX.hpp
│  │
│  ├─ objects/
│  │  ├─ ObjectDatabase.cpp
│  │  └─ ObjectDatabase.hpp
│  │
│  ├─ scene/
│  │  ├─ Mesh.cpp
│  │  ├─ Mesh.hpp
│  │  ├─ Scene.cpp
│  │  ├─ Scene.hpp
│  │  ├─ Instance.cpp
│  │  └─ Instance.hpp
│  │
│  ├─ geometry/
│  │  ├─ AABB.hpp
│  │  ├─ Ray.hpp
│  │  ├─ Triangle.hpp
│  │  ├─ BVH.cpp
│  │  ├─ BVH.hpp
│  │  └─ Intersection.cpp
│  │
│  ├─ shadow/
│  │  ├─ ShadowMap.cpp
│  │  ├─ ShadowMap.hpp
│  │  ├─ ShadowGenerator.cpp
│  │  └─ ShadowGenerator.hpp
│  │
│  └─ util/
│     ├─ BinaryReader.hpp
│     ├─ BinaryWriter.hpp
│     ├─ Logging.hpp
│     └─ PathUtil.hpp
│
├─ tests/
│  ├─ formats/
│  ├─ geometry/
│  ├─ maps/
│  └─ regression/
│
├─ fixtures/
│  ├─ flat-map/
│  ├─ one-cube/
│  ├─ cliff/
│  └─ imported-model/
│
└─ docs/
   ├─ FORMAT_NOTES.md
   ├─ SHADOW_ALGORITHM.md
   └─ COMPATIBILITY.md
```

---

# 29. Implementation Milestones

## Milestone 0 — Repository skeleton

Goal:

```text
CLI starts and parses arguments.
```

Deliverables:

- CMake project
- compiler warnings enabled
- tests configured
- CI build
- logging
- basic CLI

Compiler requirements:

```text
MSVC latest supported Visual Studio toolchain
or
Clang latest stable
```

Warnings should be treated seriously.

Recommended:

```text
/W4 /permissive-
```

or equivalent.

---

# 30. Milestone 1 — Validate `.shd` in Current Warcraft III

This is the most important first experiment.

Do not begin the model/raycasting system before proving the output mechanism works.

Steps:

1. Open a modern map.
2. Read dimensions.
3. Generate `.shd` of exact expected size.
4. Insert it into a copy of the map.
5. Test in current WC3.

Generate patterns:

```text
A. all 00
B. all FF
C. checkerboard
D. X gradient
E. Y gradient
F. four quadrants
```

Use the results to determine:

- lit/shadow byte semantics
- X direction
- Y direction
- row order
- border behavior
- any padding
- exact dimensions

Acceptance criteria:

```text
The modified map loads without corruption.

Artificial patterns appear exactly where expected.

The tool can round-trip the map archive without modifying unrelated data.
```

---

# 31. Milestone 2 — Terrain Parser

Implement:

```text
war3map.w3e
```

Output diagnostics:

```text
Map:
  width tiles
  height tiles
  vertex count
  minimum Z
  maximum Z
  cliff count
```

Add:

```text
--dump-terrain terrain.obj
```

Acceptance:

- flat map exports flat
- hills appear correctly
- cliffs have correct orientation
- map origin aligns with doodads later

---

# 32. Milestone 3 — Terrain-Only Shadows

Build terrain triangles.

Build BVH.

Cast rays against terrain only.

Acceptance test maps:

```text
flat
single hill
single cliff
ridge
valley
```

Compare visually with World Editor output.

Do not require pixel-perfect parity initially.

---

# 33. Milestone 4 — Doodad Placement

Parse:

```text
war3map.doo
```

At first use synthetic test models.

Example:

```text
cube.mdx
```

or internally generated cube geometry.

This isolates placement mathematics from MDX parsing.

Test:

```text
one cube
rotated cube
scaled cube
non-uniform scale
multiple cubes
```

Acceptance:

- exported world `.obj` geometry aligns with World Editor placement
- generated shadows rotate and scale correctly

---

# 34. Milestone 5 — Object Data Resolution

Resolve:

```text
rawcode → model path
```

Support both:

- stock objects
- custom map objects

Output statistics:

```text
Doodads:             15422
Resolved models:     15401
Unresolved objects:     21
Unique models:         185
```

Provide diagnostic list for unresolved assets.

---

# 35. Milestone 6 — MDX Geometry

Add production MDX parser or integrate an existing maintained implementation.

Acceptance:

- load simple stock doodad
- load tree
- load building
- load Reforged model
- load custom imported MDX
- no crashes on unsupported chunks
- clear warning when a feature is ignored

Security requirement:

All binary parsing must validate:

- offsets
- lengths
- chunk sizes
- counts
- arithmetic overflow
- file bounds

Map files are untrusted input.

---

# 36. Milestone 7 — CASC

Detect Warcraft III installation or accept:

```text
--war3-dir
```

Never depend exclusively on registry lookup.

Resolution strategy:

```text
CLI explicit path
    ↓
configured path
    ↓
automatic detection
```

Asset request:

```text
Doodads\Terrain\AshenTree\AshenTree0.mdx
```

must resolve from current WC3 data.

Acceptance:

- current installed game data loads successfully
- tool does not require historical MPQs
- no dependency on `war3Patch.mpq`

---

# 37. Milestone 8 — Full Doodad Shadow Calculation

Build:

```text
model mesh
    ↓
BLAS
    ↓
instances
    ↓
TLAS
```

Calculate complete map.

Measure performance.

Acceptance target:

- valid large map
- thousands of doodads
- no memory explosion
- deterministic output
- thread count changes performance but not output

---

# 38. Milestone 9 — Destructibles

Support destructible object definitions separately where needed.

Investigate:

- alive model
- dead model
- alternate variations
- editor placement state

Use whatever state World Editor uses for static shadows.

---

# 39. Milestone 10 — Compatibility Features

Implement after core correctness:

- `ignoreshadow`
- alpha tiles
- special object flags
- animated geoset handling
- transparent geosets
- shadow filtering
- special terrain rules

Each feature needs an isolated test map.

---

# 40. Milestone 11 — GUI

Only after CLI generator is reliable.

Suggested GUI:

```text
┌─────────────────────────────────────────────┐
│ Warcraft III ShadowMap Generator           │
├─────────────────────────────────────────────┤
│ Map: [ MyMap.w3x                   ][...]   │
│ WC3: [ C:\...\Warcraft III         ][...]   │
│                                             │
│ Graphics: [Auto ▼]                          │
│                                             │
│ Shadow direction                            │
│ X [ 1.000 ]                                 │
│ Y [ 1.000 ]                                 │
│ Z [-1.000 ]                                 │
│                                             │
│ [x] Terrain                                 │
│ [x] Doodads                                 │
│ [x] Destructibles                           │
│ [x] Honor ignore-shadow regions             │
│ [x] Ignore alpha terrain                    │
│                                             │
│ Threads: [ Auto ▼ ]                         │
│                                             │
│ ┌─────────────────────────────────────────┐ │
│ │              preview                    │ │
│ └─────────────────────────────────────────┘ │
│                                             │
│ [ Generate Shadows ]                        │
└─────────────────────────────────────────────┘
```

---

# 41. Determinism

Generated output should be deterministic.

Same:

```text
map
tool version
settings
asset data
```

must produce identical:

```text
war3map.shd
```

regardless of thread scheduling.

This simplifies regression testing.

---

# 42. Regression Testing

Maintain fixture maps.

Minimum fixture set:

```text
01_flat.w3x
02_checker_orientation.w3x
03_hill.w3x
04_cliff.w3x
05_single_cube.w3x
06_rotated_cube.w3x
07_scaled_cube.w3x
08_stock_tree.w3x
09_many_trees.w3x
10_imported_model.w3x
11_custom_object.w3x
12_ignore_shadow_region.w3x
13_alpha_tiles.w3x
14_reforged_model.w3x
15_large_480_map.w3x
```

For every fixture store:

```text
expected SHD hash
optional reference SHD
screenshots
tool settings
WC3 build used for validation
```

---

# 43. Comparison Method

Three outputs are useful:

```text
A = current Warcraft III World Editor
B = old Oger-Lord ShadowMap Calculator
C = new implementation
```

For legacy-compatible maps:

```text
compare B vs C
```

For modern maps:

```text
compare A vs C
```

Metrics:

```text
total differing pixels
percentage differing
connected error regions
edge disagreement
false shadow pixels
missing shadow pixels
```

A diagnostic image should be generated:

```text
black  = equal
white  = mismatch
```

This makes algorithm tuning much easier.

---

# 44. Potential World Editor Differences

Do not assume a simple ray tracer will exactly reproduce World Editor.

Possible differences include:

- projected model bounds instead of triangles
- low-resolution collision-like geometry
- geoset filtering
- shadow-specific object models
- terrain offset
- rasterization instead of ray tracing
- sample-center differences
- shadow dilation
- morphology filtering
- object editor shadow flags
- special-case cliffs
- model animation pose
- alpha material rejection
- quantized light vector
- one-sided faces
- duplicate triangles
- minimum object height

These should be resolved experimentally.

---

# 45. Reverse Engineering Strategy

Do not begin by decompiling the old executable.

Preferred order:

```text
1. Reproduce observable behavior.
2. Inspect map formats.
3. Compare generated files.
4. Create controlled fixture maps.
5. Infer algorithm.
6. Only investigate old executable details when a behavior cannot be explained.
```

Useful reverse-engineering questions:

- What geometry is considered?
- What geosets are excluded?
- Which pose is used?
- How are cliffs represented?
- What sample point corresponds to each SHD byte?
- What epsilon is used?
- Is shadow geometry expanded?
- Are rays infinite?
- Does terrain self-shadow?
- Are doodads clipped to map bounds?
- Are hidden/invisible doodads excluded?
- How do custom scaling fields behave?
- How are variations selected?

---

# 46. Performance Goals

Targets should be measured, not assumed.

Suggested development target:

```text
Large 480×480 map
Modern desktop CPU
16 threads

Goal:
seconds to low tens of seconds,
not several minutes.
```

Record:

```text
parse time
asset loading time
BVH build time
shadow calculation time
archive write time
peak RAM
```

Potential optimization sequence:

```text
1. correct algorithm
2. model cache
3. BVH
4. threading
5. two-level BVH
6. SIMD
7. ray packets
8. specialized terrain acceleration
```

Do not optimize parser code prematurely.

---

# 47. Error Handling

Errors should be explicit and actionable.

Bad:

```text
Failed.
```

Good:

```text
ERROR:
Unable to resolve model for doodad rawcode 'LTlt'.

Object:
  Rawcode: LTlt
  Position: 1024.0, -512.0, 128.0

Attempted model path:
  Doodads\Terrain\AshenTree\AshenTree0.mdx

Asset providers checked:
  map imports
  C:\Program Files\Warcraft III\...
```

Return meaningful process exit codes.

Example:

```text
0 success
1 invalid arguments
2 map open error
3 malformed map data
4 asset resolution failure
5 shadow generation failure
6 save failure
```

---

# 48. Safe Map Writing

Never modify the original during initial development.

Default:

```text
input:
MyMap.w3x

output:
MyMap.shadowed.w3x
```

For `--in-place`:

```text
MyMap.w3x
    ↓
write MyMap.w3x.tmp
    ↓
validate archive
    ↓
rename original → backup
    ↓
rename tmp → original
```

Avoid direct destructive rewriting.

---

# 49. Diagnostics

Provide:

```text
--verbose
```

and possibly:

```text
--debug-dir
```

Output:

```text
debug/
├─ terrain.obj
├─ scene.obj
├─ generated.shd
├─ generated.png
├─ scene_stats.txt
└─ unresolved_assets.txt
```

A PNG rendering of the generated SHD is particularly useful.

Example mapping:

```text
00 → white
FF → black
```

---

# 50. Shadow Preview Image

Always provide an internal function to convert SHD to image.

This is valuable even if no GUI exists.

Example:

```text
w3shadow inspect map.w3x --export-shadow shadow.png
```

Also:

```text
w3shadow inspect-shadow war3map.shd --width 256 --height 256
```

---

# 51. Validation Before Geometry Work

The very first implementation task should be:

> Generate artificial `war3map.shd` files and verify them in the current Warcraft III build.

This establishes whether all assumptions regarding:

- size
- orientation
- byte values
- archive insertion
- map loading

are correct.

Do not proceed to a full geometry engine until this passes.

---

# 52. Recommended First Coding Tasks for Codex

Codex should initially implement only the foundation.

## Task 1

Create C++23 CMake project.

Requirements:

- `/src`
- `/tests`
- executable `w3shadow`
- strict warnings
- Windows build
- no GUI

---

## Task 2

Implement:

```text
ShadowMap
```

Features:

```cpp
ShadowMap(uint32_t mapWidthTiles, uint32_t mapHeightTiles);

uint32_t widthPixels() const;
uint32_t heightPixels() const;

void clear(bool shadowed);
void set(uint32_t x, uint32_t y, bool shadowed);
bool get(uint32_t x, uint32_t y) const;

std::span<const std::byte> bytes() const;
```

Use checked indexing.

---

## Task 3

Add pattern generation.

CLI:

```text
w3shadow pattern \
    --map-width 64 \
    --map-height 64 \
    --pattern checker \
    --output test.shd
```

Patterns:

```text
black
white
checker
x-gradient
y-gradient
quadrants
```

---

## Task 4

Implement SHD preview PNG export.

Example:

```text
w3shadow pattern \
    --map-width 64 \
    --map-height 64 \
    --pattern checker \
    --output test.shd \
    --png test.png
```

---

## Task 5

Add map archive reading.

Initially only:

```text
list files
extract war3map.w3e
replace war3map.shd
save copy
```

---

# 53. Coding Quality Requirements

Use strong types where practical.

Bad:

```cpp
int width;
int height;
```

Better:

```cpp
uint32_t widthTiles;
uint32_t heightTiles;
```

Avoid global mutable state.

Prefer RAII.

Use:

```cpp
std::filesystem::path
std::span
std::string_view
std::optional
std::expected
```

where appropriate.

Avoid exceptions for routine parse failures if `std::expected` provides clearer control flow.

Example:

```cpp
std::expected<W3EMap, ParseError>
parseW3E(std::span<const std::byte> bytes);
```

---

# 54. Binary Parser Rules

All binary parsers must:

1. bounds-check every read
2. validate counts before allocation
3. validate multiplication for overflow
4. reject impossible sizes
5. preserve parser offset in error information

Example error:

```text
MDX parse error
chunk: GEOS
offset: 0x00172A80
expected: 4096 bytes
remaining: 274 bytes
```

Never trust values in map files.

---

# 55. Avoid These Early Mistakes

Do not:

- build the GUI first
- assume current WC3 still uses old MPQ game archives
- duplicate a full game renderer
- implement particles
- implement textures before needed
- transform repeated model meshes into unique copies without measuring memory
- hard-code Warcraft III installation path
- silently ignore unsupported model chunks
- overwrite user maps by default
- use brute-force triangle testing
- optimize before correctness
- assume World Editor output is always mathematically ideal
- depend on undocumented memory layouts

---

# 56. External Projects Worth Studying

These projects or ecosystems may contain useful implementation details:

## Retera Model Studio

Useful for:

- modern MDX handling
- Reforged-era model support
- Warcraft III asset resolution concepts
- CASC interaction

Repository:

https://github.com/Retera/ReterasModelStudio

---

## wc3libs

Useful for:

- Warcraft III map formats
- `war3map.w3e`
- `war3map.doo`
- `war3map.shd`
- object data
- map archives

Repository:

https://github.com/inwc3/wc3libs

---

## HiveWE

Useful for:

- modern Warcraft III map handling
- terrain interpretation
- current asset loading
- editor-style object placement
- potentially modern CASC paths

Repository:

https://github.com/stijnherfst/HiveWE

---

## HiveWorkshop ShadowMap Calculator Thread

Useful for:

- original program behavior
- reported compatibility failures
- user observations
- original controls/options

https://www.hiveworkshop.com/threads/shadowmap-calculator-made-by-oger-lord.294887/

---

## war3map.shd Format Discussion

Useful for:

- understanding binary shadow data
- historical reverse-engineering notes

https://www.hiveworkshop.com/threads/war3map-shd-file-format.213568/

---

# 57. Questions Codex Should Not Guess

These require empirical validation.

Mark them explicitly as TODOs.

```text
RESOLVED-SHD-001 (29 September 2026)
0x00 is lit and 0xFF is fully shadowed in the current Warcraft III client.

RESOLVED-SHD-002 (29 September 2026)
X is stored left-to-right. The first stored SHD row appears at the bottom of the map,
so file rows are bottom-to-top relative to conventional screen previews.

RESOLVED-SHD-003 (29 September 2026)
Square and rectangular in-game tests confirmed exactly width * height * 16 bytes.
All tested borders aligned with no header, offset, or padding.

TODO-GEO-001
Does terrain cast static shadow onto itself?

TODO-MDX-001
Which geosets cast shadows?

TODO-MDX-002
Which animation pose is used?

TODO-MDX-003
Are transparent geosets ignored?

RESOLVED-OBJ-001 (30 September 2026)
The stock `shadow` column and map override fields `dshd`/`bshd` control static-shadow
participation. Empty string values represent Has shadow: False and are excluded.

TODO-OBJ-002
How are doodad variations mapped to model files?

TODO-REGION-001
What exact naming/matching behavior was used for ignoreshadow?

TODO-ALPHA-001
What exactly does alpha-tile exclusion mean in the old calculator?

TODO-LIGHT-001
Is the default vector exactly (1,1,-1)?

TODO-RAY-001
What ray-origin offset best matches WC3?

TODO-FILTER-001
Does World Editor post-process/dilate generated shadows?
```

---

# 58. Minimum Viable Product

The MVP does not need perfect World Editor parity.

MVP success criteria:

```text
1. Open a current WC3 map.
2. Resolve current game data.
3. Parse terrain.
4. Parse doodads.
5. Resolve stock and imported MDX.
6. Construct world-space geometry.
7. Generate static shadows.
8. Produce valid war3map.shd.
9. Save a working output map.
10. Complete significantly faster than World Editor on large maps.
```

Visual results should be reasonably similar to the old ShadowMap Calculator.

---

# 59. Definition of Done for Version 1.0

Version 1.0 should satisfy:

### Compatibility

- current Warcraft III installation
- Classic/SD map
- Reforged/HD map
- custom imported MDX
- custom doodads
- large maps

### Reliability

- deterministic
- non-destructive by default
- no crash on malformed assets
- clear error reporting

### Performance

- multithreaded
- spatial acceleration
- model cache
- no obvious O(samples × triangles) behavior

### Usability

CLI example:

```text
w3shadow generate MyMap.w3x
```

should work with sensible defaults.

### Diagnostics

At minimum:

```text
--verbose
--dump-shadow
--dump-scene
```

---

# 60. Completed Foundation and Recommended Follow-Up

The original `pattern`, `inspect`, and `replace-shd` validation step is complete, including current-client byte meaning, row orientation, exact size, and border behavior. The full `generate` pipeline is implemented in `ShadowMapTool/`.

The 30 September 2026 compatibility pass added current Warcraft III DOO version 13
records (group/color fields, roll/pitch, and embedded lights), Object Editor shadow
filtering, editable light vectors, independent terrain/doodad/destructible options,
full-map existing/calculated previews, and an explicit Test mode for diagnostic patterns.
Automatic alpha-tile exclusion remains pending because it requires terrain rawcode-to-BLP
resolution and reliable alpha/variation inspection; the UI and README document the
temporary-replacement and IgnoreShadow-region workflows without claiming automatic support.

The next work is empirical compatibility validation with controlled maps from section 42: compare terrain self-shadowing, stock and custom model variations, default pose/geoset visibility, transparent materials, cliff faces, `IgnoreShadow...` naming, alpha tiles, the default light vector, ray offset, and any World Editor filtering/dilation. Record each result against the remaining section 57 TODO rather than changing rules by guesswork.

## 60.1 First paired World Editor comparison

The 64 x 64 maps `shadowmaptest_native.w3x` and
`shadowmaptest.shadowed_v2.w3x` provide the first controlled mixed-scene pair.
Their W3E terrain and DOO placement payloads are byte-identical. Both use W3E v12,
DOO v13.11, 50 regular placements, no special cliff-doodad records, and a 256 x 256
SHD. Regenerating the native map with the version-2 Smooth sub-tile settings
reproduces the tool fixture exactly.

Measured binary-mask results:

- World Editor: 2,050 shadowed samples.
- Smooth sub-tile, vector `(1, 1, -1)`: 4,631 samples; 29.4% intersection-over-union.
- Classic triangles, vector `(1, 1, -1)`: 4,238 samples; 33.0% intersection-over-union.
- Smooth terrain alone produces 1,891 samples; placed objects alone produce 2,998.
- The best tested raw candidate was Classic triangles with `Z = -0.75`: 5,173
  samples and 35.4% intersection-over-union.
- A one-sample full-neighborhood erosion of that candidate reaches 43.1%, but still
  has 2,657 samples and is not close enough to identify a general World Editor rule.
- Flips and 90/180-degree rotations all score substantially worse than the unmodified
  orientation. The discrepancy is therefore geometry/filtering, not SHD orientation.

This pair disproves the assumption that Smooth sub-tile is more World Editor-compatible;
it remains useful specifically for reducing diagonal terrain facets. The evidence is
consistent with missing posed/geoset visibility and texture-alpha/material handling for
MDX models plus different cliff-surface reconstruction. It does not justify silently
changing the default light vector or adding a hard-coded erosion pass. Isolated reference
pairs for flat terrain, one cliff, one opaque doodad, and one alpha-tested tree are the
next minimum fixtures needed to separate those effects.

## 60.2 Binary edge supersampling

Version 1.2 separates terrain reconstruction from final SHD edge quality. Each fixed
32-world-unit SHD cell can now cast a regular 1 x 1, 2 x 2, or 4 x 4 subpixel ray grid.
A cell becomes shadowed only when more than half of its rays are occluded. The output
remains the confirmed Warcraft-compatible binary 0x00/0xFF format at four cells per
terrain tile; supersampling improves the binary coverage decision rather than claiming
unsupported grayscale or a higher-resolution SHD.

The GUI names the modes Fast 1x, Smooth 2x, and Ultra 4x and defaults to Ultra.
The CLI exposes the same setting as `--edge-samples 1|2|4`. Fast 1x retains the
version-1/version-2 single-center-ray result. On the 64 x 64 reference scene, 1x, 2x,
and 4x perform 65,536, 262,144, and 1,048,576 rays respectively; local ray-stage
measurements were 0.003, 0.006, and 0.019 seconds. Large production maps should expect
the ray-stage cost to scale approximately with those sample counts.

The 480 x 480 PotS validation map also completed successfully in Ultra mode: 50,049
resolved shadow-casting placements, 5,335,128 triangles, and 58,982,400 rays took
17.334 seconds total on the development machine. Fast 1x took 13.317 seconds because
BVH construction dominates both runs. Ultra changed 68,088 of 3,686,400 output cells
(1.85%) and reduced horizontal/vertical light-shadow boundary transitions from 646,831
to 522,441, a 19.2% reduction in binary edge noise.

---

# 61. Proposed Codex Instruction

Use the following as the implementation directive:

> Build this project incrementally and preserve strict module boundaries. Start with SHD generation and map archive integration only. Do not implement MDX, CASC, terrain ray tracing, or GUI until SHD orientation and current-WC3 compatibility have been empirically validated. Every binary parser must be bounds checked. All output must be deterministic. The original map must never be modified unless `--in-place` is explicitly supplied. Add tests for every binary format and geometry primitive before using it in the full pipeline.

---

# 62. Summary

The project is technically feasible.

The recommended strategy is:

```text
DO:
modern standalone implementation
current map parsing
current asset resolution
geometry reconstruction
BVH-accelerated ray casting
direct SHD generation

DO NOT:
patch old binary as the primary solution
depend on legacy MPQs
hook Warcraft III
inject into World Editor
build the GUI before validating SHD
```

The highest-risk area is not writing `war3map.shd`.

The main engineering uncertainty is reproducing the exact set of geometry and filtering rules used by Warcraft III / the historical calculator.

That uncertainty can be reduced systematically through small controlled maps and binary/image comparisons.

The project should therefore be treated as:

```text
file-format validation
    ↓
geometry reconstruction
    ↓
behavioral reverse engineering
    ↓
performance optimization
    ↓
GUI
```

rather than as a monolithic reverse-engineering project.
