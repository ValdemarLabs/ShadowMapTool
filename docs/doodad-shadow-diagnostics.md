# Doodad shadow diagnosis and regression checks

## Confirmed defect - 21 November 2026

Stock SLK/profile shadow fields can contain numeric text: `shadow=0` means False and `shadow=1` means True. The old `hasShadowValue` implementation accepted every nonempty string except `_`, so it incorrectly accepted `0`. The installed modern `DoodadSkins.txt` inspected during diagnosis contained 260 `shadow=0` entries. An example is `APms` (Ashenvale mushrooms).

Explicit integer `dshd=0`/`bshd=0` map overrides were already decoded correctly. This explains how an explicit True-to-False test can pass while an object inheriting False from stock data fails. Custom objects may inherit this default without serializing another shadow modification. The parser now rejects numeric text `0` as well as empty/underscore shadow values. Named destructible shadow textures remain supported.

This is a reproduced code/data defect, not confirmation of the original reporter's exact cause. Obtain their tool version, minimal saved map, affected rawcode, imported model and its textures, Warcraft asset version, and session log to establish that link. Releases before 1.3.2 also lack the explicit integer-False fix.

## Model and texture behavior

The generator resolves the placed skin first, falling back to the placed object's rawcode. Regular map overrides are applied before map skin overrides. The resolved `castsShadow` gate runs before model loading and model-cache lookup. A correctly resolved False therefore cannot be overridden by an MDX material or texture.

For eligible casters, missing/unsupported alpha textures can make geometry cast opaque shadows. Bind-pose geometry, bone-driven visibility, large enclosing geometry, and shared/imported model paths can also explain an incorrect shape. Geoset/material alpha visibility is sampled at the start of Stand in version 1.3.5 described below. Those issues do not bypass False. Other potential explanations include a different placed skin/type, an older tool build, a different saved map, an existing SHD preview mistaken for a new calculation, another object's shadow, or a dark surface built into the model itself. Malformed object overrides now stop generation rather than continuing with potentially incorrect defaults.

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

## Portrait backgrounds and missing prop shadows - 22 November 2026

HerrDave reports invisible built-in portrait backgrounds casting shadows (Fruit Stands), and missing shadows on walls, towers, tents and stakes. His screenshot shows both SD and HD shadow flags enabled for the Desert Wall doodad. The diagonal gate shadow is from a separate unit hidden with Ctrl+U, not from the arch doodads; the chicken is a destructible. Placed units are not inputs to this tool's doodad/destructible geometry pass. Keep those categories separate when comparing results.

The actual map and affected model files were unavailable, and the tested tool version is unconfirmed. The following defects were reproduced with synthetic models; they are candidate explanations for the report, not a verified diagnosis of those particular imports:

- The parser ignored GEOA static alpha and KGAO visibility tracks. A background hidden during Stand but visible during Portrait consequently remained in caster geometry.
- It used only static material alpha and rejected every Blend layer. A surface with static alpha zero and visible KMTA keys, or an opaque wall using Blend, could therefore lose all its shadow geometry.
- It read the legacy texture ID placeholder in modern layers instead of the diffuse subtexture slot. This could select an unrelated texture or team glow and incorrectly mask or remove the surface. The installed Warcraft III 3.0 Barn model confirmed that the placeholder and actual diffuse index can differ.

Version 1.3.5 selects exact Stand preferentially, otherwise the first Stand variant, and samples its starting frame. It uses static non-global visibility if there is no Stand; it never selects Portrait as the display sequence. Global tracks use time zero. Keys outside the chosen interval are ignored, and step/linear/Hermite/Bezier interpolation is supported at the interval boundary. Geoset filtering reports the model path, removed triangle count and sampled sequence/frame in the log.

Opaque portions of Blend surfaces now contribute geometry. Transparent and Blend texture alpha is multiplied by sampled geoset/material opacity and compared with the existing binary cutoff (192/255); faint effects remain excluded. Additive, add-alpha, modulate and team-glow layers remain excluded. Missing or unsupported BLP textures still produce a logged opaque fallback. Object Editor False and per-map exclusions are applied before any of this model handling.

Material parsing covers the classic and Reforged layouts through MDX 1800, including diffuse subtexture selection. This remains a static approximation: bone transforms/scaling, animated texture selection/UVs, camera-dependent geometry and complete HD shader behavior are not simulated. A portrait plane hidden by bone scaling rather than alpha needs further model-specific work. One initial Stand sample cannot reproduce every frame of a time-varying prop.

Format checks used the primary [mdx-m3-viewer geoset-animation reader](https://github.com/flowtsohg/mdx-m3-viewer/blob/master/src/parsers/mdlx/geosetanimation.ts) and [WhiteoutLib's MDX material layout documentation](https://github.com/FernandoS27/WhiteoutLib/blob/master/docs/MDX_FILE_FORMAT_SPECIFICATION.md#77-mtls--materials), cross-checked against locally installed game model bytes.

### Focused retest for HerrDave

1. Record the exact tool build, Warcraft version and asset source. On a small flat saved test map, place one Fruit Stand, one affected wall/tower/tent/stake, and a stock caster control well apart. Include their imported textures and test every variation actually used in his map.
2. Remove unit gates from the test copy in World Editor; Ctrl+U only hides them in the editor view. Test doodads alone first, then destructibles separately. Use empty Exclusions, explicit Has a Shadow (SD) True, Fast 1x, hard edges, zero minimum-island cleanup, and the same sun direction for both builds. Disable Terrain, IgnoreShadow rectangles and alpha-terrain masking as in the general test above.
3. Generate fresh copies from the same saved input with versions 1.3.4 and 1.3.5. Compare the tool preview, exported SHD and `--dump-scene` geometry. The Fruit Stand body should remain, its hidden portrait background should disappear, and eligible opaque prop surfaces should return. Keep the session logs, especially model-resolution, visibility, empty-mesh, alpha-texture and caster-span messages.
4. Confirm the generated copy directly in the game before a World Editor resave. Repeat with explicit False: that type must contribute no geometry. Then restore True and check/uncheck Exclusions to verify removal and restoration independently of the model cache.
5. If a prop still fails, provide a minimal test map or the single model plus its textures, object rawcode/skin, variation and log. Inspect its Stand versus Portrait visibility and bone transforms before choosing a further fix. The entire production map is unnecessary.

### Automated coverage

Synthetic two-geoset models and BVH ray tests cover hidden Portrait geometry, static hidden geosets, alpha keys restoring visible geometry, Stand selection over Portrait/alternate sequences, no-Stand fallback, global tracks, loop-boundary interpolation, animated material visibility, Blend cutouts and opacity, excluded effect filters, modern diffuse slots, and malformed tracks. Versions 800, 900, 1000, 1100, 1200 and 1800 have material-layout fixtures. Existing inherited/explicit False and per-map exclusion tests continue to run alongside these checks. Reporter-specific in-game confirmation remains outstanding.
