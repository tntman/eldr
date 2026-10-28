# How Eldr works

Eldr is a Hydra 2.0 render delegate on Vulkan 1.3 for OpenUSD 26.08. It is a rasterizer (clustered forward shading, GPU culling and skinning); ray tracing, where the GPU has it, adds shadows, reflections, refraction and GI. Every feature has an `eldr:` setting.

| Section | Topic |
|---|---|
| Quick recipes | Rendering a frame, final-quality settings |
| Frame pipeline | GPU passes in order |
| Film and final quality | Sample accumulation |
| Lighting and shadows | Lights, shadow modes |
| Global illumination | Probes, SSGI, AO |
| Reflections | SSR, probes, planar, ray-traced |
| Transparency | OIT, refraction, holdouts |
| Volumes and fog | OpenVDB, froxel fog |
| Materials | MaterialX, OpenPBR, lobes |
| Geometry | Subdivision, displacement, LOD, skinning |
| Camera | DOF, motion blur, distortion |
| Output | AOVs, EXR, Cryptomatte, light splits |
| Performance | Pipelines, caches |
| USD authoring conventions | Primvars and attributes |
| Known limitations | |

## Quick recipes

Render a frame to EXR with a render settings prim (needs a USD built with OpenImageIO; Eldr writes the files once the frame converged):

```
usdenv.cmd usdrecord --renderer Eldr --camera /cam --renderSettingsPrimPath /Render/settings scene.usda out.png
```

Settings from Python:

```python
engine.SetRendererPlugin("HdVkRendererPlugin")
engine.SetRendererSetting("eldr:finalQuality", True)
engine.SetRendererSetting("eldr:filmSamples", 256)
```

| Goal | Settings |
|---|---|
| Converge from scratch | `eldr:finalQuality` on, `eldr:filmSamples` 128-512 |
| Linear ungraded EXR for comp | Default (`eldr:productLinear`); `eldr:autoExposure` off and `eldr:exposureCompensation` 0 for unexposed values |
| True soft shadows, bokeh, motion blur | Leave `eldr:shadowJitter`, `eldr:dofJitter`, `eldr:motionBlurTimeSamples` on (defaults) |
| Indirect light | `eldr:gi`, `eldr:ssgi` (+ `eldr:ssgiBitmask` for thin occluders) |
| Accurate off-screen shadows and reflections | `eldr:finalShadows` `raytraced` or `auto`; `eldr:rtReflections` with 1 ray |
| Clean geometry | `eldr:lodBias` -1 or `eldr:lod` off; `--complexity veryhigh` or `eldr:maxRefineLevel` |
| Fireflies | `eldr:directClamp`, `eldr:indirectClamp` |

| Environment variable | Effect |
|---|---|
| `HDVK_NO_RAY_TRACING=1` | Pretend the GPU has no ray tracing |
| `HDVK_PIPELINE_CACHE_DIR` | Move the pipeline cache (default `%LOCALAPPDATA%\Eldr`) |
| `USD_ROOT`, `USD_VENV`, `HDVK_BUILD` | Read by `usdenv.cmd` (sets plugin paths) |

## Frame pipeline

GPU passes in order. Names are the timers in usdview's **Eldr > Render Statistics** and `GetRenderStats()["gpuPassMs"]`. (opt) = runs only when needed.

| # | Pass | What it does |
|---|---|---|
| 0 | `probeCapture`, `planarReflection` (opt) | Draw the probe cube and mirrored views |
| 1 | `skinning` | Last frame's joints kept for motion, then GPU joint matrices from skeletons |
| 2 | `shutter` (opt) | Moves instances, joints, blend shapes, camera to the sample's shutter time |
| 3 | `cullCamera`, `cullShadows` | Frustum and size culling, LOD choice, indirect draws: the camera's, then the 4 sun cascades' |
| 4 | `lightClusters` | Local lights into 16x9x24 view clusters |
| 5 | `sunCascade0`..`3`, `sunShadows`, `sunShadowMin` | The 4 cascades' depth, one each; translucent and volume tint maps; min-depth reduction for the soft search |
| 6 | `depthPrepass` | Depth plus cutout alpha tests |
| 7 | `accelerationStructures`, `rayTracing` (opt) | BLAS/TLAS; traced shadows, reflections, GI probe updates |
| 8 | `ssgi` or `ambientOcclusion` | SSGI (replaces AO) or GTAO |
| 9 | `localShadows` | Atlas for sphere, rect, disk, spot, cylinder lights |
| 10 | `fog` (opt) | Froxel fog |
| 11 | `shading` | Sky, then opaque PBR: sun, local lights, dome, probes, GI, SSR, fog. Writes colour, ids, motion, light splits |
| 12 | `ssrDenoise` (opt) | Glossy SSR filters |
| 13 | `subsurface` (opt) | Screen-space subsurface blur |
| 14 | `volumes` (opt) | OpenVDB march; `volumesInFront` is the part before glass |
| 15 | `translucency` (opt) | Refraction layers, OIT, holdouts |
| 16 | `depthOfField` (opt) | Post DOF (skipped with lens jitter) |
| 17 | `film` or `taa` | Film average or TAA resolve |
| 18 | `motionBlur` (opt) | Post blur (skipped with time samples) |
| 19 | `post` | Auto exposure, bloom, tonemap |
| 20 | `aovs` (opt) | G-buffer and packing for bound AOVs |
| 21 | `lensDistortion` (opt) | Warps colour, depth, ids |
| 22 | `readback` | Colour and depth to OpenGL on the GPU, or to the CPU |

One frame in flight (synchronous). The 20k-agent crowd renders in about 21 ms at 1280x720 with defaults.

## Film and final quality

Three ways to turn jittered samples into a pixel.

| Mode | When | How |
|---|---|---|
| Interactive TAA | Camera or scene moves | Halton 2,3 jitter; Catmull-Rom history with YCoCg clipping and motion vectors. Effects loop over `eldr:interactiveSamples` (8) noise samples. `eldr:renderScale` < 1 upscales. |
| Film | Still for 3 frames; first frame of a fresh history (usdrecord) | fp32 weighted average through a Gaussian filter (`eldr:filmFilterRadius`, 1.5 px) until `eldr:filmSamples` (64): then converged (`IsConverged`, `percentDone`). Any camera, scene or setting change restarts it; a camera prim synced again unchanged (as Houdini's free camera may be) does not. `eldr:taaSamples` only sets the jitter cycle. |
| Final quality | `eldr:finalQuality`, `enableInteractive = false` (hdPrman convention), husk, or the renderer's first frame (usdrecord; not in Houdini's viewport, marked by `houdini:viewport` or `houdini:interactive`) | The film restarts from scratch after every change, never from the TAA image. A later fresh history (resize, camera switch) starts an ordinary film. |

