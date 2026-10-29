# Volume motion blur along vel

Two balls of smoke move a unit a frame along x (`tests/scenes/volume_moving.vdb`, written by
`make_volume --moving`). The left one's velocity is a vector VDB grid `vel`; the right one's is
Houdini's three float volumes `vel.x`, `vel.y`, `vel.z` (bound as `vel_x`, `vel_y`, `vel_z`). Both have
Velocity Blur on (`primvars:karma:object:vblur` 1), and the camera's shutter is open half a frame, so in
a final render both smear along x: the left by half a unit, the right by a whole unit (Volume Velocity
Blur Scale 2, `primvars:karma:object:volumevblurscale`). Their shadows on the ground smear too.

This is Karma's Lookup Offset mode: each sample reads the fields where the smoke was `vel * t` earlier.

Render settings prims:
- `/Render/blur`: the default.
- `/Render/sharp`: `eldr:volumeVelocityBlur` off.

Try also: `primvars:karma:object:vblur` 0 on a volume (sharp), or `eldr:motionBlurTimeSamples` off
(the blur needs the film's time samples, so interactive frames are sharp).

Open it:
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_09_volume_motion_blur\scene.usda`, then
  Eldr's final quality (the blur builds up over the film's samples)
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam -rs /Render/blur scene.usda out.png`
- husk: `husk -R HdVkRendererPlugin -s /Render/blur scene.usda -o out.exr` (Karma: `-R BRAY_HdKarma`)
