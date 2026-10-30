# Bloom and tonemapping

The Cornell box lit by a glowing panel (40) and a warm bulb (60, 30, 8) seen directly, so the image has values far over 1. Linear Beauty (`eldr:productLinear`, on by default) makes husk's and Houdini's beauty linear with no tonemap and no bloom, so every prim here turns it off; usdview and usdrecord tonemap either way. `eldr:colorLimit` 1000 keeps the emitters' full HDR (the default limit clamps them at 20). Eldr's 0 means off, but Karma's Color Limit 0 renders black, so the scene uses 1000 for both.

## What to look at

- `/Render/aces` (the default tonemapper, bloom 0.04): highlights roll off, the bulb clips towards yellow.
- `/Render/agx` (`eldr:tonemapper` agx): softer; the bulb's orange desaturates towards white.
- `/Render/bloom` (`eldr:bloomIntensity` 0.4): a wide glow around the panel and the bulb.
- `/Render/linear` (`eldr:tonemapper` none, bloom 0): exposed linear, hard clipping.
- `eldr_default` and `karma` write linear EXRs (no tonemap): view them in a viewer with a display transform.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_17_bloom_tonemap\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/aces scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
