# Tonemapping

<!-- summary -->
The display tonemapper: ACES, AgX or none (`eldr:tonemapper`).

- **Compare:** open `showcase/2026-10-30_17_tonemapping/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `aces` (default) vs `agx` vs `linear`.
- **Look at:** The bulb: clipping towards yellow with ACES, desaturating towards white with AgX, hard-clipped with none.
<!-- summary -->

The Cornell box lit by a glowing panel (40) and a warm bulb (60, 30, 8) seen directly, so the image has values far over 1. Linear Beauty (`eldr:productLinear`, on by default) makes husk's and Houdini's beauty linear with no tonemap, so every prim here turns it off; usdview and usdrecord tonemap either way. `eldr:colorLimit` 1000 keeps the emitters' full HDR. `eldr_default` and `karma` write linear EXRs: view them with a display transform.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_17_tonemapping\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-30_17_tonemapping/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