What the film enables (each has a switch, all on by default):

| Feature | Setting | Notes |
|---|---|---|
| Exact soft-shadow penumbrae | `eldr:shadowJitter` | Final-quality runs only |
| True depth of field with occlusion | `eldr:dofJitter` | |
| Motion blur at shutter times | `eldr:motionBlurTimeSamples` | |
| Fog without history | `eldr:fogFilmJitter` | |
| GI preconvergence | `eldr:giPreconverge` | Film waits for fresh probes |
| Adaptive sampling | `eldr:adaptiveThreshold` (0), `eldr:adaptiveFinalThreshold` (0 = off; opt in for final quality) | Converged once `eldr:adaptivePixelFraction` (0.999) of pixels stopped. Not with motion blur time samples (they run to `eldr:filmSamples`). |

- Stochastic effects use per-effect Sobol dimensions with blue-noise rotation (`eldr:sampleDimensions`). RMSE against a 1024-sample reference, 64 samples: sunangle 0.0034 to 0.0003, fog 0.0052 to 0.0004, Kitchen_set 0.0068 to 0.0004; 0.2-0.5 s.
- Ids, depth and surface AOVs (normal, st, P, camera depth) are the sample nearest the pixel centre, not averages.
- Colour AOVs (albedo, emission, light splits) are filtered like the beauty, so the splits keep summing to it. Without the film (TAA) they are a running mean.

## Lighting and shadows

Lights are UsdLux: radiance = `color * intensity * 2^exposure`, times blackbody if `enableColorTemperature`, divided by area when `normalize`.

| Light | Support |
|---|---|
| DistantLight | The sun; `inputs:angle` gives soft penumbrae. The first shadow-casting one gets the cascades. |
| SphereLight | Energy-conserving representative-point specular, horizon-clipped diffuse, `treatAsPoint` |
| RectLight, DiskLight | Exact polygon form factor (rect) or illuminance (disk); emit from -Z; `inputs:texture:file` textures them (Karma orientation: left edge at +X, top row at +Y) |
| CylinderLight | Side only; `treatAsLine` |
| DomeLight | Latlong `inputs:texture:file`, rotation; diffuse SH9 and GGX-prefiltered cube |
| Shaping | Cone angle, softness, focus, focus tint |

- **Highlights of sized lights** are energy-conserving: reflected power, peak and shape match a brute-force GGX integral within 5% (and Karma) for sphere, rect, disk, sun and cylinder. Diffuse is unchanged. Textured lights are read at the lobe's mip.
- **Clustered lights:** 63 per cluster, cut off at 1% of mid-gray on a white surface.
- **Emissive meshes** (constant `emissiveColor` above `eldr:emissiveLightThreshold`, small, max 32) also become rect or sphere lights with shadows.
- **Light shapes** are hidden from the camera unless `eldr:lightShapesVisible`; `inputs:karma:light:renderlightgeo = 0` hides one. The same attribute on a dome hides it from the camera (unauthored: shown, unlike Karma) while it still lights and reflects; `eldr:domeCameraVisible` and Hydra's `domeLightCameraVisibility` hide every dome.
- **Dome sun** (`eldr:domeSun`): bright HDRI texels become a shadowed distant light.
- **Clamps:** `eldr:directClamp` (0 = off) and `eldr:indirectClamp` (10) limit fireflies.
- **Ambient:** GTAO on the dome's light, or with SSGI the traced visibility.

### Shadow modes

| Mode | How |
|---|---|
| Sun cascades | 4 cascades fit to the scene, texel-snapped, normal offset and slope bias. Soft from the light angle (PCSS, bounded blocker search) or 3x3 PCF. |
| Local light atlas | 4096x4096 tiles; spots one perspective tile, others six cube faces; nearest lights first. Tiles halve (to 64, about 680 lights) before any light goes unshadowed. Redraws only when scene or tiles change. |
| Translucent and volume tint | Colour maps beside the shadow maps. |
| Ray-traced (`eldr:rtShadows`) | Sun + 3 local lights (15 in final quality), soft from light size; skinned instances capped (`eldr:rtSkinnedInstances`). Rays pass translucent casters (taking their colour) and march volumes (`eldr:rtTranslucentShadows`). |
| Final-quality traced (`eldr:finalShadows`) | `maps`, `raytraced` or `auto`. One ray per light and film sample at the sample dimensions' point of the light: exact penumbra. `auto` traces dense scenes whose skinned instances all block rays. |
| Jittered maps (`eldr:shadowJitter`) | Maps rendered from a point on the light each film sample: exact penumbra (sun error 0.016 vs PCSS 0.065). |

Traced shadows cover opaque pixels only. With volumes, fog, translucent surfaces or subsurface in the scene a traced sun keeps its cascades for those (1.05 ms on volume.usda); traced local lights keep atlas tiles for them (about 1.4 ms for three lights; still frames 0-0.2 ms).

## Global illumination

