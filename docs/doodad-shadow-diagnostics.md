# Doodad shadow diagnosis and regression checks

## Confirmed defect - 21 November 2026

Stock SLK/profile shadow fields can contain numeric text: `shadow=0` means False and `shadow=1` means True. The old `hasShadowValue` implementation accepted every nonempty string except `_`, so it incorrectly accepted `0`. The installed modern `DoodadSkins.txt` inspected during diagnosis contained 260 `shadow=0` entries. An example is `APms` (Ashenvale mushrooms).

Explicit integer `dshd=0`/`bshd=0` map overrides were already decoded correctly. This explains how an explicit True-to-False test can pass while an object inheriting False from stock data fails. Custom objects may inherit this default without serializing another shadow modification. The parser now rejects numeric text `0` as well as empty/underscore shadow values. Named destructible shadow textures remain supported.

This is a reproduced code/data defect, not confirmation of the original reporter's exact cause. Obtain their tool version, minimal saved map, affected rawcode, imported model and its textures, Warcraft asset version, and session log to establish that link. Releases before 1.3.2 also lack the explicit integer-False fix.

## Model and texture behavior

The generator resolves the placed skin first, falling back to the placed object's rawcode. Regular map overrides are applied before map skin overrides. The resolved `castsShadow` gate runs before model loading and model-cache lookup. A correctly resolved False therefore cannot be overridden by an MDX material or texture.

For eligible casters, missing/unsupported alpha textures can make geometry cast opaque shadows. Bind-pose geometry, animated visibility, large enclosing geometry, and shared/imported model paths can also explain an incorrect shape. Those issues do not bypass False. Other potential explanations include a different placed skin/type, an older tool build, a different saved map, an existing SHD preview mistaken for a new calculation, another object's shadow, or a dark surface built into the model itself. Malformed object overrides now stop generation rather than continuing with potentially incorrect defaults.

## Reliable World Editor test

Prepare the map and all Object Editor edits manually in World Editor. Use a small flat map, separate placements well, avoid enclosing/background models, and save before opening the tool.

1. Place a stock control with inherited False, such as `APms` in the inspected modern assets. Confirm the field in the current editor. Do not rely only on a True-to-False edit, since that can serialize an explicit override and miss the inherited-default bug.
2. Make a custom copy that changes only its model to the reported imported model, keeping inherited False. Include all required model textures.
3. Make copies using the same native/imported models with explicit False and explicit True. Keep scale, height, variation, and rotation identical. Test one isolated caster at a time if shadow footprints could overlap.
4. In ShadowMapTool, start with an empty exclusion list. Disable Terrain and Destructibles for doodad tests, or Terrain and Doodads for destructible tests. Disable IgnoreShadow rectangles and alpha-terrain masking for this flat control. Use Fast 1x, hard edges, zero minimum-island cleanup, and a fixed light direction to reduce unrelated filtering variables.
5. Calculate a fresh preview for each case. True must contribute geometry and a visible control shadow; False must contribute no caster geometry regardless of model. The CLI `--dump-scene` and `--dump-shadow` outputs help distinguish geometry from an in-game appearance issue. Logs separately count object-data-disabled and manually excluded placements.
6. Check the True type in Exclusions and Apply: it must disappear from caster geometry. Uncheck it and recalculate: the baseline SHD bytes must return. The False control must remain disabled throughout. Two different rawcodes using the same model must be independently excludable.
7. Save as a copy and open that exact copy directly in Warcraft III before resaving through World Editor. If darkness remains when all caster categories are off and the generated SHD is empty, investigate model/texture or engine rendering rather than attributing it to generated static SHD.
8. Repeat with the reporter's actual placed skin, asset backend/version, and DOO compatibility setting if the minimal case passes. Compare editor-generated and tool-generated copies from the same saved input.

## Exclusion list contract

- Empty by default; unchecked means follow object data, never force a shadow.
- One row per placed rawcode, covering every placement and skin of that type. The same model used by another rawcode remains independent.
- The displayed setting is the resolved object-data value, not a promise that an object produces visible pixels. Mixed skin values and missing definitions are labeled.
- Search matches name, rawcode, and model. Custom map string-table names are resolved when available; stock comments/model names provide fallbacks.
- Apply persists `<map filename>.w3shadow-exclusions`; Cancel does not persist changes. Presets do not carry exclusions between maps.
- GUI and CLI generation use the same filtering. Map copies receive the current sidecar; moving/renaming a map externally requires moving/renaming its sidecar too.
- Excluding a caster does not clear shadows from other casters that overlap its location. `IgnoreShadow` regions are receiver-area masks and serve a different purpose.

## Automated verification

The tests cover stock numeric False/True in SLK and skin profiles, empty/underscore and named shadow textures, a custom imported-model object inheriting False, explicit True/False overrides, per-map save/load and clearing, malformed sidecar rejection, complete and selective caster exclusion, shared-model cache independence, and byte-identical SHD restoration after clearing exclusions. The legacy installed-asset integration test checks eligible plus disabled placement counts. The GUI smoke test exercises checkbox notifications, search, and Cancel. Full in-game confirmation of the reporter's model remains manual.
