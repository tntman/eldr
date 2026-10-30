# Volume motion blur

<!-- summary -->
Volumes blur along their `vel` field (a vector grid or Houdini's vel.x/.y/.z), as Karma's Lookup Offset.

- **Compare:** open `showcase/2026-10-29_18_volume_motion_blur/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `blur` vs `sharp` (`eldr:volumeVelocityBlur` off).
- **Look at:** The two smoke balls and their shadows smear along x; the right one twice as far (blur scale 2).
<!-- summary -->

Two balls of smoke move a unit a frame along x (`tests/scenes/volume_moving.vdb`, written by
`make_volume --moving`). The left one's velocity is a vector VDB grid `vel`; the right one's is
Houdini's three float volumes `vel.x`, `vel.y`, `vel.z` (bound as `vel_x`, `vel_y`, `vel_z`). Both have
Velocity Blur on (`primvars:karma:object:vblur` 1), and the camera's shutter is open half a frame, so in
a film render (a still image over film samples) both smear along x: the left by half a unit, the right by a whole unit (Volume Velocity
Blur Scale 2, `primvars:karma:object:volumevblurscale`). Their shadows on the ground smear too.

This is Karma's Lookup Offset mode: each sample reads the fields where the smoke was `vel * t` earlier.

Render settings prims:
- `/Render/blur`: nothing set, so volume velocity blur is on (the default); `eldr_default` is the same.
- `/Render/sharp`: `eldr:volumeVelocityBlur` off.

Try also: `primvars:karma:object:vblur` 0 on a volume (sharp), or `eldr:motionBlurTimeSamples` off
(the blur needs the film's time samples, so interactive frames are sharp).

Open it:
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_18_volume_motion_blur\scene.usda`, then
  leave it still and wait: the blur builds up over the film's samples
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam -rs /Render/blur scene.usda out.png`
- husk: `husk -R HdVkRendererPlugin -s /Render/blur scene.usda -o out.exr` (Karma: `-R BRAY_HdKarma`)

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_18_volume_motion_blur\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_18_volume_motion_blur/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
