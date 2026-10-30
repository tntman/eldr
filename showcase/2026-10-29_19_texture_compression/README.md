# Texture compression

`eldr:textureCompression` stores 8-bit textures block-compressed on the GPU: colour as
BC7 (1 byte per texel instead of 4), gray as BC4 (half a byte instead of 1), gray + alpha as BC5. The
mips are made before compressing (sRGB filtered in linear). Off (the default) loads exact textures, as Karma never compresses; on compresses them. Each file is encoded once and kept in
`%LOCALAPPDATA%\Eldr\textures`; later loads read it from there.

Two quads show their textures as emission, one screen pixel per texel, so differences are not hidden by
lighting. Left: `detail.png` (sRGB colour, BC7) with gradients, hard-edged discs, a checker and one-texel
lines. Right: `rings.png` (raw gray, BC4). Look at the disc edges and the thin lines: BC7's mode 6 puts one
colour line through each 4x4 block, so a block where three colours meet comes out slightly smeared.
`make_textures.py` writes the two images. Measured (usdrecord, compressed vs exact): 37 dB PSNR here, the worst at the one-texel lines; crowd_9k 52 dB with its textures 172 to 112 MB.

Render settings prims:
- `/Render/compressed`: `eldr:textureCompression` on.
- `/Render/exact`: `eldr:textureCompression` off, the exact 8-bit texels.

Open it:
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_19_texture_compression\scene.usda`, then set
  Texture Compression off or on in the Hydra settings (materials reload their textures).
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam -rs /Render/exact scene.usda exact.png`
- husk: `husk -R HdVkRendererPlugin -s /Render/compressed scene.usda -o out.exr`

## Houdini scene

Open `scene.hip`. Its LOP network sublayers `scene.usda` and has one render settings node per settings prim:

- `eldr_default`: Eldr with nothing set (its default settings).
- `eldr_final`: Eldr with each setting a final render needs ticked one by one: traced reflections, refraction and hit shadows, final gather, caustics, GI, traced shadows (16 lights, 1 ray each), denoise, film restart, 256 film samples.
- `karma`: a Karma Render Settings node to match (same camera, resolution and samples).
- The other nodes are this scene's A/B settings above.

Pick a node and render its USD Render ROP (`render_<node>`; images go to `render/`). Eldr's ROPs run `$ELDR_HUSK`, Karma's plain husk. Nothing switches automatically. For the viewport, choose Eldr in the viewport's renderer menu (a script can't set it). The same prims are in `scene.usda` for usdview and husk: `husk -R Eldr -s /Render/eldr_final scene.usda`.

The file is built by `hython houdini/make_showcase_hip.py`; rerun it after changing `scene.usda`.
