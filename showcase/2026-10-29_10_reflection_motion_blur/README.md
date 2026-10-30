# Motion blur in reflections

<!-- summary -->
Planar and traced reflections blur with motion blur, rendered at each film sample's shutter time.

- **Compare:** open `showcase/2026-10-29_10_reflection_motion_blur/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `planar` vs `frozen` (`eldr:planarMotionBlur` off), `traced` (`eldr:rtReflections`).
- **Look at:** The racing cube's reflection in the floor: blurred like the cube, sharp when frozen. Render frame 2.
<!-- summary -->

An orange cube races across the mirror floor in front of the chrome ball (3 units a frame, shutter open half a frame). Render frame 2.

- `/Render/planar`: the floor's planar reflection blurs as the cube does (`eldr:planarMotionBlur` on: each film sample renders the mirror at its shutter time).
- `/Render/frozen`: `eldr:planarMotionBlur` off: a sharp reflection under a blurred cube.
- `/Render/traced`: `eldr:rtReflections` on: floor and ball reflect from each sample's TLAS, so both blur.
- The chrome ball under `planar`/`frozen` is probe-lit (`eldr:reflectionProbe`): probes are captured at the frame's time, so the cube in the ball stays sharp. A capture is six renders and a CPU prefilter, too slow per film sample; use traced reflections for blurred reflections on curved surfaces.
- Open: `usdenv usdview --renderer Eldr showcase\2026-10-29_10_reflection_motion_blur\scene.usda` (set the frame to 2), or `husk -R Eldr -s /Render/planar -f 2 scene.usda`.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_10_reflection_motion_blur\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_10_reflection_motion_blur/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
