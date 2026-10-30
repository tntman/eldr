# Textures, UDIMs and colour spaces

Middle: `tests/scenes/colorspace.usda`: each column pairs a colour authored in ACEScg (top, tagged `lin_ap1_scene`) with the same colour pre-converted to linear Rec.709 (bottom); Eldr shades in linear Rec.709, so each pair matches. Left column constants, right column 16-bit textures. Right: a MaterialX image of `udim.<UDIM>.png` on uvs 0..2: tiles 1001 (bottom left), 1002 (bottom right), 1011 (top left); 1012 is missing and shows the image's `default` (black, lit only by the specular), as Karma. Bottom: a floor tiling `quadrants.png` (sRGB) 40 times towards the horizon.

## What to look at

- `/Render/textures` (the default, `eldr:anisotropy` 16): the tiles stay crisp far down the floor; each colour pair matches.
- `/Render/aniso1` (`eldr:anisotropy` 1, trilinear only): the far floor blurs into a smear.
- Texture compression has its own showcase (2026-10-29_19).

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_21_textures_colour\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/textures scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
