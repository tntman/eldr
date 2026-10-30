# Velocity blur

`tests/scenes/motion_blur.usda` at frame 24 (the layer's time range is pinned to it) with Velocity Blur turned on per object. Without it, objects that only author `velocities` do not blur (Karma's default).

- `mesh_velocitiesOnly`: `primvars:eldr:object:vblur = 1`; `mesh_velocitiesAccelerations`: 2 (adds accelerations).
- `pointsVelocities`, `inst_angularVelocities`: only `karma:` names (vblur, instance_vblur); Eldr reads them too.
- `curvesVelocities`: karma vblur 1 but eldr vblur 0. Eldr wins, so it stays sharp.
- `inst_velocities`: `primvars:eldr:object:instance_vblur = 1`.
- `skinned`: skel.usda's agent, a tube bent by joints. Sampled motion takes its joints at the shutter's open and close, so it blurs in a single frame (husk, usdrecord) like the rest.

## What to look at

- `/Render/blurOn` vs `/Render/blurOff`: `eldr:motionBlur` now toggles without changing the frame. Time-sampled objects blur from their samples; velocity-only ones from vblur.
- `/Render/blurFinal`: the final-render settings (traced reflections and refraction, final gather, caustics), with `eldr:motionBlurTimeSamples` on: each film sample renders at its own shutter time, so the blur converges over film samples.
- `/Render/cameraBlur`: the panning camera.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-28_02_velocity_blur\scene.usda` (pick a settings prim in the stage tree: right-click, set as active)
- Houdini: Sublayer or Reference LOP on the file, Eldr as the viewport renderer; a Render Settings LOP can pick `/Render/<name>`
- husk: `husk -R Eldr -s /Render/<name> showcase/2026-10-28_02_velocity_blur/scene.usda`

Settings prims: `/Render/blurOn`, `/Render/blurOff`, `/Render/blurFinal`, `/Render/cameraBlur`; standard: `eldr_default`, `eldr_final`, `karma`.

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
