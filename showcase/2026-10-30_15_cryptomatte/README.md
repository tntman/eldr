# Cryptomatte

The Cornell box with a render product writing the beauty and two Cryptomatte layers: by object (`primvars:__name`, the rprim path) and by material (`primvars:__materialname`), as Karma's Cryptomatte LOP names them. The EXR has `CryptoObject00..02` and `CryptoMaterial00..02` (ranks 0-5, id and coverage pairs) and the spec's metadata (name, MurmurHash3_32, uint32_to_float32, manifest), so Nuke's Cryptomatte gizmo or Houdini's COP Cryptomatte picks mattes.

## What to look at

- `/Render/crypto` (the default): soft Cryptomatte (film only): up to 6 ids per pixel with their filtered coverage; the mattes of an edge sum to 1.
- `/Render/cryptoHard` (`eldr:cryptomatteSoft` off): one id per pixel, coverage 1: aliased mattes.
- The beauty is the same in both: open `cryptomatte.exr` (written next to where you run husk or usdrecord) in a Cryptomatte viewer and pick the ball's edge.

## Open

- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-30_15_cryptomatte\scene.usda`, then pick a settings prim (right-click, set as active), or flip the settings in Eldr > Render Settings.
- husk: `husk -R Eldr -s /Render/crypto scene.usda` (or `/Render/eldr_final`, `/Render/karma` with `-R BRAY_HdKarma`).

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
