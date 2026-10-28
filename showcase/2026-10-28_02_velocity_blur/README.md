# Velocity blur

`tests/scenes/motion_blur.usda` at frame 24 (the layer's time range is pinned to it) with Velocity Blur turned on per object. Without it, objects that only author `velocities` do not blur (Karma's default).

- `mesh_velocitiesOnly`: `primvars:eldr:object:vblur = 1`; `mesh_velocitiesAccelerations`: 2 (adds accelerations).
- `pointsVelocities`, `inst_angularVelocities`: only `karma:` names (vblur, instance_vblur); Eldr reads them too.
- `curvesVelocities`: karma vblur 1 but eldr vblur 0. Eldr wins, so it stays sharp.
- `inst_velocities`: `primvars:eldr:object:instance_vblur = 1`.

## What to look at

- `/Render/blurOn` vs `/Render/blurOff`: `eldr:motionBlur` now toggles without changing the frame. Time-sampled objects blur from their samples; velocity-only ones from vblur.
- `/Render/blurFinal`: final quality with time samples. usdrecord already renders final quality, so it looks like `blurOn` there; in usdview it converges over film samples.
- `/Render/cameraBlur`: the panning camera.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_02_velocity_blur\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-28_02_velocity_blur/scene.usda`

Settings prims: `/Render/blurOn`, `/Render/blurOff`, `/Render/blurFinal`, `/Render/cameraBlur`.