| Layer | Setting | Needs | What it gives |
|---|---|---|---|
| Dome light + AO | `eldr:imageBasedLighting`, `eldr:ambientOcclusion` | prepass | Unoccluded sky light darkened by GTAO |
| DDGI probes | `eldr:gi` | ray tracing | Probe grid (`eldr:giProbes`, `eldr:giRaysPerProbe`); octahedral irradiance and visibility atlases blended by `eldr:giHysteresis`; Chebyshev visibility prevents leaks; probes in geometry drop out; hits shaded with direct light, emission and probe bounces; dome fallback |
| SSGI | `eldr:ssgi` | prepass | Near-field indirect light from the screen (GTAO horizon scan, last frame's lighting) at half resolution, denoised. Replaces AO. |

- `eldr:ssgiBitmask`: 32-sector bitmask, light passes behind thin objects (occluder thickness `eldr:ssgiBitmaskThickness`).
- `eldr:ssgiThickness`: rays respect thin and grazing surfaces.
- `eldr:screenBackface`: back-face hits take `eldr:screenBackfaceScale` (0.25) of the light, in SSGI and SSR.
- Slices are normalized so a white furnace never exceeds the open sky.
- `eldr:emissiveLights`, `eldr:fogGiAmbient`, `eldr:probeNormalize` use GI.
- `eldr:giPreconverge`: the probes' about 33 updates run in the first frame.

Costs at 1280x720: probes 2-3 ms (Kitchen), SSGI 0.67 ms at half resolution.

## Reflections

Each pixel takes the best source for its roughness.

| Source | Setting | How |
|---|---|---|
| SSR | `eldr:ssr` | 32-step ray march with bisection, shaded from last frame; fades with roughness (0.3-0.6) and at edges. `eldr:ssrHitFilter` reads hits at the ray cone's mip (6 mips). Hits outside last frame's view are misses. |
| SSR denoiser | `eldr:ssrDenoise` (off) | One GGX ray per pixel, 9-ray spatial filter, temporal reprojection, bilateral filter; roughness up to 0.8. Crowd shading 11.7 to 20.4 ms. Not with ray-traced reflections. |
| Reflection probes | `eldr:reflectionProbe` | Captured cube with parallax correction. One automatic probe, or up to 8 authored (`primvars:eldr:reflectionProbe`; smallest box wins, soft edges). `eldr:probeNormalize` needs GI or SSGI. |
| Planar reflections | `eldr:planarReflections` | Mirrors (`primvars:eldr:planarReflection`, else largest flat glossy opaque mesh) render the scene mirrored with an oblique near plane; `eldr:planarCount` (4) at `eldr:planarScale` (0.5). |
| Ray-traced reflections | `eldr:rtReflections` | GGX rays shaded at hits, up to roughness 0.7, replace dome and SSR; rough ones averaged over up to 12 pixels. Sees displacement (`eldr:rtDisplacement`) and skinned instances up to the cap. |

Mirror pixels take the planar reflection; others use the probe (if on) or the dome as the base, with SSR where the screen has the hit.

## Transparency

| Feature | Setting | How |
|---|---|---|
| Cutouts | | UsdPreviewSurface `opacityThreshold > 0`, MaterialX opacity textures at 0.5; alpha-tested in every pass, shadows too |
| Translucency | `eldr:translucency`, `eldr:oit` | Opacity below 1 (constant, or a texture without threshold). Drawn after the sky, weighted blended OIT (`eldr:oitDepthWeight`) or sorted per mesh. Diffuse and emission scale with opacity, highlights don't. |
| Refraction | `eldr:refraction` | [prepass] Transmission (standard_surface `transmission*`, OpenPBR `transmission_*`) or authored `ior` refract a mip-chained copy of the opaque colour. Thickness estimate places the exit point; roughness blurs (`eldr:refractionSamples`); Fresnel splits; absorption at the surface or Beer-Lambert over `transmission_depth`. |
| From inside | `eldr:refractionBackFaces` | Back faces first with inverted ior: Snell's window, total internal reflection (48.8 degrees for water) |
| Layers | `eldr:multiRefraction`, `eldr:refractionLayers` | Background re-copied after each layer: glass behind glass |
| Ray-traced refraction | `eldr:rtRefraction` | [RT] On in final quality. Traces the nearest refracting surface through up to `eldr:rtRefractionBounces` interfaces (total internal reflection, Fresnel choice, absorption, GGX normal per sample for rough glass). Materials are evaluated at each hit (textures, programs, normal maps); shaded like traced reflections, so off-screen content shows. Counts as indirect glossy in the light splits. Displaced glass bends rays by its displaced triangles and height-map bump (`eldr:rtRefractionDisplacement`). |
| Translucent shadows | `eldr:translucentShadows` | Glass casts faint, tinted shadows |
| Holdouts | `eldr:holdouts`, `eldr:holdoutAlpha` | Matte objects: black, zero alpha, keep depth, ids, shadows. Translucent holdouts cut by opacity, also from MaterialX graphs. |
| Subsurface | `eldr:subsurface` | See Materials |

## Volumes and fog

**Fog** (`eldr:fog`): height fog in a 160x90x64 froxel grid; medium = `density * exp(-falloff * (height - base))`, Henyey-Greenstein phase, lit by distant lights (cascades: pillars throw shafts), local lights (atlas tiles) and the dome (probes if `eldr:fogGiAmbient`). Energy-conserving; applied to surfaces and sky. No USD fog schema: settings only.

**OpenVDB volumes** (`eldr:volumes`; needs a USD with OpenVDB):

| Aspect | Support |
|---|---|
| Prims | `UsdVolVolume` with `OpenVDBAsset` fields, bound by name; `fieldIndex` honoured. Without a volume material only `density` is read, as in Karma; `eldr:volumeDefaultFields` adds `temperature` (blackbody), `Cd`/`color` and `emission` |
| Rendering | NanoVDB on the GPU, fullscreen march over the shaded image, empty space skipped |
| Lighting | HG phase, single scattering as Karma's default (3-octave multiple scattering of distant lights with `eldr:volumeMultipleScattering`), sun cascades for opaque occluders, dome SH, local lights as points via atlas tiles; emission from temperature (blackbody) or an emission field |
| Materials | MaterialX `volume` (anisotropic_vdf, absorption_vdf, uniform_edf) or Karma's `kma_volume`. Inputs: constants, `geompropvalue` field reads and their products (`constant`, `multiply`, `convert`, `dot`), blackbody of a temperature field. Absorption and scattering are per channel: coloured smoke, coloured transmittance. Coefficients are per unit of density. A colour field scales the scattering; another field times a colour is emission. `eldr:volumeAnisotropy` for volumes without one. |
| Ramps and remaps | Functions of one float field each, up to three per material, baked into 1024-entry tables over the field's value range (linear in between) and multiplied in. A function of the density replaces the density (a ramp of it is the coefficient); a function of temperature times a colour is fire. Nodes, as MaterialX's stdlib defines them: `remap`, `range`, `smoothstep`, `clamp`, `mix`, `add`, `subtract`, `multiply`, `divide`, `power`, `min`, `max`, `modulo`, `invert`, `absval`, `floor`, `ceil`, `sign`, `sqrt`, `exp`, `ln`, `ifgreater`, `ifgreatereq`, `ifequal`, `switch`, `extract`, `combine2/3/4`, `luminance`, `ramp` (linear, smooth, step; standard, radial, circular, box), `ramplr`, `ramptb`, `ramp4`, `splitlr`, `splittb` (a hard step: no derivatives in a volume), a field into `texcoord` through `convert` or `combine2`. Houdini's: the MtlX Color/Float Ramp and Cubic Ramp VOPs (`hinvlinear`, `huniformramp`, `huniformcubic`, `hcatmullrom`), Karma's `kma_rampconst` (constant, linear, Catmull-Rom). A node of two fields, or any other node: the material is ignored with a warning. Measured against Karma CPU (husk, Houdini-authored networks): emission and absorption within 5% per channel (a Catmull-Rom ramp's faint blue 14%); lit scattering (single, as Karma's default) within 3% for ramps of the density. |
| Houdini SOP volumes | Houdini build only; see the Houdini page |

| Interaction | Setting |
|---|---|
| Volumes shadow surfaces and fog from the sun | `eldr:volumeShadows` |
| ... from local lights | `eldr:localVolumeShadows` |
| Volumes shadow each other (sun, local lights, dome); a lone volume pays nothing | `eldr:volumeMutualShadows` |
| Glass tints fog; volumes behind glass are dimmed and refracted | `eldr:volumeUnderTransparency` |
| Half-resolution march, about 3x cheaper | `eldr:volumeResolution` 2 |

## Materials

Eldr shades in linear Rec.709. Colour-space tags (`sourceColorSpace`, UsdColorSpaceAPI) convert from ACEScg, ACES2065-1, P3, Rec.2020, sRGB and gamma encodings; unknown names warn once and read as untagged.

| Surface | Notes |
|---|---|
| UsdPreviewSurface | Metallic and specular workflows, clearcoat (F0 from `ior`), `ior` refraction, opacity, cutouts, displacement, normal maps |
| MaterialX standard_surface | base, specular (`specular_color`), roughness, metalness, coat, sheen, thin film, anisotropy, transmission, subsurface, emission, opacity, normal maps, displacement |
| OpenPBR Surface | The same lobes under OpenPBR names (`fuzz_*`, `coat_*`, `thin_film_*`, `geometry_opacity`, `transmission_*`, `subsurface_*`) |

Material context order: `mtlx`, Storm's `glslfx`, then universal (`outputs:surface`). Surfaces Eldr doesn't read (a `surface` of pbrlib BSDFs and EDFs, glTF PBR, Disney, `surface_unlit`) render the default grey and warn once; docs/materialx.md has the measured node coverage.

### Spec conformance

Eldr shades each surface model by its spec: the USD spec for UsdPreviewSurface, MaterialX's nodegraphs and pbrlib (`libraries/bxdf`, `libraries/pbrlib/genglsl`) for standard_surface and OpenPBR (`renderer/shaders/brdf.glsl`'s `SetupBase`). Checked lobe by lobe against the spec formulas: tilted patches under one distant light (9 light and view directions per case, 23 cases) agree within 0.3% (worst 2.5%), and under a white furnace dome within 1% facing the camera. Differences from Karma come from rasterised lighting, not the materials (Karma agrees with the spec within 1% on coated standard_surface and OpenPBR, and on furnace albedos).

