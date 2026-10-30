# Cheaper traced refraction and volume local-light shadows

Two speed-ups. Each one should look the same with it on and off.

`scene.usda` adds three sphere lights (warm, cool and white) to `tests/scenes/volume.usda`.
Volumes now take a local light's shadow (its atlas tap, plus the other volumes on the way) every second
march step, `eldr:volumeLocalShadowStride` 2. The steps in between scale the unshadowed light by the
last tap's shadowed fraction. The lanes of a subgroup tap together, so the skipped taps save time.
- `/Render/stride2`: the default. At 1080p in final quality, volumes take 237 ms per sample.
- `/Render/stride1`: a tap every step, as before. Volumes take 258 ms per sample. Against
  `/Render/stride2`: PSNR 63 dB, at most 5/255.
- `/Render/stride4`: volumes take about 209 ms per sample. Against `/Render/stride1`: PSNR 57 dB.

`glass.usda` is `tests/scenes/glass.usda` with traced refraction always on. With
`eldr:rtRefractionClip`, a pixel's first ray looks for glass only, up to the opaque depth.
- `/Render/clip`: the default. rayTracing takes 37.5 ms per sample.
- `/Render/noClip`: 41.7 ms per sample. The image is identical.

Open it:
- usdview: `usdenv usdview --renderer Eldr showcase\2026-10-29_20_rt_volume_cost\scene.usda`
- usdrecord: `usdenv usdrecord --renderer Eldr --camera /cam -rs /Render/stride2 --disableCameraLight scene.usda out.png`
- husk: `husk -R HdVkRendererPlugin -s /Render/stride2 scene.usda -o out.exr`
