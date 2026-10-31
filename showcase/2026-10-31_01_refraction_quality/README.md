# Refraction Quality

<!-- summary -->
More paths through rough glass instead of more film samples (`eldr:refractQuality`, Karma's `karma:object:refractquality`).

- **Compare:** open `showcase/2026-10-31_01_refraction_quality/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `refractQuality1` vs `refractQuality4`.
- **Look at:** The frosted ball on the right: grain and sparkles at 1, a smooth blur at 4. The clear and green balls don't change.
<!-- summary -->

`tests/scenes/glass.usda`: a clear, a green and a frosted glass ball (OpenPBR, roughness 0.35) and a thin pink pane, lit by a sun and StinsonBeach.hdr. Both A/B prims render 32 film samples with ray-traced refraction (`eldr:rtRefraction`), where Refraction Quality sends that many paths per pixel through rough glass. Smooth glass takes one path, so the clear and green balls cost nothing extra.

## What to look at

- `/Render/refractQuality1`: one path per pixel and sample. The frosted ball is grainy, with sparkles where the rough glass catches the sun.
- `/Render/refractQuality4` (`eldr:refractQuality` 4): four paths through the frosted ball. Its noise matches about 128 samples at quality 1, for about 1.7 times the cost per sample.

Screen-space refraction (the default, `eldr:rtRefraction` off) uses Refraction Quality for its blur taps; its film is already clean at 16 samples. Traced refraction needs a ray-tracing capable GPU; without one, both prims look alike. StinsonBeach.hdr ships with USD: `usdenv.cmd` puts its folder on the resolver's search path. For husk set `PXR_AR_DEFAULT_SEARCH_PATH` to `<USD>\lib\usd\hdx\resources\textures`.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-31_01_refraction_quality\scene.usda`, then pick a settings prim (right-click, set as active), or flip the setting in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/refractQuality4 scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
