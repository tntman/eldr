# Moving camera mode

<!-- summary -->
While the camera moves, Eldr renders smaller (to a GPU frame budget) with one shadow ray and paused GI; after the stop, quality steps up over a few frames and the film starts from the TAA image.

- **Compare:** open `showcase/2026-10-30_01_moving_camera/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `moving` (the defaults) vs `full` vs `abrupt`.
- **Look at:** Tumble the camera: moving frames are softer and much faster; once still, both are the same.
<!-- summary -->

While the camera moves Eldr cuts the per-frame work, and brings full quality back gradually once it stops:

- `eldr:movingRenderScale` (0.5): renders at half size and TAA upscales. It now also works with depth of field and an open shutter, so it applies in Houdini, whose free camera has an open shutter.
- `eldr:movingShadowRays` (1): one traced shadow ray per light.
- `eldr:movingGiPause` (on): settled GI probes skip their update while only the camera moves.
- `eldr:movingFrameBudget` (33 ms): the moving render scale follows the measured GPU time, so this scene may stay larger than half size.
- `eldr:rampFrames` (6): after the stop, size, shadow rays and GI updates step up over 6 frames instead of in one.
- `eldr:filmWarmStart` (4): the film starts from the TAA image, so the first still frames are not noisier than the last moving one.

Then tumble with `/Render/abrupt` (the last three off): the stop pops from half size to full in one slow frame and the first film frames are noisier.

What to look at: tumble with `/Render/moving` (the defaults), then with `/Render/full`. Moving frames are softer and much faster. Once the camera stops, the image is the same under both.

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_01_moving_camera\scene.usda`, then pick the settings prim, or flip the three settings in Eldr > Render Settings.
- Houdini: reference the scene in a LOP network, pick Eldr in the viewport and tumble. The Eldr tab has the three settings under Performance and debug.
- Measure: `viewportBench scene.usda /cam --houdini --stop 1500` (add `--set eldr:movingRenderScale=1` for the A/B).

1920x1080, GTX 1080 Ti, moving: 64 ms a frame with everything kept, 12.5 ms with the defaults.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_01_moving_camera\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-30_01_moving_camera/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