| Lobe | Spec | Status |
|---|---|---|
| GGX distribution, height-correlated Smith masking, alpha = roughness² | all models | Exact (roughness under 0.045 shades as 0.045, alpha 0.002, so point lights keep a finite highlight) |
| UsdPreviewSurface Fresnel | Schlick: F0 = ((1 - ior) / (1 + ior))² to white; metals albedo to albedo; interpolated by `metallic`; specular workflow `specularColor` to white | Exact |
| Diffuse under the specular | MaterialX `layer`: the diffuse loses the dielectric's directional albedo towards the viewer | Exact (albedo from the split-sum table) |
| Multiple scattering | MaterialX's energy compensation on every GGX lobe of standard_surface and OpenPBR; UsdPreviewSurface single scattering (Storm, Karma) | Exact form; `eldr:multiScatter` off makes everything single scattering |
| standard_surface dielectric | exact Fresnel of `specular_IOR`, weight `specular`, tint `specular_color` | Exact for lights; image lights use Schlick's albedo at F0, as MaterialX's GLSL (1-3% under exact at 50 degrees) |
| standard_surface metal | conductor of `artistic_ior(base_color, specular_color)` | Hoffman's F82 form fitted to the conductor (exact at 0, 82 and 90 degrees; within 0.5%, a dark channel up to 6% at grazing light) |
| OpenPBR metal | F82-tint Schlick (`specular_color`) times `specular_weight` | Exact |
| OpenPBR dielectric | `specular_weight` scales F0 through the index, the index relative to the coat's under a coat | Exact for lights; image lights as standard_surface's |
| Diffuse roughness | standard_surface: MaterialX's Oren-Nayar; OpenPBR: energy-compensated Oren-Nayar (Fujii) | Exact for lights (area lights: towards their centre); image lights through MaterialX's albedo fits |
| Coat | dielectric of the coat's IOR (UsdPreviewSurface: Schlick, white), coat colour on the base, albedo scaling; standard_surface `coat_affect_color`, `coat_affect_roughness`; OpenPBR roughening and `coat_darkening`; emission under the coat | Exact for lights; image lights Schlick's albedo. No `coat_normal`, coat anisotropy or coat rotation (the base's normal) |
| Sheen (standard_surface) | Imageworks microfiber over the diffuse, under the specular | Exact; image lights through MaterialX's albedo fit |
| Fuzz (OpenPBR) | Zeltner's LTC sheen over the coat and base | Exact (MaterialX's fits) |
| Anisotropy | `mx_roughness_anisotropy`, OpenPBR's mapping | Exact; tangent from the uv set's u direction (no tangent primvars) |
| Thin film | Airy reflectance with polarization, per light | Approximated: unpolarized, at the view angle, standing in for F0 |
| Transmission | dielectric BTDF under the coat (its colour and reflection attenuate it), absorption by `transmission_color` over `transmission_depth` | Approximated: refraction of the screen (or traced), Schlick at the interface, roughness as blur, path length from the depth behind (capped by the object's bounds) |
| Subsurface | random walk | Approximated: separable screen-space blur of the diffuse light |
| Constants only | `diffuse_roughness`, `base_diffuse_roughness`, `coat_affect_*`, `coat_darkening` | Textured or graph-driven values keep the constant |

Not mapped: MaterialX `gltf_pbr` (no surface), UsdPreviewSurface `opacityMode` presence (shaded as transparent). Ray-traced hits (GI rays, reflections of reflections) shade with a simplified F0 = 0.04 / metallic base.

| Topic | Support |
|---|---|
| Lobes (own pipelines; plain materials don't pay) | clearcoat (tints the base), sheen (Imageworks; OpenPBR fuzz: Zeltner), thin film (Airy), anisotropic GGX (tangent from the uv u direction, `specular_rotation`), hair (Kajiya-Kay on BasisCurves), toon bands, multi-scattering energy compensation (`eldr:multiScatter`: standard_surface and OpenPBR; UsdPreviewSurface single-scatter, as Karma) |
| Texturable inputs | base colour, roughness, metallic, emission, opacity, normal, displacement; with `eldr:texturedLobes` / `eldr:texturedTransmission`: specularColor, coat (colour, roughness), sheen, anisotropy, thin film, ior, transmission (colour, depth, thin walled), subsurface (colour, radius, scale) |
| Texture reads | UsdUVTexture, MaterialX `image` / `tiledimage`, UDIM, wrap modes, uv transforms, several uv sets |
| Material programs (`eldr:materialGraphs`) | Graphs compiled whole into per-pixel programs: math, mix, overlay, clamp, remap, conditionals, HSV, extract/combine, `image`, `tiledimage`, `place2d`, `texcoord`, `geompropvalue`, ramps, `normalmap`, UsdUVTexture, UsdTransform2d, primvar readers, Houdini ramps `hinvlinear` and `huniformramp`. Also fract, round, sign, tan, asin, acos, atan2, magnitude, crossproduct, rotate2d/3d, switch (a constant `which` compiles one input), and/or/not/xor, splitlr/splittb, time/frame (the scene's current frame), geomcolor, tangent/bitangent, viewdirection, artistic_ior, transformmatrix (constant matrices), transformpoint/vector/normal. Nodes defined by a nodegraph (MaterialX's stdlib/pbrlib/nprlib, Houdini's MaterialX library when `$HFS` is set, a document's own nodedefs) are expanded into the nodes Eldr compiles: `contrast`, `colorcorrect`, `ramp4`, `range`, `safepower`, `bump` (with `heighttonormal`, from screen derivatives; flat per vertex and at ray hits; Houdini (Karma context) materials read bump heights in scene units, as Karma), `triplanarprojection`, `unifiednoise3d`, `kma_camerafalloff`, ... Unconnected inputs read their nodedef's `defaultgeomprop` (UV0, Nworld, Pobject). With `eldr:proceduralTextures`: MaterialX noise (noise2d/3d, fractal2d/3d, cellnoise, worleynoise, position, normal), matching Storm. A node Eldr can't compile is bypassed as MaterialX bypasses it (its `defaultinput`, else its first input of the output's type, else the output's default); without a nodedef the input keeps its constant (one warning per node type). Render stats `materialFallbackInputs` and `materialFallbackNodes` (usdview's Eldr > Render Statistics, Houdini's viewport stats) count those inputs and name the nodes; unknown surface shaders count too and warn once. |
| Geometry-side | per-vertex `displayColor`, GeomSubsets with materials, any primvar a program names at any rate (including instancer primvars such as Houdini crowd `random`) |
| Subsurface | Separable per-channel Gaussian of the diffuse light (taps over 3x the largest radius, max 128 px, depth-aware); thin parts transmit the sun, thin-walled pass it whole |
| Toon | `inputs:eldr:toonBands` on a material, or `eldr:toonBands` globally; `inputs:eldr:toonSoftness` edges; emission untouched |

## Geometry

| Feature | Support |
|---|---|
| Meshes | Implicit surfaces converted to meshes, GeomSubsets, PointInstancer and native instancing (one indirect draw per prototype), render tags and purposes |
| Subdivision (`eldr:subdivision`, `eldr:maxRefineLevel`) | catmullClark, loop, bilinear via OpenSubdiv at the display style's refine level or a mesh's own (`primvars:eldr:refineLevel`); creases, corners, holes, boundary and face-varying rules; limit positions and normals. Skinned meshes refine in the rest pose, skin on the GPU. |
| Displacement (`eldr:displacement`) | UsdPreviewSurface and MaterialX displacement move vertices along normals, so depth, shadows, motion and ray tracing agree; shading normal bumped by the height slope. Best on subdivided meshes. Culling bounds grow by the largest offset. Height maps read at the mip matching vertex spacing. |
| Displacement graphs (`eldr:displacementGraphs`) | A shading graph runs per vertex, also for ray-tracing BLASes (`eldr:rtDisplacement`). Graphs reading world position, normal or instance primvars give the 256 largest instances a BLAS each (rebuilt when they or their primvars change); others share the first instance's. |
| LOD (`eldr:lod`, `eldr:lodBias`) | Meshes of 1000+ triangles get up to 3 coarser levels (quadric simplification weighing normals, uvs, colours, joint weights); culling picks the coarsest level under about 1 px of error, with hysteresis. Crowd 54.5M to 13.6M triangles, 68 to 28-34 ms; Kitchen at Very High 34.8M to 6.4M, 30 to 10.5 ms. `eldr:lodFade` crossfades over 8 frames. |
| Skinning (UsdSkel) | GPU linear blend or dual quaternion (`primvars:skel:skinningMethod = "dualQuaternion"`), 4 or 8 influences, blend shapes, per-instance poses (one draw per prototype); authored normals kept |
| Curves (BasisCurves) | linear, bezier, bspline, catmullRom; 6-sided tubes or flat ribbons (with normals); `widths`, `displayColor`, materials, instancing; u runs along the curve. 100k hair curves in about 2 s. `eldr:curveRibbons` / `primvars:eldr:ribbons`: camera-facing ribbons (2 triangles per step instead of 12). |
| Points | Icosahedra `widths` across, or camera-facing discs with `eldr:pointSprites` / `primvars:eldr:sprites` |
| Volumes | See Volumes and fog |

## Camera

Reads UsdGeomCamera `focalLength`, `horizontalAperture`, `fStop`, `focusDistance`, `shutter:open`, `shutter:close`. usdview's free camera has no lens (no DOF or blur).

| Feature | Interactive | Film |
|---|---|---|
| Depth of field | Thin-lens gather on a golden-angle spiral before TAA, with a 32x32 near-field tile pass (foreground spreads, background never bleeds over sharper foreground) | `eldr:dofJitter` moves the camera across the aperture per sample (sheared, focus plane stays): true bokeh with occlusion. `eldr:dofResidualBlur` runs post DOF at 1/sqrt(samples) of the blur to fill gaps. |
| Motion blur | McGuire reconstruction along motion vectors (camera, objects, skinning, blend shapes) after TAA | `eldr:motionBlurTimeSamples` renders the whole scene at a Halton-spread shutter time per sample (box shutter, linear motion from the previous frame; instances, joints, blend shapes, camera), so shadows, rays, refraction and DOF see the motion. 1.7-2.1 ms per sample on the crowd. `eldr:motionBlurResidual` spreads each sample over its shutter stratum along motion vectors for a smooth blur. |
| Single-frame blur | | `eldr:sampledMotion`: rigid prims, instances and camera take motion from the scene's transform samples across the shutter. `eldr:deformationBlur`: point caches use per-point velocity. Skinned joints still need a rendered frame before. |
| Upscaling | `eldr:renderScale` < 1 (min 0.25): TAA reconstructs a full-size history. Film, final quality, DOF, open shutter, lens distortion and extra AOVs render full size. | |

**Lens distortion:** camera attributes `lensDistortion:*` warp the finished frame with ids and depth; the scene renders wider (overscan, up to 2x). A Karma Physical Lens (`karma:camera:lensshader`, or a bound lens material) is read, so Karma cameras distort the same.

## Output

**Hydra AOVs** (usdview AOV menu, usdrecord, husk): `color` (RGBA16F, linear unless the tonemapper runs), `depth`, `primId`, `instanceId` and the extra AOVs below. They cost nothing unless bound. Empty pixels are 0. A bound buffer returns to the CPU only if the app read it since the last frame. While the camera moves, extra AOVs the app didn't read last frame are skipped; stills, final quality and render products render all.

**Extra AOVs.** UsdRender source names match Houdini Karma's standard render vars:

| AOV | Names | Content |
|---|---|---|
| Normal | `normal`, `ray:hitN`, `ray:hitN_facing` | World-space shading normal, camera-facing |
| Camera normal | `Neye`, `ray:hitN_camera` | Camera space |
| Position | `ray:hitP` | World space |
| Camera position | `Peye`, `ray:hitP_camera` | Camera space |
| Depth | `ray:hitPz` | Distance along the view axis, positive |
| Camera depth | `cameraDepth` | Camera-space z (negative in front, like hdEmbree) |
| st | `primvars:st` | Texture coordinates (1-3 components) |
| Base colour | `export_basecolor` | Base colour |
| Diffuse albedo | `lpe:CDA` | Base colour x (1 - metallic) |
| Emission | `lpe:CO`, `lpe:C.*O` | Emitted radiance (not in reflections) |
| Diffuse | `lpe:C<RD>L` direct, `lpe:C<RD>.+L` indirect, `lpe:C<RD>.*L` combined | Light splits |
| Glossy | `lpe:C<RG>L`, `lpe:C<RG>.+L`, `lpe:C<RG>.*[LO]` | Light splits |
| Cryptomatte object | `primvars:__name` | Ids of the rprim path |
| Cryptomatte material | `primvars:__materialname` | Ids of the material path |

A bound float buffer gets as many components as it has. One buffer per kind; if two names for a kind are bound, the last wins.

**Light splits.**

| Part | Definition |
|---|---|
| Direct | Lights' light; also the dome's unblocked light (`eldr:splitDomeDirect`): diffuse under AO/SSGI visibility, reflection where no probe, mirror, SSR or traced hit replaced it |
| Indirect | GI, SSGI, multi-bounce, probes, reflections beyond that |
| Sum | Direct + indirect = combined; with emission = the linear beauty, within 3%, for diffuse, coat, sheen, emissive, subsurface, transmission, under GI, denoised SSR and traced refraction |
| Refraction | Counts as indirect glossy (no separate AOV) |
| Denoised SSR | Indirect glossy |
| Subsurface | Moves light within the diffuse splits |
| Sun through thin subsurface | Direct diffuse |
| Cost | Compiled in only when an AOV asks: lights scene 0.92 vs 0.62 ms |
| Accumulation | Like colour: TAA frames or film |

**Light path intensity** (`eldr:directIntensity`, `eldr:indirectIntensity`, both 1) scales direct or indirect light in the final pixel only, not the light transport or AOVs: the beauty is the splits times the intensities. Refracted light and SSR reflections are scaled once. Fog and toon shading stay unscaled. At 1 the shaders compile without it.

**EXR render products** (`eldr:renderProducts`): with an active UsdRenderSettings (`usdrecord --renderSettingsPrimPath`, husk), each raster product becomes an EXR through OpenImageIO once converged. Render vars name AOVs as UsdRender does (`sourceType` `lpe` gives `lpe:` + source name, `primvar` gives `primvars:` + it, else the source name). Files are zip-compressed float32.

| Layer | Layout |
|---|---|
| First colour var | R, G, B, A |
| Further vars | `<var>.R/G/B/A`; single channels `<var>.Z` |
| primId, instanceId | int-valued `<var>.R` |
| Cryptomatte | `<var>00`, `01`, `02` (ranks 0-5, each `(id, coverage, id, coverage)`) as Karma writes them, with spec metadata: `cryptomatte/<key>/name`, `hash` MurmurHash3_32, `conversion` uint32_to_float32, `manifest` JSON. Manifests are also in `GetRenderStats()["cryptomatte"]`. |

- **Soft Cryptomatte** (`eldr:cryptomatteSoft`, film only): per pixel, the 6 ids with the most filter weight, each with its coverage over all weight (background included), sorted. Edges between two objects sum to 1, so a decoder gets the filtered matte of the beauty. State: 14 floats per pixel per AOV (116 MB at 1080p). Interactive or off: one id, coverage 1, rank 0.
- **Size** (`eldr:productResolution`): the frame renders at the first raster product's `resolution`, `pixelAspectRatio` (by `aspectRatioConformPolicy`) and `dataWindowNDC` when that differs from the app's buffers. No authored `resolution` means 2048x1080 (as husk and Karma). The EXR display window is the resolution, the data window the rendered part. The app's buffers (colour, depth, ids, AOVs, husk's float primid and Cryptomatte) get a nearest resample.
- **Colour** (`eldr:productLinear`, on): the beauty is linear and untonemapped, exposed by the frame's exposure, no bloom. For exactly-as-rendered values set `eldr:autoExposure` off and `eldr:exposureCompensation` 0. Off: exposed, bloomed, tonemapped (ACES default).

**husk.** husk binds vars by its own channel names ("C", "depth") and carries `sourceName`/`sourceType`; Eldr resolves the AOV from that, so the table above holds under husk, which writes the EXR (Float32Vec4 colour renders as half and widens). husk (which hands Eldr its products as the `rasterRenderProducts` setting) makes the frame offline, whatever the settings prim holds: final quality, no auto exposure (unless `eldr:autoExposure` is authored), linear beauty. Any `eldr:<setting>` attribute on the active UsdRenderSettings prim is that render setting, applied when its value changes.

## Performance

| Topic | How |
|---|---|
| GPU-driven scene | Device-local vertex, index and instance pools; only changed meshes upload. Meshes of identical content (a crowd's agents, each with its own joints) share one copy of their vertices, indices, skin weights and levels of detail: crowd 9k 1.7 GB to 0.57 GB, first frame 14 to 5 s. System memory: duplicates are found as meshes sync and keep one copy of their arrays; meshes without face-varying data flatten indexed (per point, not per corner); every mesh's arrays are freed once uploaded, uploads stage in 64 MB chunks, and a still subdivision surface frees its OpenSubdiv refiner. Peak in husk: Kitchen_set at husk's default complexity (level 3, 28M triangles) 11.4 to 4.3 GB, crowd 9k 12.6 to 7.6 GB, 25M unique triangles 6.3 to 4.8 GB. Culling (frustum, small objects, LOD) produces indirect draws, also for shadow cascades and atlas tiles. Draws split at 500M triangles to avoid driver timeouts. |
| Zero cost when off | Shading pipelines compile in sets per combination of optional features (`kCompiledFeatures`). A new combination compiles about 96 pipelines (up to 35 s cold on heavy scenes). |
| Pipeline cache | `eldr:pipelineCache`: `%LOCALAPPDATA%\Eldr\pipelines-<driver uuid>-<shader hash>.bin`; a new shader build starts a new file (delete old ones). Crowd first frame 33.5 to 2.0 s. |
| Prewarm | `eldr:pipelinePrewarm` (Hydra default): after a feature change, compiles the sets one toggle away (GI, SSGI, fog first) on half the cores. |
| Culling | GPU frustum, small-object (`eldr:cullPixels`, sun shadows `eldr:shadowCullPixels`), LOD, distance light cut-off. No Hi-Z occlusion. |
| GPU sharing | With an OpenGL Hgi, colour and depth skip the CPU (1080p 17.4 to 3.2 ms) |
| Numerics | NaN/Inf cleaned where they enter each history (TAA, SSGI, GI, exposure) |

Costly opt-ins on a 1080 Ti at 1280x720: `eldr:ssrDenoise` +9 ms (crowd), `eldr:rtShadows` +10 ms (Kitchen), `eldr:rtReflections` +15 ms (Kitchen), `eldr:gi` 2-3 ms, volumes 7-22 ms for a large cloud. They matter little for seconds-per-frame renders.

## USD authoring conventions

All optional, beyond standard UsdPreviewSurface, UsdLux and UsdGeom.

| Convention | On | Meaning |
|---|---|---|
| `bool primvars:eldr:reflectionProbe = 1` | mesh (typically an invisible Cube) | Reflection probe: never drawn, not in bounds or shadows; its world box is the influence and parallax volume, captured from its centre. Max 8, smallest first. Needs `eldr:reflectionProbe`. |
| `bool primvars:eldr:planarReflection = 1` | flat mesh | Planar mirror. Needs `eldr:planarReflections`. Without any, the largest flat glossy opaque mesh. |
| `bool primvars:eldr:hair = 0` | BasisCurves (constant) | Opt out of hair shading (cables, ropes). Unauthored or true: hair. |
| `bool primvars:eldr:ribbons` | BasisCurves | Overrides `eldr:curveRibbons` per curve |
| `bool primvars:eldr:sprites` | Points | Overrides `eldr:pointSprites` per prim |
| `float inputs:eldr:toonBands = 3`, `float inputs:eldr:toonSoftness = 0.05` | material surface shader | Toon bands per unit of light (0 = none), band edge width |
| `inputs:eldr:clearcoatColor` | UsdPreviewSurface shader | Coat tint (read as `coat_color`); constant or connected |
| `int primvars:eldr:refineLevel` | subdivision mesh | Its own refine level instead of the host's (usdview Complexity, Houdini Level of Detail); still at most `eldr:maxRefineLevel` |
| `int primvars:karma:object:holdoutmode = 1` | gprim | Holdout: black, zero alpha, keeps depth, ids, shadows. Mode 2 acts as matte. |
| `string primvars:karma:object:rendervisibility` | mesh, curves, points | Karma's ray visibility: `*`, `-primary` (phantom: unseen, still shadows and reflects), `-shadow`, `primary\|shadow`, `""` (unrenderable). Diffuse, reflect and refract are one class in Eldr (ray-traced reflections, GI, refraction); screen-space effects see what the camera sees. |
| `bool primvars:karma:object:mblur = 0` | gprim | No motion blur of its own (transform, deformation); camera motion still blurs it |
| `int primvars:karma:object:curve_style`, `point_style` | BasisCurves, Points | Karma's Render Curves As (0, 2 ribbons; 1 tubes; 3 the render setting) and Render Points As (0, 2 discs; 1 spheres). Oriented ribbons without normals and oriented discs face the camera. `eldr:ribbons`, `eldr:sprites` win. |
| `float primvars:karma:object:width_scale` | BasisCurves, Points | Multiplies the widths |
| `int primvars:karma:object:dicingdepthmax` | subdivision mesh | Caps the refine level (-1: no cap) |
| `collection:matte` | active UsdRenderPass | Collection members are holdouts (usdview: "Set As Active Render Pass"; `usdrecord --renderPassPrimPath`, needs the pass's `renderSource`). Whole gprims only. |
| UsdRenderSettings, RenderProduct, RenderVar | `/Render/...` | EXR products with AOVs and Cryptomatte; `sourceType` `lpe` and `primvar` honoured |
| `eldr:<setting>` | active UsdRenderSettings | Sets that render setting |
| `lensDistortion:k1,k2,k3,k4,k5,k6,p1,p2,center,anaSq,asym,scale`, `lensDistortion:type` (`standard`, `fisheye`) | camera (custom) | Lens warp; `k3`-`k6`, `p1`, `p2` are OpenCV terms |
| `karma:camera:material:binding`, `karma:camera:use_lensshader`, `karma:camera:lensshader` | camera | Karma Physical Lens `opencv_k1..k6`, `opencv_p1`, `opencv_p2` become the same distortion |
| `primvars:skel:skinningMethod = "dualQuaternion"` | skinned mesh | Dual-quaternion skinning (default linear blend) |
| `inputs:angle` | DistantLight | Sun angular diameter for soft shadows |
| `inputs:texture:file` | RectLight, DiskLight, DomeLight | Image on the light (left edge at +X, top row at +Y, as Karma) or the dome's latlong |
| `shadow:enable` | lights | Lights without it cast no shadows |
| `emissiveColor` (constant) | UsdPreviewSurface / standard_surface emission | Small bright emitters become lights |
| `fStop`, `focalLength`, `focusDistance`, `horizontalAperture`, `shutter:open`, `shutter:close` | UsdGeomCamera | Depth of field, motion blur |
| `wrapS/wrapT` | UsdUVTexture | repeat, mirror, clamp, black, useMetadata (default). MaterialX `image` `uaddressmode`/`vaddressmode`: periodic, mirror, clamp, constant. |
| `<UDIM>` asset paths | textures | Tiles 1001-1100 found by the resolver, also in usdz |
| Subdivision tags, GeomSubsets, `displayColor` | meshes | As UsdGeom defines |
| `fieldName`, `fieldIndex` and a MaterialX `volume` or Karma `kma_volume` material | UsdVolVolume with OpenVDBAsset | Fields by the material's `geompropvalue` names; without a material `density` (`eldr:volumeDefaultFields`: also `temperature`, `Cd`/`color`, `emission`) |

## Known limitations

| Area | Limitation |
|---|---|
| Output | All render products use the first raster product's framing; the collection is ignored. Vars Eldr cannot fill are left out with a warning. No `elementId` AOV. |
| Output | Cryptomatte ranks come from film samples in the 3x3 filter footprint; thin objects between samples can be missed. Interactive: one id per pixel. |
| Hydra | Volumes are not pickable; a field prim's own transform is ignored; OpenVDB assets only. |
| Hydra | Dome `poleAxis`, portals, shadow colour/distance and IES profiles are ignored. One frame in flight. |
| Lights | 63 lights per cluster (the rest are dropped); every cluster tests every light. |
| Lights | Textured lights use two taps, not a filtered polygon; fog, ray-traced reflections and emissive-as-light ignore the texture. |
| Lights | Emissive surfaces become lights only when constant, rigid, small and at most 32; others light via GI only. |
| Shadows | Sun: no blend between cascades; kernels clamp to 5% of a cascade. Local lights: redraw is all-or-nothing when a caster moves; kernels clamp at cube-face seams. |
| Fog | Single-tap shadows; the dome's light in fog is unoccluded without GI. |
| GI | Probes can't tell a single-sided wall from the inside of a closed object; meshes without normals use winding. |
| SSGI, SSR | No depth mip chain (large radii sample sparsely); SSR runs at full resolution with one ray and reflects only what is on screen. |
| Probes, planar | Probe reflections lack screen-space effects and volumes; CPU prefilter; no seam filtering. Planar: one sharp reflection for rough mirrors; coplanar mirrors don't share renders. |
| Ray tracing | Translucent meshes don't block rays (except tinting shadows and refraction); skinned instances capped; no BLAS compaction (about 2x memory); local lights unshadowed at hit points; closest-hit normal on displaced meshes is the rest triangle's. |
| Transparency | OIT and sorting are per mesh. Holdouts cut everything at the pixel, from constants or textures (not programs). Subsurface is a per-channel Gaussian (not Burley). |
| Refraction | Screen space outside final quality. Traced refraction follows only the nearest refracting surface's ray; skinned glass beyond `eldr:rtSkinnedInstances` isn't traced. Volume transparency splits only the nearest layer. |
| Materials | Unsupported graph nodes are bypassed (their pass-through input or default); Arnold or RenderMan volume shaders keep the constant. Volume graphs: Karma's Pyro Shader and Pyro Preview aren't read, nor blackbody of a ramp or remap, a colour field times a ramp, or more than three field functions. A field function that isn't 0 at 0 (a remap with `outlow` above 0, a ramp that starts above black) applies only where the density grid has data, not in its empty tiles; Karma's goes on to the volume's bounds. Catmull-Rom overshoot below 0 is clamped (Karma keeps negative emission). Volume shadow maps and traced shadows use the mean of coloured extinction; half-resolution volumes keep only the mean transmittance. A constant coefficient (no field read) counts per unit of density; Karma fills the active voxels uniformly. Gray textures in gamma colour spaces aren't decoded. No tangent primvars (frame follows uv derivatives). |
| Materials | Coat: no coat normal, anisotropy or SSR; no `coat_darkening`. Thin film assumes air above. Hair has no TT lobe; a closed curve's u jumps at its weld. |
| Materials | With a textured setting off, a material with a textured weight is still drawn in the translucent or subsurface pass (weight 0). |
| Displacement | Top mip only; vector displacement drops tangential parts; skinning stretch doesn't scale bump; `displacementEnabled` not read. |
| Textures | Past the table size they fall back to constants. A mesh carries up to two extra uv sets; a GeomSubset with a different first uv set reads the mesh's. |
| LOD | Border and seam vertices never move (many uv islands simplify little); meshes with over a million instances keep one coarser level. |
| Curves | World-space 6-sided tubes (12 triangles per step) unless ribbons. |
| Culling | No Hi-Z occlusion culling. |
| Camera | Box shutter, linear motion between frames. Film filter gather is 3x3 (enough to 1.5 px). Overscan renders at output resolution (centre loses up to 2x detail). Karma's curvature and aberration lens effects are not matched. |
| Sampling | Adaptive sampling rarely stops early on scenes with edges. |
| Post | Bloom doesn't spill onto the app's background colour. |
| Tooling | Prewarm handles single toggles only and prewarms sets without light splits. Old cache files stay on disk. Scene upload is one synchronous submit per commit; the volume grid pool uploads whole on any change. The Attribute Editor (usdview plugin) edits the session layer only. |
