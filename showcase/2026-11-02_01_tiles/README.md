# Tiled rendering

<!-- summary -->
husk and usdrecord render very large images tile by tile, so GPU memory follows the tile size, not the image.
<!-- summary -->

A Cornell box with a glowing panel and a warm bulb (bloom), depth of field focused on the chrome ball and ambient occlusion in the corners. Any of them would show a seam between tiles. Both A/B nodes turn on the reflection probe (`eldr:reflectionProbe`): screen-space reflections see only a tile and its border, and the probe fills in what lies farther away.

- **tiles**: 512-pixel tiles (12 at 1920x1080), rendered one after another. Look for seams along the tile edges (x = 512, 1024, 1536; y = 512 from the bottom): there are none.
- **whole**: the same settings, the whole image at once. The two images differ only by a little noise, except in the chrome ball: there the whole image's screen-space reflections find the panel and walls on screen, while a tile reflects the probe.
- **eldr_default / eldr_final / karma**: Eldr with nothing set, Eldr's final settings, Karma to compare.

Tiles apply to husk and usdrecord only; the viewport renders the whole image. Use **Tile Size** for images too large for the GPU (8K and up): 2048 is a good start.

## Open

Houdini: `scene.hip`. husk: `husk -R Eldr -s /Render/tiles showcase/2026-11-02_01_tiles/scene.usda`. usdrecord: `usdenv usdrecord --disableCameraLight --renderer Eldr --renderSettingsPrimPath /Render/tiles showcase\2026-11-02_01_tiles\scene.usda tiles.png`.

## Houdini scene

Open `scene.hip`. A Sublayer LOP loads `geo.usda`, the scene's geometry only. Everything else is a Houdini node you can change: the camera (Camera LOP), each light (Light, Distant Light, Dome Light LOPs), the materials (a Material Library with MaterialX, USD Preview Surface and Karma VOPs), render products and AOVs (Render Product, Render Var LOPs), and one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

A value a node has no parameter for sits on the node as an extra parameter, or on an Edit Properties LOP right after it. A material Houdini's VOPs can't rebuild exactly comes from `scene.usda` through a Reference LOP.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). `scene.usda` holds the same scene for usdview and husk (it sublayers `geo.usda`): `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda` (`--check` compares the hip's stage with `scene.usda`).
