# Volume local-light shadow stride

<!-- summary -->
Volumes take a local light's shadow every second march step instead of every step, for the same image (`eldr:volumeLocalShadowStride`).

- **Compare:** open `showcase/2026-10-29_20_volume_light_shadow_stride/scene.hip` (or `scene.usda`) and compare `eldr_default` vs `eldr_final` vs `karma`; the feature's A/B nodes: `stride2` (default) vs `stride1` vs `stride4`.
- **Look at:** The three coloured lights in the smoke: the same picture, less time (render stats).
<!-- summary -->

`tests/scenes/volume.usda` plus three sphere lights (warm, cool and white). Between taps, the unshadowed light is scaled by the last tap's shadowed fraction.

- `/Render/stride2`: the default. At 1080p with the final-render settings, volumes take 237 ms per sample.
- `/Render/stride1`: a tap every step: 258 ms per sample. Against `stride2`: PSNR 63 dB, at most 5/255.
- `/Render/stride4`: about 209 ms per sample. Against `stride1`: PSNR 57 dB.

## Open

- Houdini: open `scene.hip` (see below)
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_20_volume_light_shadow_stride\scene.usda`, then pick a settings prim
- husk: `husk -R Eldr -s /Render/eldr_final showcase/2026-10-29_20_volume_light_shadow_stride/scene.usda`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
