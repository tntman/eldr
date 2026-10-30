# Height fog

<!-- summary -->
Height fog with light shafts from the sun and spot lights (`eldr:fog`).

- **Compare:** open `showcase/2026-10-30_12_fog/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `fog` vs `fogOff`, `fogDense`.
- **Look at:** The shafts between the pillars, the spot's cone and the hazy far wall.
<!-- summary -->

`tests/scenes/fog.usda`: a low sun behind a colonnade shining towards the camera, a blue spot on the right. Fog is settings only (no USD fog schema): height fog in a 160x90x64 froxel grid, lit by the sun's cascades (the pillars throw shafts), local lights (the spot's cone) and the dome, Henyey-Greenstein phase (`eldr:fogAnisotropy` 0.6).

## What to look at

- `/Render/fog`: density 0.03, height falloff 0.15: shafts between the pillars, the spot's cone, the far wall hazy.
- `/Render/fogOff` (`eldr:fog` off): clear air. `eldr_default` and `eldr_final` are fog-free too (fog is off by default).
- `/Render/fogDense`: density 0.08, warm `eldr:fogColor`, no falloff: thick haze.
- Karma has no fog setting: its `karma` node renders clear air; a Karma comparison needs a volume box.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_12_fog\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/fog scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
