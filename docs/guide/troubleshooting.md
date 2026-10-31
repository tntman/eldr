# Troubleshooting

Common problems and what to check first.

## Eldr isn't in the renderer menu

- Check that `eldr.json` is in `Documents/houdini22.0/packages/` (or that `HOUDINI_PACKAGE_DIR` points at its folder) and that its paths point at your build.
- Eldr must be built against Houdini's own USD (the Houdini build, `build-h22`). A plugin built for USD 26.08 doesn't load in Houdini.
- husk: `husk --list-renderers` should show `HdVkRendererPlugin (Eldr)`.

## The Render Settings LOP has no Eldr tab

The node was made before Eldr was installed. Right-click it and choose **Rebuild Renderer-Specific Parameters**.

## The image never stops changing

The film restarts on every real change: camera, scene, settings. If something animates (a playing timeline, a simulation cooking), the film keeps restarting. Stop playback. If the image still flickers, set `HDVK_DEBUG_AOVS=1` before starting Houdini: every 60 frames Eldr prints how many frames moved the camera or changed the scene.

## The render is noisy

1. Raise **Film Samples** (256 for finals).
2. Turn on **Denoise** (OIDN).
3. Noise only in one effect (bounce light, traced shadows)? Raise that effect's **Quality** instead of all samples (see [Sampling and denoising](sampling.md#quality-multipliers)).
4. Single bright pixels: lower **Indirect Light Clamp**.

## The render is too dark or too bright

- Look at the **Camera Exposure**: the camera's exposure, ISO, shutter and f-stop scale the image, as in Karma.
- **Exposure Compensation** adds stops.
- In the viewport, check Houdini's colour view: Eldr gives linear colour.
- The dome renders white? Its HDRI path didn't resolve: check the texture path (Eldr's package adds Houdini's own texture folder for `StinsonBeach.hdr`).

## A ray-traced setting does nothing

Settings marked [RT] need a GPU with Vulkan ray tracing. Without it they are skipped. Also: **Final Gather** needs **Global Illumination**; **Caustics** needs caster objects.

## Materials render grey or wrong

- Open the render stats: an orange **material fallback** line names nodes Eldr bypassed.
- A surface built from pbrlib BSDF nodes renders grey (not supported).
- Karma's Pyro Shader isn't read: build a MaterialX volume material instead.

## Volumes are missing

- Eldr must be built against a USD with OpenVDB (the default for both builds).
- With SOP Import in Houdini, volumes appear as `op:` paths: they render inside Houdini only. For husk, write the stage with a USD ROP first.
- A missing field shows as a missing volume (Houdini's console doesn't print USD's warnings).

## The first frame takes long

Eldr compiles its GPU programs on the first frame with a new feature set (seconds to tens of seconds when cold) and caches them on disk. The second start is fast. A build with new shaders starts a fresh cache.

## husk renders crowds slowly or runs out of memory

Use `$ELDR_HUSK` as the USD Render ROP's Render Command (deferred skinning): see [Crowds and performance](performance.md#crowds-deferred-skinning).

## usdrecord renders are brighter than husk's

usdrecord adds a camera headlight. Pass `--disableCameraLight`.

## Where to ask for more detail

- [Settings reference](../settings.md): every setting with its default and cost.
- [How Eldr works](../features.md): how each feature works, and the **Known limitations** table.
- [Houdini](../houdini.md): build, install, and what is tested in Houdini.
