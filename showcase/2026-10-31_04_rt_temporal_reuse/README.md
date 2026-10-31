# Ray-traced temporal reuse

<!-- summary -->
While the camera moves, traced shadows and reflections reuse earlier frames' rays at the same surface point, so moving frames are cleaner with one ray per light (`eldr:rtTemporalReuse`).

- **Compare:** open `showcase/2026-10-31_04_rt_temporal_reuse/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `rtTemporalReuse` vs `rtTemporalReuseOff`.
- **Look at:** Tumble the camera: soft shadow edges and rough reflections crawl less with `rtTemporalReuse`. Once still, both are the same.
<!-- summary -->

`tests/scenes/emissive.usda` with traced shadows and reflections on. While the camera moves, each frame traces one shadow ray per light (`eldr:movingShadowRays`) and one reflection ray per pixel.

## What to look at

- `/Render/rtTemporalReuse` (the default): each pixel's traced shadow and reflection blend with up to 16 earlier frames (`eldr:rtTemporalFrames`) where the same surface was seen. Penumbrae and rough reflections stay calm while tumbling. Where a surface comes into view from behind another, it starts from this frame's rays, so a thin noisy edge shows for a few frames.
- `/Render/rtTemporalReuseOff`: each moving frame's rays stand alone; only TAA averages them, and it clamps the noise to the pixel's neighbours, so penumbrae crawl.
- Stop the camera: the film takes over and both prims give the same image.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-31_04_rt_temporal_reuse\scene.usda`, then pick a settings prim (right-click, set as active), or flip **Ray-Traced Temporal Reuse** in Eldr > Render Settings (Viewport group).
- Houdini: reference the scene in a LOP network, pick Eldr in the viewport and tumble.
- Measure: `viewportBench scene.usda /cam --houdini --stop 1500 --set eldr:rtShadows=1 --set eldr:rtReflections=1` (add `--set eldr:rtTemporalReuse=0` for the A/B).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
