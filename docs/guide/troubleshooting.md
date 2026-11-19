# Troubleshooting

Find the symptom, check the cause, apply the fix. Install problems (Eldr missing from the menu, no Eldr tab, husk licence) are in [Getting started](getting-started.md#troubleshooting-the-install).

## The image

| Symptom | Cause | Fix |
|---|---|---|
| The image never stops changing | The film restarts on every real change (camera, scene, settings); a playing timeline or a cooking simulation keeps restarting it | Stop playback. Still flickering? Set `HDVK_DEBUG_AOVS=1` before starting Houdini: every 60 frames Eldr prints how many frames moved the camera or changed the scene |
| Too dark or too bright | The camera's exposure, ISO, shutter and f-stop scale the image, as in Karma (**Camera Exposure**, on) | Check the camera's exposure attributes; **Exposure Compensation** adds stops on top |
| The viewport looks flat or washed out | Eldr gives linear colour; Houdini's colour view (OCIO) does the display transform | Check the viewport's colour view, not an Eldr setting |
| The dome renders white | Its HDRI path didn't resolve | Check the texture path. Eldr's package adds Houdini's own texture folder, so `StinsonBeach.hdr` resolves by name |
| A light's shape is missing from the camera | Light shapes are hidden by default, as in Karma | Turn on **Render Light Geometry** on the light (Karma or Eldr tab) |
| usdrecord renders are brighter than husk's | usdrecord adds a camera headlight | Pass `--disableCameraLight` |
| Eldr and Karma differ | Some features are approximated | See [Eldr and Karma](karma.md#known-differences) |

## Noise

| Symptom | Cause | Fix |
|---|---|---|
| Grain everywhere | Too few film samples | Raise **Film Samples** (256 for finals), or turn on **Denoise** (OIDN): 16 denoised samples beat 64 raw ones |
| Grain only in bounce light, AO or SSGI | Few gather rays | Raise **Diffuse Quality** instead of all samples ([quality multipliers](sampling.md#quality-multipliers)) |
| Grain only in ray-traced shadows or reflections | Few rays for that effect | Raise **Light Quality** or **Reflection Quality** |
| Single bright pixels (fireflies) | Small, very bright light paths | Lower **Indirect Light Clamp**; **Direct Light Clamp** limits direct light |
| Shadows slightly blurry in a short render | The effect filters are still fading out | Render more samples than **Effect Denoise Fade** (64) |
| Hair or fine text smeared | OIDN smooths very fine detail | Compare with the `eldr:beautyRaw` AOV; use more samples without Denoise on such shots |

## Effects and materials

| Symptom | Cause | Fix |
|---|---|---|
| A ray-traced setting does nothing | Settings marked [RT] need a GPU with Vulkan ray tracing; without it they are skipped | Check the GPU and driver. Also: **Final Gather** needs **Global Illumination**, **Caustics** needs caster objects |
| A material renders grey or wrong | A node Eldr doesn't support is bypassed (its pass-through input or default) | Open the render stats: an orange **material fallback** line counts the bypassed nodes. The [MaterialX coverage](../materialx.md) page lists what is supported |
| A BSDF-graph surface renders grey | The graph has no lobe Eldr maps (for example only a hair BSDF), or mixes surface shaders | Build the surface on standard_surface or OpenPBR |
| A Pyro Shader volume renders plain | Karma's Pyro Shader isn't read | Build a MaterialX volume material ([Volumes](volumes.md#with-a-material)) |

## Volumes

| Symptom | Cause | Fix |
|---|---|---|
| Volumes are missing | Eldr is built against a USD without OpenVDB | Use a build with OpenVDB (the default for both builds) |
| Volumes render in Houdini but not in husk | SOP Import gives `op:` paths, which only Houdini resolves | Write the stage with a USD ROP first, then render that file |
| One volume is missing | A field didn't load; Houdini's console doesn't print USD's warnings | Check the field's file path and grid name |

## Geometry, speed and memory

| Symptom | Cause | Fix |
|---|---|---|
| The first frame takes long | Eldr compiles its GPU programs on the first frame with a new feature set (seconds to tens of seconds when cold) | Wait once: they are cached on disk (`%LOCALAPPDATA%\Eldr`) and the next start is fast. A build with new shaders starts a fresh cache |
| husk renders crowds slowly or runs out of memory | Each agent becomes its own mesh | Use `$ELDR_HUSK` as the USD Render ROP's **Render Command** (deferred skinning): see [Crowds and performance](performance.md#crowds-deferred-skinning) |
| A mesh renders faceted, not smooth | Its `subdivisionScheme` is `none`: SOP Import authors that for polygons | Turn on SOP Import's **Treat Polygons as Subdivision Surfaces**, or **Render as Subdivision** on Eldr's Render Geometry Settings tab |
| Still frames take long to settle | The final settings (ray tracing, 256 samples) are on in the viewport | Switch the viewport back to the draft render settings prim while you work |

## Where to look next

- [Settings reference](../settings.md): every setting with its default and cost.
- [How Eldr works](../features.md): how each feature works, and the **Known limitations** table.
- [Environment variables](../environment.md): debug output and timing.
- [Houdini](../houdini.md): build, install, and what is tested in Houdini.
