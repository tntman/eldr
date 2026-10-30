# Cheaper traced refraction and volume local-light shadows

Two speed-ups. Each one should look the same with it on and off.

`scene.usda` adds three sphere lights (warm, cool and white) to `tests/scenes/volume.usda`.
Volumes now take a local light's shadow (its atlas tap, plus the other volumes on the way) every second
march step, `eldr:volumeLocalShadowStride` 2. The steps in between scale the unshadowed light by the
last tap's shadowed fraction. The lanes of a subgroup tap together, so the skipped taps save time.
- `/Render/stride2`: the default. At 1080p with the final-render settings on, volumes take 237 ms per sample.
- `/Render/stride1`: a tap every step, as before. Volumes take 258 ms per sample. Against
  `/Render/stride2`: PSNR 63 dB, at most 5/255.
- `/Render/stride4`: volumes take about 209 ms per sample. Against `/Render/stride1`: PSNR 57 dB.

`glass.usda` is `tests/scenes/glass.usda` with `eldr:rtRefraction` on in both prims. With
`eldr:rtRefractionClip`, a pixel's first ray looks for glass only, up to the opaque depth.
- `/Render/clip` (in `glass.usda`, not `scene.usda`): the default. rayTracing takes 37.5 ms per sample.
- `/Render/noClip` (also in `glass.usda`): 41.7 ms per sample. The image is identical.

Open it:
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_20_rt_volume_cost\scene.usda`
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam -rs /Render/stride2 --disableCameraLight scene.usda out.png`
- husk: `husk -R HdVkRendererPlugin -s /Render/stride2 scene.usda -o out.exr`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
