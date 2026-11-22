# ShadowMapTool v1.3.5

This release fixes model visibility and material filtering that could create shadows from invisible portrait backgrounds or omit visible prop surfaces. Thanks to **HerrDave** for reporting both problems.

## Highlights

- Geoset and material alpha now follow the start of the model's **Stand** sequence. Geometry hidden in Stand but visible in Portrait is excluded, while surfaces animated visible in Stand can cast shadows.
- Opaque portions of **Blend** materials now cast shadows, with texture and opacity cutouts. Additive, modulate and team-glow effects remain excluded.
- Modern MDX models now use the actual diffuse texture slot instead of the legacy placeholder. Material animation parsing supports layouts through MDX 1800.
- Visibility diagnostics report removed geometry and the sampled sequence/frame. Added regression tests cover portrait geometry, missing Blend surfaces, modern texture slots and malformed animation tracks.

Object Editor **Has a Shadow = False** remains effective. Per-map exclusions still start empty and never force disabled objects to cast shadows.

## Validation and limits

The Windows release build and all three test groups passed locally. A reference-map generation against installed Warcraft III assets resolved all 50 placements with no unresolved models.

HerrDave's actual map and affected models were unavailable, so confirmation of his specific imports remains pending. Visibility is sampled once, and geometry still uses the bind/default pose; animated bone scaling, bone motion and texture animation are not reconstructed. A portrait background hidden through bone transforms may still need a per-map exclusion.

Use **Save as copy** for the first comparison. The [diagnostic guide](https://github.com/ValdemarLabs/ShadowMapTool/blob/v1.3.5/docs/doodad-shadow-diagnostics.md) includes a focused retest procedure that separates doodads, destructibles and unit shadows.

The Windows download includes the GUI, CLI, CascLib runtime, documentation and diagnostic guide. See the [changelog](https://github.com/ValdemarLabs/ShadowMapTool/blob/v1.3.5/CHANGELOG.md) for full details.
