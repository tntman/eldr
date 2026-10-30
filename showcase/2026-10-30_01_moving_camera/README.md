# Moving camera mode

While the camera moves Eldr cuts the per-frame work and brings full quality back on the first still frame:

- `eldr:movingRenderScale` (0.5): renders at half size and TAA upscales. It now also works with depth of field and an open shutter, so it applies in Houdini, whose free camera has an open shutter.
- `eldr:movingShadowRays` (1): one traced shadow ray per light.
- `eldr:movingGiPause` (on): settled GI probes skip their update while only the camera moves.

What to look at: tumble with `/Render/moving` (the defaults), then with `/Render/full`. Moving frames are softer and much faster. Once the camera stops, the image is the same under both.

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_01_moving_camera\scene.usda`, then pick the settings prim, or flip the three settings in Eldr > Render Settings.
- Houdini: reference the scene in a LOP network, pick Eldr in the viewport and tumble. The Eldr tab has the three settings under Performance and debug.
- Measure: `viewportBench scene.usda /cam --houdini --stop 1500` (add `--set eldr:movingRenderScale=1` for the A/B).

1920x1080, GTX 1080 Ti, moving: 64 ms a frame with everything kept, 12.5 ms with the defaults.
