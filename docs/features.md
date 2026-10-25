# How Eldr works

Eldr (code name HdVk) is a Hydra 2.0 render delegate on Vulkan 1.3 for
OpenUSD 26.08. It is built for fast, high-quality raster renders for VFX:
seconds per frame is fine when it buys quality, output is EXR with AOVs. It is
not a game renderer. It is a rasterizer first (clustered forward shading, GPU
culling and skinning); ray tracing, where the GPU has it, adds shadows,
reflections and global illumination on top. Everything is switched by render
settings; see [settings.md](settings.md) for every key.

Contents: [Quick recipes](#quick-recipes) -
[Frame pipeline](#the-frame-pipeline) -
[Film, TAA, final quality](#film-accumulation-interactive-taa-and-final-quality) -
[Lighting and shadows](#lighting-and-shadows) -
[GI](#global-illumination) - [Reflections](#reflections) -
[Transparency](#transparency-and-refraction) -
[Volumes and fog](#volumes-and-fog) - [Materials](#materials) -
[Geometry](#geometry) - [Motion blur, DOF, upscaling](#motion-blur-depth-of-field-and-upscaling) -
[Output](#output-aovs-exr-products-cryptomatte-light-splits) -
[Performance](#performance) -
[USD authoring conventions](#usd-authoring-conventions) -
[Known limitations](#known-limitations)

## Quick recipes

Render a frame to EXR with a render settings prim (needs a USD built with
OpenImageIO; Eldr writes the files itself once the frame has converged):

```
usdenv.cmd usdrecord --renderer Eldr --camera /cam --renderSettingsPrimPath /Render/settings scene.usda out.png
```

usdenv.cmd sets the plugin paths (see the header of `usdenv.cmd`). The renderer
name for `--renderer` is `Eldr`.

Settings through Python (a script that renders with `UsdImagingGL.Engine`):

```python
engine.SetRendererPlugin("HdVkRendererPlugin")
engine.SetRendererSetting("eldr:finalQuality", True)
engine.SetRendererSetting("eldr:filmSamples", 256)
```

A sensible final-quality starting point:

| Goal | Settings |
|---|---|
| Converge from scratch, no interactive shortcuts | `eldr:finalQuality` on, `eldr:filmSamples` 128-512 |
| Linear, ungraded EXR for comp | the default: the product's beauty is linear and untonemapped (`eldr:productLinear`); `eldr:autoExposure` off and `eldr:exposureCompensation` 0 for unexposed values |
| True soft shadows, bokeh, motion blur | leave `shadowJitter`, `dofJitter`, `motionBlurTimeSamples` on (defaults) |
| Indirect light | `eldr:gi` and `eldr:ssgi` (and `eldr:ssgiBitmask` for thin occluders) |
| Accurate shadows and reflections off screen | `eldr:rtShadows`, `eldr:rtReflections` with 1 ray (the film averages) |
| Clean geometry | `eldr:lodBias` -1 or `eldr:lod` off, `--complexity veryhigh` (or `eldr:maxRefineLevel`) |
| Fireflies | `eldr:directClamp` / `eldr:indirectClamp` |

Environment variables: `HDVK_NO_RAY_TRACING=1` pretends the GPU has no ray
tracing; `HDVK_PIPELINE_CACHE_DIR` moves the shader cache folder
(default `%LOCALAPPDATA%\Eldr`); `usdenv.cmd` honours `USD_ROOT`, `USD_VENV`
and `HDVK_BUILD`.

## The frame pipeline

One frame, in the order the GPU passes run. The names are the timer names in
usdview's **Eldr > Render Statistics** window and in
`GetRenderStats()["gpuPassMs"]`. Passes marked (opt) run only when their
feature is on or something needs them.

| # | Pass | What it does |
|---|---|---|
| 0 | `probeCapture`, `planarReflection` (opt) | Second renderers draw the reflection probe cube and the mirrored views of planar mirrors. Skinned meshes keep their last joints in these. |
| 1 | skinning (not timed) | `skin.comp` builds joint matrices on the GPU from the skeletons' transforms. |
| 2 | `shutter` (opt) | With motion blur time samples: moves instances, joints, blend shape weights and the camera to the sample's shutter time. |
| 3 | `culling` | GPU frustum and size culling per instance, level-of-detail choice; fills indirect draw commands. |
| 4 | `lightClusters` | Assigns local lights to 16x9x24 view clusters. |
| 5 | `sunShadows`, `sunShadowMin` | Four-cascade sun shadow maps (and a min-depth reduction that bounds the soft-shadow search). Translucent and volume tint maps are drawn here too. |
| 6 | `depthPrepass` | Depth only (plus cutout alpha tests). Later passes read it. |
| 7 | `accelerationStructures`, `rayTracing` (opt) | BLAS/TLAS updates; one ray generation pass finds each pixel's surface and traces shadow and reflection rays, and GI probe updates. |
| 8 | `ssgi` or `ambientOcclusion` | Screen-space GI (replacing AO) or GTAO. |
| 9 | `localShadows` | Shadow atlas for sphere, rect, disk, spot and cylinder lights. |
| 10 | `fog` (opt) | Froxel fog: inject lighting, integrate front to back. |
| 11 | `shading` | Sky, then opaque meshes: PBR with the sun, local lights, dome, probes, GI, SSR, fog. Writes colour, ids, motion vectors and (if asked) the light splits. |
| 12 | `ssrDenoise` (opt) | Glossy SSR spatial, temporal and bilateral filter. |
| 13 | `subsurface` (opt) | Screen-space subsurface blur. |
| 14 | `volumes` (opt) | OpenVDB ray march; `volumesInFront` is the part in front of glass. |
| 15 | `translucency` (opt) | Refraction layers, weighted blended OIT (or sorted blending), holdout mattes. |
| 16 | `depthOfField` (opt) | Post DOF (skipped when the film uses lens jitter). |
| 17 | `film` or `taa` | Film accumulation (plain average) or TAA resolve. |
| 18 | `motionBlur` (opt) | Post motion blur (skipped under time samples). |
| 19 | `post` | Auto exposure, bloom, tonemap. |
| 20 | `aovs` (opt) | G-buffer pass and packing for bound extra AOVs. |
| 21 | `lensDistortion` (opt) | Warps colour, depth and ids. |
| 22 | `readback` | Colour and depth go to OpenGL on the GPU, or to the CPU. |

The renderer is synchronous: one frame in flight. A typical animated
20,000-agent skinned crowd renders in about 21 ms at 1280x720 with the defaults
on a GTX 1080 Ti.

## Film accumulation, interactive TAA and final quality

Eldr has three ways to turn jittered, noisy samples into a pixel.

**Interactive TAA.** While the camera or the scene moves, the camera is jittered
(Halton 2,3), and a temporal anti-aliasing resolve reprojects history with
camera and object motion vectors (Catmull-Rom history, YCoCg variance clipping
on tonemapper-compressed colour). The stochastic effects loop over their first
`eldr:interactiveSamples` (8) noise samples so the history settles. With
`eldr:renderScale` below 1, the scene renders smaller and TAA upscales
(TAAU).

**Film accumulation.** TAA's clipping also clips legitimate noise (soft
shadows, SSGI, SSR) so a still camera would converge to a biased mean. Once the
camera and scene have been still for 3 frames, from the first frame of a fresh
history (the first frame of `usdrecord`), and always in final quality, the film
replaces TAA: a plain weighted average in fp32 of each jittered sample through
a Gaussian pixel filter (`eldr:filmFilterRadius`, 1.5 px). It runs until
`eldr:filmSamples` (64) is reached, which is when the render reports itself
converged (`IsConverged`, and `percentDone` in the statistics). Any change to
the camera, the Hydra scene or the settings starts it over. With the film,
`taaSamples` only sets the pixel jitter cycle. Frames with motion blur used to
stay TAA's; with time samples they stay on the film.

**Final quality** (`eldr:finalQuality`, or Hydra's `enableInteractive = false`,
the convention of husk and hdPrman) makes the film start from scratch after
every change, never from the TAA image. Use it for offline renders.

What the film enables (each has its own switch, all on by default and
marked [film] in the settings):

- Jittered light shapes: true soft-shadow penumbrae (`shadowJitter`), in
  final-quality runs only (a fresh film or `finalQuality`).
- Lens jitter: true depth of field with occlusion (`dofJitter`).
- Motion blur by rendering the scene at shutter times
  (`motionBlurTimeSamples`).
- Per-sample froxel fog without history (`fogFilmJitter`).
- GI preconvergence: the film waits until fresh probes have settled
  (`giPreconverge`).
- Adaptive sampling: off in interactive films (`adaptiveThreshold`, 0); in
  final-quality runs on by default (`adaptiveFinalThreshold`, 0.01), and a film
  counts as converged once `adaptivePixelFraction` (0.99) of the pixels stopped.

All stochastic effects take their noise from per-effect Sobol dimensions with
blue-noise rotation (`eldr:sampleDimensions`), so the average converges
without screen-space patterns. Measured at 960 px against a 1024-sample
reference (RMSE): sunangle 0.0034 to 0.0003, fog 0.0052 to 0.0004,
Kitchen_set 0.0068 to 0.0004. Convergence at 64 samples takes 0.2-0.5 s on
those scenes.

Ids, depth and Cryptomatte are one unfiltered sample's: under the film the
one nearest the pixel centre so far (kept while later samples land farther
out, so id edges don't move with the last sample's jitter; the jitter is
the same for every pixel), else the frame's (TAA: its jittered sample).
Extra AOVs from depth (P, camera depth) still average the samples (not
the one nearest the pixel centre).

## Lighting and shadows

**Lights** are UsdLux, with UsdLux semantics as in hdEmbree: radiance =
`color * intensity * 2^exposure`, times a blackbody colour if
`enableColorTemperature`, divided by area when `normalize`.

| Light | Support |
|---|---|
| DistantLight | The sun: `inputs:angle` gives soft penumbrae. First shadow-casting one gets the cascades. |
| SphereLight | Representative-point specular, horizon-clipped diffuse, `treatAsPoint`. |
| RectLight, DiskLight | Exact polygon form factor (rect) or illuminance (disk); emit from -Z. `inputs:texture:file` textures them. |
| CylinderLight | Side only; `treatAsLine`. |
| DomeLight | Latlong texture (`inputs:texture:file`), rotation; becomes diffuse SH9 and a GGX-prefiltered cube. |
| Shaping | ShapingAPI cone angle, softness, focus and focus tint. |

Local lights go into a clustered forward+ grid: a cluster holds at most 63
lights and each light is cut off where it would light a white surface at 1% of
middle gray. Emissive meshes (constant `emissiveColor` above
`emissiveLightThreshold`, small enough) are additionally promoted to lights
(rects for flat panels, spheres otherwise; at most 32) and cast shadows.
Light shapes are not drawn to the camera unless `lightShapesVisible` is on
(a light with Karma's `bool inputs:karma:light:renderlightgeo = 0`, "Render Light Geometry", is never drawn;
it still lights the scene and shows in reflections of what it lights, not of
its own shape).

**Shadows.**

- Sun: four cascades fitted to the scene bounds, texel-snapped, with normal
  offset and slope bias. Soft penumbrae from the light's angle (PCSS with a
  bounded blocker search), or 3x3 PCF.
- Local lights: a 4096x4096 atlas of tiles; spots get one perspective tile,
  others six cube faces; lights nearest the camera get tiles first; the atlas
  redraws only when the scene or the shadowed tiles change.
- Translucent casters and volumes tint shadows through colour maps beside the
  shadow maps.
- Ray-traced shadows (`rtShadows`) replace maps for the sun and the three most
  important local lights, soft from the light's size, with a skinned-character
  cap (`rtSkinnedInstances`).
- Under the film, `shadowJitter` renders the maps from a point on the light so
  the average is the exact penumbra (sun disc: worst error 0.016 against PCSS
  0.065).

**Dome sun.** `domeSun` (on by default) pulls the bright texels of an HDRI out
of the image-based light and turns them into a shadowed distant light.

**Clamps.** `directClamp` / `indirectClamp` limit brightness against
fireflies. 0 is off; `indirectClamp` is 10 by default, `directClamp` 0.

Ambient: GTAO (visibility, bent AO and multi-bounce on the dome's light) or,
with SSGI, the traced visibility.

## Global illumination

Three layers, each separately switchable:

| Layer | Setting | Needs | What it gives |
|---|---|---|---|
| Dome light + AO | `imageBasedLighting`, `ambientOcclusion` | prepass | Unoccluded sky light darkened by GTAO. |
| DDGI probes | `gi` | ray tracing | A grid of probes traces rays each frame (`giProbes` across the scene, `giRaysPerProbe` rays). Irradiance and visibility are octahedral atlases blended with `giHysteresis`. Chebyshev visibility prevents light leaks; probes inside geometry drop out; where no probe sees a surface, shading falls back to the dome. Hits are shaded with direct light, emission and further bounces from the probes themselves. |
| SSGI | `ssgi` | prepass | Per-pixel near-field indirect light from the screen (GTAO's horizon scan, lighting from last frame), traced at half resolution (a different pixel of each 2 x 2 tile every frame, `ssgiTileOrder`) and denoised, accumulated over still frames. Replaces AO; the probes or dome provide the rest. |

SSGI variants: `ssgiBitmask` uses an EEVEE-style 32-sector visibility bitmask
(light passes behind thin objects, occluders `ssgiBitmaskThickness` of the
radius thick); `ssgiThickness` makes rays respect thin
and grazing surfaces. `screenBackface` (on) gives hits on the back of what
the camera sees `screenBackfaceScale` (0.25) of the front's light, in SSGI and
SSR alike (Blender 5.2's Backface option). Each pixel's slices are
normalized by what they would see unoccluded, so visibility and gathered
light add up to exactly the sky's share: in a white furnace no pixel gets
brighter than under the open sky (before: up to 2.3% at grazing angles). `emissiveLights`, `fogGiAmbient` and `probeNormalize`
use the GI. With the film, `giPreconverge` waits for the probes: their about 33
updates run in the first frame, as probe traces and updates alone (no raster or
shading in between), not as 33 frames.

Typical costs on a 1080 Ti at 1280x720: probes 2-3 ms (Kitchen), SSGI
0.67 ms at half resolution.

## Reflections

Reflections are layered; each pixel takes the best available source for its
roughness:

1. **SSR** (`ssr`): ray marching against the depth buffer, 32 steps by
   default, bisection refinement at crossings, shaded from last frame's image,
   fades with roughness (0.3 to 0.6) and at screen edges. Thickness estimator
   (`ssgiThickness`) handles thin strips and grazing surfaces. `ssrHitFilter`
   reads each hit from the mip of last frame's image that matches the ray's
   cone (the history keeps 6 mips for it), so rough reflections blur instead
   of sparkling; hits outside last frame's view are misses (probe or dome).
2. **SSR denoiser** (`ssrDenoise`, off): one GGX-visible-normal ray per pixel,
   a ratio-estimator spatial filter (9 rays), temporal reprojection through the
   reflected point and a variance-guided bilateral filter. Handles roughness
   up to 0.8 (fade 0.5 to 0.8). Costly (the crowd's shading goes from 11.7 to
   20.4 ms); not combined with ray-traced reflections.
3. **Reflection probes** (`reflectionProbe`): a cube captured in the scene,
   prefiltered like the dome, with parallax correction against a box and the
   captured distances. One automatic probe at the scene centre, or up to 8
   authored probes (meshes flagged `primvars:eldr:reflectionProbe`, smallest
   box wins, soft edges). `probeNormalize` scales a probe by the local ambient
   light (needs GI or SSGI) to stop dark corners reflecting a lit room.
4. **Planar reflections** (`planarReflections`): mirrors (flagged
   `primvars:eldr:planarReflection`, or the largest flat glossy opaque mesh)
   get the scene rendered mirrored in their plane, with an oblique near plane,
   up to `planarCount` (4) mirrors at `planarScale` of the frame size.
5. **Ray-traced reflections** (`rtReflections`): GGX-sampled rays, shaded at
   hits, up to roughness 0.7, replace the dome and SSR; rough ones are
   averaged over up to 12 pixels in the surface plane before the film.
   Displacement is seen (`rtDisplacement`), also on skinned instances (displaced
   before skinning, as the raster does, in rt_skin.comp); skinned instances only up to the
   cap.

Mirror pixels take their planar reflection instead of SSR; other surfaces
use the probe (if on) or the dome as the base and SSR where the screen has
the hit. Ray-traced reflections replace the dome's and SSR's up to roughness
0.7.

## Transparency and refraction

- **Cutouts**: UsdPreviewSurface opacity with `opacityThreshold > 0`, and
  MaterialX opacity textures at a fixed 0.5, are alpha-tested in every pass,
  shadows included.
- **Translucency**: opacity below 1 (constant, or a texture without a
  threshold) makes a material translucent. Translucent meshes are drawn after
  the sky in their own pass, sorted per mesh back to front, with weighted
  blended OIT (`oit`, distance weights to `oitDepthWeight`) by default; diffuse
  and emission scale with opacity, highlights do not.
- **Refraction** (`refraction`, needs the prepass): materials with
  transmission (standard_surface `transmission*`, OpenPBR `transmission_*`) or
  an authored UsdPreviewSurface `ior` refract a mip-chained copy of the opaque
  colour. A thickness estimate (distance to the opaque surface behind, capped
  by the mesh size, or the view ray for `thin_walled`) places the exit
  point; roughness blurs over mips and `refractionSamples` taps; Fresnel
  splits reflection and transmission; colour absorbs at the surface or with
  Beer-Lambert over `transmission_depth`.
- **From inside** (`refractionBackFaces`): back faces are drawn first with the
  inverted ior, so water from below shows Snell's window and total internal
  reflection (the critical angle is 48.8 degrees for water).
- **Layers** (`multiRefraction`): refracting meshes draw back to front and the
  colour is copied again after each, so glass behind glass shows (up to
  `refractionLayers` copies).
- **Ray-traced refraction** (`rtRefraction`, needs ray tracing and the
  prepass; on in final quality by default): refracting meshes join the TLAS
  under a mask of their own (shadow, reflection and GI rays still pass
  through them). Per pixel a ray finds the nearest refracting surface and
  follows its refracted ray through the glass, up to `rtRefractionBounces`
  interfaces: into and out of each medium, glass behind glass, total internal
  reflection, a Fresnel choice between reflecting and refracting after the
  first surface, Beer-Lambert absorption over `transmission_depth`, and a GGX
  microfacet normal per sample by the roughness (rough glass converges over
  the film). It is shaded at its hit as traced reflections are, so what is
  off screen or behind other objects shows through glass. The front surface
  keeps its own Fresnel, tint and reflections; other refracting fragments at
  the pixel stay screen space. Interfaces after the first use the materials'
  constants and textures (not programs), the first the hit's interpolated
  normal and constant ior.
- **Translucent shadows** (`translucentShadows`): glass casts a faint, green
  glass a green, shadow.
- **Holdouts** (`holdouts`): matte objects write black and zero alpha but keep
  depth, ids and shadows. Translucent holdouts (`holdoutAlpha`) cut colour and
  alpha by their opacity.
- **Subsurface** (`subsurface`): see Materials.

Screen-space limits (without ray-traced refraction): only what is on screen is refracted, and OIT is an
approximation (sorting is per mesh, not per triangle).

## Volumes and fog

**Fog** (`fog`): height fog in a 160x90x64 froxel grid over the view frustum
(depth slices grow quadratically to the scene's far end or `fogMaxDistance`).
Each froxel's medium is `density * exp(-falloff * (height - base))`, lit through
a Henyey-Greenstein phase by every distant light (through its cascades, so
pillars throw shafts), local lights (through their atlas tiles) and the dome
(from GI probes if `fogGiAmbient` and GI are on). Front-to-back integration is
energy conserving; the shading pass applies it to opaque and translucent
surfaces and the sky pass to the sky. Under the film each sample gets
per-froxel jitter with no history (`fogFilmJitter`). There is no USD fog
schema; fog is driven by settings only.

**OpenVDB volumes** (`volumes`; builds against a USD with OpenVDB, as the
default `C:\dev\usd-26.08-vdb`): `UsdVolVolume` with `OpenVDBAsset` fields.
Fields bind by name: `density`, `temperature`, `Cd` or `color`, `emission`;
`fieldIndex` is honoured. Grids become NanoVDB on the GPU and are ray-marched
by a fullscreen pass over the shaded image, empty space skipped with the
tree. Lighting: Henyey-Greenstein phase, three-octave multiple scattering of
distant lights, the sun's cascades for opaque occluders, the dome's SH
convolved with the phase, local lights as points, and emission from
temperature (blackbody) or an emission field. Volume materials come from
MaterialX (`volume` shader: absorption/scattering per density, anisotropic
vdf, uniform_edf emission, blackbody temperature emission). Settings
multiply density and emission, and `volumeAnisotropy` is for volumes without
a material.

Interactions: volumes shadow surfaces from the sun through the translucent
tint maps (`volumeShadows`) and from the local lights' atlas tiles
(`localVolumeShadows`); volumes behind glass are dimmed and refracted
(`volumeUnderTransparency`); a half-resolution march
(`volumeResolution` 2) cuts cost by about 3x.

## Materials

Eldr shades in linear Rec.709. Colour-space-tagged textures and colours
(`sourceColorSpace`, UsdColorSpaceAPI) are converted from ACEScg, ACES2065-1,
P3, Rec.2020, sRGB and gamma encodings; unknown names warn once and read as
untagged.

**Supported surfaces**

| Surface | Notes |
|---|---|
| UsdPreviewSurface | Metallic and specular workflows (`useSpecularWorkflow`), clearcoat (F0 from `ior`), `ior` refraction, opacity and cutouts, displacement, normal maps. |
| MaterialX standard_surface | base, specular (and `specular_color`), roughness, metalness, coat, sheen, thin film, anisotropy, transmission, subsurface, emission, opacity, normal maps, displacement. |
| OpenPBR Surface | The same lobes under OpenPBR names (`fuzz_*`, `coat_*`, `thin_film_*`, `geometry_opacity`, `transmission_*`, `subsurface_*`). |

Material context order: `mtlx`, then Storm's `glslfx`, then universal
(`outputs:surface`), so Houdini/Karma assets and assets made for usdview both
work.

**Lobes** (layered over the metallic base, in separate pipelines so plain
materials do not pay): clearcoat (own GGX lobe, tints the base),
sheen (Charlie microfiber), thin film (Belcour-Barla Airy model), anisotropic
GGX (tangent from the uv set's u direction, rotated by `specular_rotation`),
hair (Kajiya-Kay two-lobe on BasisCurves), toon bands, and the base's energy
compensation (multi-scattering).

**What can be textured.** Texture reads (UsdUVTexture, MaterialX `image` /
`tiledimage`, UDIM, per-texture wrap modes, uv transforms, several uv sets
through the material program) cover base colour, roughness, metallic,
emission, opacity, normal, displacement, and (with
`texturedLobes` / `texturedTransmission`) specularColor, coat, coat colour,
coat roughness, sheen, sheen colour and roughness, anisotropy and rotation,
thin film thickness, weight and IOR, ior, transmission, transmission colour,
transmission depth (Beer-Lambert per pixel), thin walled (above 0.5),
subsurface, subsurface colour, radius and its scale (standard_surface
`subsurface_scale`, OpenPBR `subsurface_radius_scale`).

**Material programs** (`materialGraphs`): graphs behind inputs that a
texture or constant cannot express are compiled to a small per-pixel
interpreter: MaterialX math, mix, overlay, clamp, remap, conditionals, HSV,
extract/combine, `image`, `tiledimage`, `place2d`, `texcoord`,
`geompropvalue`, ramps, `normalmap`, UsdUVTexture, UsdTransform2d and
primvar readers; Houdini's ramp nodes `hinvlinear` and `huniformramp`. With
`proceduralTextures` they include MaterialX noise (noise2d/3d, fractal2d/3d,
cellnoise, worleynoise, position, normal), ported from MaterialX's own
library so results agree with Storm. A graph is compiled whole or not at all;
if it cannot be, the input keeps its constant (a warning is printed once per
node type).

Geometry-side: per-vertex `displayColor`, GeomSubsets with their own
materials, and any primvar a program names, at constant, uniform, varying,
vertex, faceVarying or instance rate (instancer primvars, e.g. Houdini crowd
`random`).

**Subsurface.** The diffuse light of subsurface materials is blurred by
separable per-channel Gaussians (taps spread over three times the largest
radius, at most 128 px, depth-aware) in a second shading pass; thin parts
transmit the sun, and thin-walled materials pass it whole.

**Toon.** `inputs:eldr:toonBands` on a material, or `eldr:toonBands` globally,
quantises the lit intensity relative to the albedo into flat bands with
`inputs:eldr:toonSoftness` edges; emission is untouched.

## Geometry

- **Meshes** with implicit surfaces (sphere, cube, cone, cylinder, capsule,
  plane) converted to meshes, GeomSubsets, instancing (PointInstancer, native
  instances; one indirect draw per prototype), render tags/purposes.
- **Subdivision** (`subdivision`, `maxRefineLevel`): catmullClark, loop and
  bilinear refined with OpenSubdiv to the display style's refine level.
  Creases, corners, holes, boundary and face-varying interpolation are
  honoured; limit positions and normals; face-varying uv seams stay seams.
  Skinned meshes refine in the rest pose and skin on the GPU.
- **Displacement** (`displacement`): UsdPreviewSurface `displacement` and
  MaterialX displacement shaders move vertices along their normals in the
  vertex stage, so depth, shadows, motion vectors and ray tracing all see the
  same surface; the shading normal is bumped by the height map's slope
  (surface gradient). Best on refined subdivision surfaces. Culling bounds grow
  by the largest offset. Height maps are read at the mip whose texels are as
  far apart as the mesh's vertices, so a map finer than the mesh doesn't
  alias. With `displacementGraphs`, a displacement driven by a shading graph
  (math, noise, several textures) runs that graph per vertex.
- **Levels of detail** (`lod`, `lodBias`): meshes of 1000 triangles or more
  get up to three coarser levels (quadric error simplification that also
  weighs normals, uvs and colours, and joint weights for skinned meshes);
  culling picks the coarsest level whose error stays under about a pixel with
  hysteresis. Measured: crowd 54.5M triangles drawn down to 13.6M with the
  image within 0.6% of pixels, GPU 68 ms to 28-34 ms; Kitchen at Very High
  34.8M to 6.4M triangles, 30 ms to 10.5 ms. With `lodFade` a level change
  crossfades over 8 frames with a moving dither that TAA or the film blends,
  instead of popping.
- **Skinning** (UsdSkel): GPU linear blend or dual quaternion
  (`primvars:skel:skinningMethod = "dualQuaternion"`), four or eight influences,
  blend shapes; per-instance poses so a crowd is one indirect draw per
  prototype. Authored normals on skinned meshes are kept.
- **Curves** (BasisCurves): linear, bezier, bspline and catmullRom, tubes of
  six sides or flat ribbons when normals exist; `widths`, `displayColor`,
  materials, instancing. u runs along each curve for hair shading and
  textures. 100k hair curves come up in about 2 s. `curveRibbons` (or a
  curve's `primvars:eldr:ribbons`) draws curves without normals as flat
  ribbons turned towards each pass's eye: 2 triangles per step instead of 12.
- **Points**: icosahedra `widths` across, or with `pointSprites` (or
  `primvars:eldr:sprites`) camera-facing discs shaded like spheres.
- **Volumes**: see above.

## Motion blur, depth of field and upscaling

**Camera lens** (UsdGeomCamera): `focalLength`, `horizontalAperture`,
`fStop`, `focusDistance`, `shutter:open`, `shutter:close`. usdview's free camera
has no lens (no DOF, no blur).

- **Depth of field.** Interactive/TAA: a thin lens's circle of confusion
  gathered on a golden-angle spiral before TAA, with a 32x32 tile pass of the
  largest near-field circle so blurred foreground spreads, background never
  bleeds over sharper foreground. Film: `dofJitter` instead moves the camera
  across the aperture per sample, shearing the view so the focus plane stays
  (true bokeh with occlusion). With `dofResidualBlur` (on) the post DOF
  still runs per sample at 1/√(film samples) of the circle of confusion,
  filling the gaps between lens samples, as EEVEE does.
- **Motion blur.** Interactive: McGuire's reconstruction along each pixel's
  motion (camera reprojection plus object motion vectors including skinning and
  blend shapes) after TAA. Film: `motionBlurTimeSamples` renders the scene at a
  Halton-spread time inside the shutter per sample (box shutter, motion
  linear from the previous frame to this one for instances, joints, blend
  shapes and the camera), so shadows, rays, refraction and DOF all see the
  moving geometry. Costs one shutter pass per sample (1.7-2.1 ms on the 20k
  crowd). With `sampledMotion` (on) rigid prims, instances and the camera
  take their motion from the scene's transform samples at shutter open and
  close (continued linearly to a frame), not from the last rendered frame, so
  a single usdrecord or husk frame is blurred too. With `deformationBlur`
  (on) meshes whose points move (point caches) get a velocity per point
  from Hydra's point samples at shutter open and close, which time samples
  and motion vectors follow (ray tracing still sees the frame's points).
  Skinned joints still need a rendered frame before.
- **Upscaling** (`renderScale` below 1): while TAA resolves, render at that
  fraction (min 0.25) and reconstruct into a full-size history (Gaussian
  weights from the 3x3 samples' sub-pixel positions). Interactive only: the
  film and final quality render at full size, as do frames with DOF, an open
  shutter, lens distortion or extra AOVs.
- **Lens distortion**: `lensDistortion:*` camera attributes (`k1`, `k2` radial;
  `k3`-`k6`, `p1`, `p2` OpenCV terms; `center`, `anaSq`, `asym`, `scale`,
  type `standard` or `fisheye`) warp the finished frame with ids and depth.
  The scene renders wider (overscan, up to 2x). Karma's lens material binding
  (`karma:camera:material:binding` with `karma:camera:use_lensshader`) is read
  so Karma cameras distort the same.

## Output: AOVs, EXR products, Cryptomatte, light splits

**Hydra AOVs** (usdview AOV menu, `usdrecord`, husk): `color` (RGBA16F, linear
unless the tonemapper runs; see below), `depth`, `primId`, `instanceId`
(picking and selection), plus the extra AOVs below. They cost nothing unless
bound. Where nothing was drawn they are 0.

**Extra AOVs and Karma-style names.** The names UsdRender hands Hydra are the
source names of Houdini Karma's standard render vars (nothing of Houdini is
linked).

| AOV | Names it answers to | Content |
|---|---|---|
| Normal | `normal`, `ray:hitN`, `ray:hitN_facing` | World-space shading normal (always camera-facing, Karma's N_facing) |
| Camera normal | `Neye`, `ray:hitN_camera` | Camera space |
| Position | `ray:hitP` | World space |
| Camera position | `Peye`, `ray:hitP_camera` | Camera space |
| Depth | `ray:hitPz` | Distance along the view axis, positive |
| Camera depth | `cameraDepth` | Camera-space z (negative in front, like hdEmbree) |
| st | `primvars:st` | Texture coordinates (1 to 3 components) |
| Base colour | `export_basecolor` | Base colour |
| Diffuse albedo | `lpe:CDA` | Base colour times (1 - metallic) |
| Emission | `lpe:CO`, `lpe:C.*O` | Emitted radiance (emission seen in reflections is not tracked) |
| Diffuse | `lpe:C<RD>L` (direct), `lpe:C<RD>.+L` (indirect), `lpe:C<RD>.*L` (combined) | Light splits |
| Glossy | `lpe:C<RG>L`, `lpe:C<RG>.+L`, `lpe:C<RG>.*[LO]` | Light splits |
| Cryptomatte object | `primvars:__name` | Ids of the rprim path |
| Cryptomatte material | `primvars:__materialname` | Ids of the material path |

A bound buffer of any float format gets as many components as it has. Only one
buffer per kind: if two names for the same kind are bound, the last wins.

**Light splits.** "Direct" is the lights' (distant, sphere, rect, disk,
cylinder) and, as in Karma, the dome's directly visible light
(`splitDomeDirect`, on): its diffuse irradiance under the AO's or SSGI's
visibility, and its reflection under specular occlusion where no probe,
mirror, SSR or traced reflection replaced it. Indirect is what GI's probes,
SSGI, multi-bounce, probes and reflections add beyond that. Direct and
indirect add up to the combined value, and with the emission to the linear beauty
(tests/test_aov_sums.py), for diffuse, coat, sheen and emissive surfaces, plain and under
GI, to within 3%. Not in the splits: the denoised glossy SSR's rays (6-7% of a lit sphere
with `ssrDenoise`), a transmissive surface's refracted light, and for subsurface
materials the profile blur (up to 10% across light and shadow edges) and the
thin-walled transmission; the splits are the light before them. A sheen or coat's
dome light counts as dome light (direct with `splitDomeDirect`), also under GI. They are
only compiled into the shading pipelines when an AOV asks for them (one bit of
the pipeline set key), at 0.92 ms against 0.62 ms for the shading pass of the
lights test scene. They accumulate like the colour: over TAA frames, or the film.

**Light path intensity** (`directIntensity`, `indirectIntensity`, both 1; Blender 5.1)
multiplies the same direct and indirect light in the final pixel, not in the light transport
(GI still bounces unscaled light) nor in the AOVs. Closures outside the splits (subsurface
transmission, refraction, fog, toon) stay unscaled. At 1 the shaders compile without it.

**EXR render products** (`renderProducts`): with an active UsdRenderSettings
prim (`usdrecord --renderSettingsPrimPath`, husk), each raster product becomes
one EXR through OpenImageIO, once the frame has converged. Render vars name
their AOVs as UsdRender does (`sourceType` `lpe` gives `lpe:` plus the source
name, `primvar` gives `primvars:` plus it, else the source name). Channel
layout:

- The first colour render var is the R, G, B, A layer; further ones are
  `<var>.R/G/B/A`; single channels are `<var>.Z`.
- primId and instanceId are int-valued `<var>.R`.
- Cryptomatte vars become `<var>00` layers (rank 0 and 1) with the spec's
  metadata (`cryptomatte/<key>/name`, `hash` MurmurHash3_32,
  `conversion` uint32_to_float32, `manifest` JSON of name to hex id). The
  manifests are also in `GetRenderStats()["cryptomatte"]`. Ids take the film
  sample's nearest the pixel centre; they are not averaged.
- Files are zip-compressed float32 EXR. With `productResolution` (on) the
  frame renders at the first raster product's `resolution`, conformed to its
  `pixelAspectRatio` by its `aspectRatioConformPolicy` and cut to its
  `dataWindowNDC`, when that isn't the app's buffer size (usdrecord sizes
  its buffers from `-w` and the camera). A product with nothing authored
  renders at UsdRenderSettingsBase's fallback 2048 x 1080, as in husk and
  Karma, not at the app's size: the EXR's display window is the
  resolution, its data window the part rendered, with `PixelAspectRatio`.
  The app's colour, depth and ids get that frame resampled; its other AOVs
  stay empty. Off: the frame's size.

**Colour in the EXR.** By default (`productLinear`) the beauty is linear and
untonemapped: the scene exposed by the frame's exposure, without bloom, whatever
`eldr:tonemapper` says (the viewport keeps its tonemapper). For values exactly
as rendered turn off `eldr:autoExposure` and set `eldr:exposureCompensation` to
0. With `productLinear` off the beauty is what the post chain presents:
exposed, bloomed and tonemapped (ACES by default).

**Holdouts and mattes** produce black with zero alpha; see
[authoring](#usd-authoring-conventions).

## Performance

- **GPU-driven scene**: vertex, index and instance pools are device-local;
  only changed meshes upload. Culling (frustum, small objects, LOD choice)
  produces indirect draws; shadow cascades and atlas tiles are culled the same
  way. Big scenes: pools are sized once per commit; draws split into calls of at
  most 500M triangles so a huge frame cannot trip the driver timeout.
- **Optional features cost nothing when off.** The shading pipelines are
  compiled in sets, one per combination of optional features a frame uses
  (`kCompiledFeatures` specialization constant), because skipped branches still
  cost registers: all features together made the plain fragment stage 123
  registers and 103 KB instead of 110 and 56 KB (1.75 ms on the crowd). So
  turning a feature on the first time compiles a new set (about 96
  pipelines, up to 35 s cold on a heavy scene).
- **Pipeline cache** (`pipelineCache`): compiled pipelines are saved to disk
  (`%LOCALAPPDATA%\Eldr\pipelines-<driver uuid>-<shader hash>.bin`); a new shader
  build starts a new file (delete old ones to reclaim space). Crowd first frame
  33.5 s to 2.0 s on a second run.
- **Prewarm** (`pipelinePrewarm`, Hydra default): after each frame whose
  features changed, a worker thread compiles the sets one toggle away (GI,
  SSGI, fog first) on half the cores, so toggling in usdview takes tens of
  milliseconds rather than half a minute. Batch renders do not need it.
- **Culling**: GPU frustum culling, small-object culling (`cullPixels`),
  levels of detail, and distance-based light cut-off. There is no Hi-Z
  occlusion culling.
- **GPU sharing**: with an OpenGL Hgi, colour and depth reach usdview without a
  CPU round trip (1080p frame 17.4 ms to 3.2 ms).
- **Numerics**: NaN/Inf are cleaned where they enter each history (TAA, SSGI,
  GI, exposure), so one bad pixel cannot spread.

Rough budget on a GTX 1080 Ti at 1280x720 for the 20k crowd, defaults: about
21 ms. Costly opt-ins: `ssrDenoise` (+9 ms on that scene), `rtShadows`
(+10 ms Kitchen), `rtReflections` (+15 ms Kitchen), `gi` (2-3 ms), volumes
(7-22 ms for a large cloud). They matter little to seconds-per-frame renders.

## USD authoring conventions

What Eldr reads beyond standard UsdPreviewSurface, UsdLux and UsdGeom. All are
optional; the namespaces are `eldr:` for scene-level features and `eldr:` for
shading inputs.

| Convention | Where | Meaning |
|---|---|---|
| `bool primvars:eldr:reflectionProbe = 1` | any mesh, typically an invisible Cube | The mesh is a reflection probe: never drawn, never in the scene bounds or shadows. Its world box is the influence and parallax volume, captured from its centre. Up to 8, smallest first. Needs `eldr:reflectionProbe` on. |
| `bool primvars:eldr:planarReflection = 1` | a flat mesh | Marks the mirror for planar reflections. Without any, the largest flat glossy opaque mesh is used. Needs `eldr:planarReflections`. |
| `bool primvars:eldr:hair = 0` | BasisCurves, constant | Opt out of hair shading: the curves shade as ordinary surfaces with SSR (cables, ropes). Unauthored or true: hair. An int works. |
| `float inputs:eldr:toonBands = 3`, `float inputs:eldr:toonSoftness = 0.05` | a material's surface shader | Toon bands per unit of light (0: none) and band edge width, in bands. |
| `inputs:eldr:clearcoatColor` | UsdPreviewSurface shader | Coat tint (UsdPreviewSurface has none; read as standard_surface `coat_color`). Constant or connected. |
| `int primvars:karma:object:holdoutmode = 1` (Karma's) | a gprim | Holdout: black, zero alpha, keeps depth, ids and shadows. Mode 2 (background) is treated as matte. A gprim in the active UsdRenderPass's `matte` collection is a holdout too (through Eldr's own `eldr:matte` primvar). |
| `collection:matte` on a UsdRenderPass | active render pass | Everything in the collection is a holdout (the delegate reads the pass itself; usdview: "Set As Active Render Pass"; `usdrecord --renderPassPrimPath`, which needs the pass's `renderSource`). Whole gprims only. |
| UsdRenderSettings, RenderProduct, RenderVar | `/Render/...` | Active settings write EXR products with the AOVs and Cryptomatte listed above. `sourceType` `lpe` and `primvar` are honoured. |
| `lensDistortion:k1,k2,k3,k4,k5,k6,p1,p2,center,anaSq,asym,scale` and `lensDistortion:type` (`standard`, `fisheye`) | custom attributes on a camera | Lens warp. `k1`, `k2`, `type`, `center`, `anaSq`, `asym`, `scale` come through HdCamera; `k3`-`k6`, `p1`, `p2` are OpenCV terms Eldr reads from the camera. See [Output](#output-aovs-exr-products-cryptomatte-light-splits) and `renderer/distortion.h`. |
| `karma:camera:material:binding`, `karma:camera:use_lensshader` | camera | A Karma Physical Lens (`kma_physicallens`) material's `opencv_k1..k6`, `opencv_p1`, `opencv_p2` become the same distortion. Edits reach the camera on its next sync. |
| `primvars:skel:skinningMethod = "dualQuaternion"` | skinned mesh | Dual-quaternion skinning (otherwise linear blend). |
| `inputs:angle` | DistantLight | Sun angular diameter for soft shadows (plus the film's jitter). |
| `inputs:texture:file` | RectLight, DiskLight, DomeLight | Image over the light's surface (rect and disk; as UsdLux specifies, st (0,0) is the +X +Y corner) or the dome's latlong. |
| `shadow:enable` | lights | Lights without it cast no shadows. |
| `emissiveColor` (constant) | UsdPreviewSurface or standard_surface emission | Small bright emitters are promoted to lights. |
| Camera `fStop`, `focalLength`, `focusDistance`, `horizontalAperture`, `shutter:open`, `shutter:close` | UsdGeomCamera | Depth of field and motion blur. |
| `UsdUVTexture` `wrapS/wrapT` | textures | repeat, mirror, clamp, black, useMetadata (the default: the file's own wraps, black without). MaterialX `image` `uaddressmode`/`vaddressmode`: periodic, mirror, clamp, constant. |
| Sdf asset paths with `<UDIM>` | textures | Tiles 1001-1100 are found by the resolver (also inside usdz). |
| Subdivision tags, GeomSubsets, `displayColor` primvars | meshes | As UsdGeom defines them. |
| UsdVolVolume with OpenVDBAsset fields (`fieldName`, `fieldIndex`) and a MaterialX `volume` material | volumes | Field roles `density`, `temperature`, `Cd`/`color`, `emission`. |

Houdini: a separate optional module of the Houdini build (not required for
the standalone renderer) maps render statistics into Solaris; Houdini ramp
nodes work through the MaterialX path.

## Known limitations

Collected from the `ponytail:` comments in the code (shortcuts with a known
ceiling). Each is a candidate for later work.

Output and Hydra

- Render products all render at the first raster product's framing; the
  collection is ignored. Vars Eldr cannot
  fill are left out with a warning. No `elementId` AOV (face picking).
- Cryptomatte has one sample per pixel: edges do not blend.
- Volumes are not pickable. A field prim's own transform is ignored (as in
  Storm); only OpenVDB assets.
- Dome `poleAxis`, portals, shadow colour/distance and IES profiles are
  ignored.
- One frame in flight; the renderer is synchronous.

Lighting and shadows

- Cluster capacity is 63 lights; the rest are dropped. Every cluster tests
  every light (fine for hundreds).
- Sun shadows: no blend between cascades; kernels clamp to 5% of a cascade.
- Local light shadows: all-or-nothing redraw when a caster moves; kernels are
  clamped at cube-face seams.
- Textured lights use two taps, not a filtered polygon; fog, ray-traced
  reflections and emissive-as-light do not see the texture.
- Emissive surfaces become lights only when constant, rigid, small and
  at most 32; textured, program-driven, skinned or big emitters light the scene
  through GI only.
- Fog: single-tap shadows; the dome's light in fog is unoccluded without GI.

GI and reflections

- GI probes cannot tell a single-sided wall from the inside of a closed object;
  meshes without normals use the winding for back faces.
- SSGI/SSR: no depth mip chain (very large radii sample sparsely); SSR
  full resolution and one ray; hits in the history are shaded from last frame's
  image, so only what is on screen is reflected.
- Probes: reflections inside probes lack screen-space effects and volumes;
  prefiltering on the CPU; no seam filtering; one sharp reflection for rougher
  mirrors in planar reflections; planar mirrors do not share coplanar renders.
- Ray tracing: translucent meshes do not block rays; rt skinned instances are
  capped; no BLAS compaction (about twice the memory); local lights are
  unshadowed at hit points; the closest-hit normal is the rest triangle's for
  displaced meshes.

Transparency

- OIT and sorting are per mesh; one mesh's triangles keep their order.
- Refraction is screen space outside final quality (ray-traced refraction
  traces only the nearest refracting surface's ray, and skips skinned glass);
  the translucent depth for volumes covers whole
  meshes and only the nearest layer splits a volume ray.
- Holdout mattes cut everything at the pixel (also layers in front); the
  holdout opacity comes from constants or textures, not material programs.
- Subsurface is a per-channel Gaussian (not Burley), the sun only for
  transmission.

Materials

- A graph is compiled whole or not at all; unsupported nodes (and Arnold or
  RenderMan volume shaders) leave the input at its constant.
- Gray textures in gamma-encoded colour spaces are not decoded.
- No tangent primvars: the tangent frame follows the uv set's derivatives.
- Coat: no coat normal, anisotropy or screen-space reflection; OpenPBR
  `coat_darkening` is not modelled. Thin film assumes air above it. Hair has no
  TT lobe (backlit hair gets no glow) and a closed curve's u jumps at its weld.
- A textured `thin_walled` or `transmission_depth` acts per pixel in shading,
  but ray-traced refraction reads their constants. With the textured setting
  off, a material with a textured weight is still drawn in the translucent or
  subsurface pass (weight 0).
- Displacement: top mip only; vector displacement drops tangential
  components; no shading graphs in the vertex stage; skinning stretch does not
  scale the bump; HdDisplayStyle `displacementEnabled` is not read.
- Texture capacity: textures past the table size fall back to constants. A mesh
  carries up to two extra uv sets (vertex-rate primvars share four floats); a
  GeomSubset whose first uv set differs from the mesh's reads the mesh's.
- Skinning is applied after refinement from the rest pose; subdivision refines
  skinned meshes in the rest pose.

Geometry

- LOD: border and seam vertices never move, so meshes cut into many uv
  islands simplify little; no dithered crossfade; meshes with over a million
  instances keep one coarser level only.
- Curves are world-space tubes of six sides (12 triangles per step).
- No Hi-Z occlusion culling.

Film and camera

- Motion blur uses a box shutter and linear motion between frames.
- The film's pixel filter gather is 3x3, enough up to the default 1.5 px.
- Adaptive sampling rarely stops early on scenes with edges.
- The overscanned view renders at the output resolution, so the centre
  loses detail by up to 2x; Karma's curvature and aberration lens effects and its exact
  NDC for wide images are not matched.
- Post: bloom does not spill onto the app's background colour.

Performance and tooling

- Pipeline prewarm handles single toggles only (GI and SSGI at once wait for a
  compile), and prewarms sets without the light splits.
- Cache files of older shader builds stay on disk; delete the folder to
  reclaim space.
- The scene upload is one synchronous submit per commit; the volume grid pool
  is uploaded whole when any grid changes.
- The Attribute Editor (usdview plugin) edits to the session layer only.
