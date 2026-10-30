# Light path intensity

The Cornell box under GI, where the ceiling and the colour on the white walls are bounced light. `eldr:directIntensity` and `eldr:indirectIntensity` scale direct or indirect light in the final pixel only, like a comp grade on the light splits: the light transport and the AOVs are unchanged, the beauty is the splits times the intensities. Fog and toon shading stay unscaled.

## What to look at

- `/Render/neutral`: both 1, GI on.
- `/Render/indirectBoost` (`eldr:indirectIntensity` 3): bright ceiling, strong red and green bleeding, filled shadows.
- `/Render/directDim` (`eldr:directIntensity` 0.3): the light's pool on the floor and the direct highlight dim; the bounce stays.
- Karma has no such control; its `karma` node is the neutral look.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_16_light_path_intensity\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/neutral scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
