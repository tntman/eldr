# Texture compression

`eldr:textureCompression` (on by default) stores 8-bit textures block-compressed on the GPU: colour as
BC7 (1 byte per texel instead of 4), gray as BC4 (half a byte instead of 1), gray + alpha as BC5. The
mips are made before compressing (sRGB filtered in linear). Each file is encoded once and kept in
`%LOCALAPPDATA%\Eldr\textures`; later loads read it from there.

Two quads show their textures as emission, one screen pixel per texel, so differences are not hidden by
lighting. Left: `detail.png` (sRGB colour, BC7) with gradients, hard-edged discs, a checker and one-texel
lines. Right: `rings.png` (raw gray, BC4). Look at the disc edges and the thin lines: BC7's mode 6 puts one
colour line through each 4x4 block, so a block where three colours meet comes out slightly smeared.
`make_textures.py` writes the two images. Measured (usdrecord, compressed vs exact): 37 dB PSNR here, the worst at the one-texel lines; crowd_9k 52 dB with its textures 172 to 112 MB.

Render settings prims:
- `/Render/compressed`: the default.
- `/Render/exact`: `eldr:textureCompression` off, the exact 8-bit texels (look-dev).

Open it:
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_19_texture_compression\scene.usda`, then flip
  Texture Compression in the Hydra settings (materials reload their textures).
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam -rs /Render/exact scene.usda exact.png`
- husk: `husk -R HdVkRendererPlugin -s /Render/compressed scene.usda -o out.exr`
