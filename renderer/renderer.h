#pragma once

#include "aov.h"
#include "device.h"
#include "distortion.h"
#include "fog.h"
#include "gtao.h"
#include "lens.h"
#include "post.h"
#include "raytracing.h"
#include "scene.h"
#include "subsurface.h"
#include "ssr.h"
#include "taa.h"
#include "transparency.h"
#include "volume.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace vkr {

struct FrameData;
class Reflections;

class Renderer {
public:
    struct Frame {
        uint32_t width = 0;
        uint32_t height = 0;
        float viewProj[16];       // USD row-vector order, GL clip space
        float invViewProj[16];
        float cameraPos[3];       // world space
        float clearColor[4] = {0, 0, 0, 0};
        float clearDepth = 1.0f;
        bool showEnvironment = true;  // draw the dome texture as background
        bool postProcess = true;      // false: color is the raw linear HDR scene
        bool taa = true;              // with postProcess: jittered + temporally resolved
        bool ambientOcclusion = true; // GTAO on the dome (image-based) lighting
        float aoRadius = 0.5f;        // world units
        bool ssr = true;              // screen-space reflections (needs postProcess)
        bool shadows = true;          // the distant light's shadow cascades
        bool localShadows = true;     // local lights' shadow atlas
        bool softShadows = true;      // local lights' penumbrae from their size (PCSS); off: 3x3 PCF
        uint32_t taaSamples = 16;     // jitter positions a still camera accumulates
        float cullPixels = 0.5f;      // instances under this screen radius are skipped (0: none)
        // Always worth having, but each costs time: off to compare or to
        // speed up a heavy scene.
        bool depthPrepass = true;        // off: the main pass writes depth (no GTAO, no SSR)
        bool frustumCulling = true;      // off: every instance is drawn (small-object culling too)
        bool localLights = true;         // sphere, rect, disk and cylinder lights
        bool clusteredLights = true;     // off: every pixel loops over all local lights
        bool lightTextures = true;       // rect and disk lights' inputs:texture:file
        bool imageBasedLighting = true;  // the dome's diffuse and specular light
        bool normalMaps = true;
        bool materialGraphs = true;      // material programs (MaterialX graphs); off: plain inputs
        bool objectMotion = true;        // moving objects' motion vectors (TAA, motion blur); off: camera only
        bool translucency = true;        // off: translucent objects aren't drawn
        // Quality against speed.
        uint32_t shadowResolution = 2048;      // sun cascades, texels square (256..8192)
        uint32_t shadowCascades = kCascades;   // 1..kCascades
        uint32_t localShadowResolution = 512;  // a local light's atlas tile (64..kShadowAtlasSize / 2)
        uint32_t softShadowSamples = 16;       // PCSS blocker search and filter taps (1..16)
        uint32_t aoSlices = 3;                 // GTAO directions per pixel (1..8)
        uint32_t ssrSteps = 32;                // reflection ray march steps (1..256)
        uint32_t anisotropy = 16;              // material texture filtering (1 = off .. 16)
        // The camera's lens (UsdGeomCamera, world units), with postProcess:
        // depth of field when fStop > 0, motion blur while the shutter is open.
        bool depthOfField = true;
        bool motionBlur = true;
        float focalLength = 0.0f;
        float fStop = 0.0f;
        float focusDistance = 0.0f;
        float sensorWidth = 0.0f;     // horizontal aperture the projection shows
        float shutter = 0.0f;         // open interval in frames (shutter:close - shutter:open)
        float shutterOpen = 0.0f;     // shutter:open, frames relative to this one
        // Motion blur time samples (with motionBlur): under the film each
        // sample renders the scene at a time in the shutter interval, every
        // pass (shadows, rays) included, instead of the post blur. Motion
        // is linear from the last frame's to this one's (transforms, joints,
        // blend shape weights, camera), as the motion vectors are.
        bool motionBlurTimeSamples = true;
        // Sampled motion (the Hydra layer's eldr:sampledMotion): the motion
        // the blur and the time samples use is the scene's own through the
        // shutter, so a first frame blurs too. Meshes carry it as
        // prevTransforms (Mesh::shutterMotion); the camera one frame before
        // is here (viewProj and cameraPos when it doesn't move).
        bool sampledMotion = false;
        // Deformation blur (eldr:deformationBlur): meshes with
        // Mesh::velocities (point caches, sampled through the shutter)
        // move along them in time samples and motion vectors. Off: they
        // are ignored.
        bool deformationBlur = true;
        // Residual DOF (eldr:dofResidualBlur): with dofJitter the post DOF
        // still runs on each film sample, its circle of confusion scaled
        // by 1/sqrt(filmSamples), to fill the gaps between lens samples
        // (EEVEE's).
        bool dofResidualBlur = true;
        float prevViewProj[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float prevCameraPos[3] = {0, 0, 0};
        // Inverse projection's z and w columns over NDC z (row-vector order):
        // camera distance = -(x * z + y) / (z * z + w), z = window depth * 2 - 1.
        float depthToDistance[4] = {0, 0, 0, 0};
        PostProcess::Settings post;
        // Procedural textures: MaterialX noise in material programs (off: it reads 0).
        bool proceduralTextures = true;
        // Lens distortion: the camera's lensDistortion:* warp the finished
        // frame, ids and depth included (distortion.h); the caller renders
        // the view distortion.overscan times wider.
        bool lensDistortion = true;
        LensDistortion::Params distortion;
        // Blend shape motion: skinned meshes' motion vectors (TAA, motion
        // blur) include their blend shapes' change since the last frame.
        bool blendShapeMotion = true;
        // Extra AOVs (Targets::aovs): world to camera (Neye, Peye,
        // cameraDepth; row-vector order) and per primId its Cryptomatte
        // object and material ids (CryptomatteHash), 2 per id.
        bool extraAovs = true;
        // Light splits (Karma's convention): the dome's directly visible
        // light (its irradiance under the AO's or SSGI's visibility, its
        // reflection where nothing replaces it) counts as direct; off: as
        // indirect, with GI, probes and reflections.
        bool splitDomeDirect = true;
        // Blender 5.1's Light Path Intensity: multipliers of the direct and
        // indirect light in the final pixel (the light splits' division, the
        // dome per splitDomeDirect), not of the light transport. 1: off, no cost.
        float directIntensity = 1.0f, indirectIntensity = 1.0f;
        // Volumes also shadow surfaces from the local lights (their atlas tiles, through the tint
        // atlas; needs volumes.shadows and localShadows).
        bool localVolumeShadows = true;
        // The local lights' shapes (sphere, rect, disk, cylinder) show to the camera as emitters of their
        // radiance, unless the light is hidden from the camera (Light::cameraVisible).
        bool lightShapesVisible = false;
        float view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const uint32_t *cryptomatte = nullptr;
        uint32_t cryptomatteIds = 0;
        // GPU sharing (the Hydra layer's HdVkGLInterop, interop.h): colour
        // and depth go to the app's OpenGL on the GPU instead of Targets.
        bool shareFrame = true;
        // Render products (the Hydra layer's HdVkRenderProducts): the active
        // UsdRenderSettings' products are written as EXR files once converged.
        bool renderProducts = true;
        // eldr:productResolution: products render at the first raster
        // product's resolution, pixel aspect ratio and data window (the
        // Hydra layer's; the app's buffers get that image resampled).
        bool productResolution = true;
        // Also exposes the beauty without bloom or tonemapping for
        // Renderer::Read (Targets::linearColor): an EXR's linear beauty.
        bool linearColor = false;
        bool linearBeauty = true;  // eldr:productLinear: the Hydra layer sets linearColor for products
        // Material lobes beyond the metallic base (shaders/brdf.glsl); off:
        // materials render without them.
        bool specularWorkflow = true;  // UsdPreviewSurface specularColor as F0
        bool clearcoat = true;         // a second GGX lobe on top (coat)
        bool sheen = true;             // microfiber lobe (fabric, fuzz)
        bool thinFilm = true;          // thin-film iridescence on the specular
        bool anisotropicSpecular = true;  // highlights stretched along a tangent (brushed metal)
        bool toon = true;              // NPR toon bands of materials with their own (Material::toonBands)
        uint32_t toonBands = 0;        // with toon: bands for every other material too (0: none)
        bool hairShading = true;       // curves (Mesh::hair) with strand highlights
        bool texturedLobes = true;     // lobe inputs from textures and graphs (material programs); off: constants
        bool texturedTransmission = true;  // the same for transmission and subsurface inputs
        // Ray tracing (raytracing.cpp), ignored without Device::rayTracing.
        // With the depth prepass: traced shadows of the sun and the three
        // lights that matter most (no shadow maps for them; soft from the
        // sun's angle and the lights' size) and traced reflections (instead
        // of the dome's and SSR, up to roughness 0.7), both noisy per frame
        // for TAA to average. Translucent meshes only tint shadow rays
        // (rtTranslucentShadows); of the
        // skinned ones, the rtSkinnedInstances largest on screen do (posed
        // and refit every frame).
        bool rtShadows = false;
        uint32_t rtShadowSamples = 4;      // rays per light and pixel (1..16); final quality: 1 per film sample
        // Shadows of final quality's film runs (_filmFinal) without
        // rtShadows: shadow maps, traced (then the sun and up to 15 local
        // lights, not 3), or traced when likely cheaper (Renderer::Render's
        // rule: triangles per pixel and shadowed light). Interactive frames
        // follow rtShadows alone.
        enum class FinalShadows { Maps, Raytraced, Auto };
        FinalShadows finalShadows = FinalShadows::Maps;
        // Traced shadows go on through translucent casters (their colour,
        // as the tint maps') and through the volumes (with volume.shadows;
        // their transmittance, marched as volume_shadow.frag does); off:
        // translucent meshes cast none, volumes neither.
        bool rtTranslucentShadows = true;
        bool rtReflections = false;
        uint32_t rtReflectionSamples = 1;  // rays per pixel (1..16)
        uint32_t rtSkinnedInstances = 16;  // skinned instances that block rays (0..kMaxRtSkinned), for every ray-traced effect
        // Global illumination (ray-traced DDGI probes; ignored without
        // Device::rayTracing): the diffuse light bounced off the scene,
        // instead of the dome's unoccluded light.
        bool gi = false;
        uint32_t giProbes = 16;      // along the scene's longest side (2..64; at most 16384 in all)
        uint32_t giRays = 128;       // per probe and frame (16..512)
        float giHysteresis = 0.97f;  // weight of the probes' history (0..0.99): smoother, but slower to react
        // Screen-space GI (gtao.cpp; no ray tracing needed), with the depth
        // prepass: per-pixel indirect diffuse light from what's on screen,
        // instead of GTAO; the probes (gi) or the dome light the rest.
        bool ssgi = false;
        uint32_t ssgiSteps = 8;        // per direction and side (1..32)
        float ssgiRadius = 1.0f;       // world units
        uint32_t ssgiResolution = 2;   // traced at 1 / this (1, 2), denoised and upsampled
        // Emissive surfaces as lights: small meshes emitting at least the
        // threshold also light the scene as sphere lights (with shadows);
        // the caller applies these to the Scene (Scene::SetEmissiveLights).
        // GI picks up every emissive surface's light regardless.
        bool emissiveLights = true;
        float emissiveLightThreshold = 4.0f;  // scene-referred radiance of the brightest channel
        // The dome's sun (Scene::SetDomeSun, applied by the caller too): its
        // texels over the threshold (scene radiance, luminance) leave the
        // image-based light and become a distant light with shadows.
        bool domeSun = true;
        float domeSunThreshold = 10.0f;
        // Translucency (transparency.h): weighted blended OIT, its weights
        // falling with distance to this power (0: a plain average); off:
        // sorted per mesh, blended over.
        bool oit = true;
        float oitDepthWeight = 4.0f;
        // Refraction (needs depthPrepass): translucent materials with a
        // transmission or an ior bend and tint the opaque colour behind
        // them, rough ones blurred over this many taps (1..16).
        bool refraction = true;
        uint32_t refractionSamples = 8;
        // Translucent shadows: translucent casters tint and lighten the
        // sun's and local lights' shadows, through maps of 1 / this the
        // shadow maps' resolution (1..4).
        bool translucentShadows = true;
        uint32_t translucentShadowDivisor = 2;
        // Subsurface scattering (needs depthPrepass): the diffuse light of
        // subsurface materials blurred over this many taps per direction
        // (1..64), and the sun shining through their thin parts.
        bool subsurface = true;
        uint32_t subsurfaceSamples = 15;
        // Holdouts (Mesh::holdout): black with zero alpha; off: drawn as
        // usual.
        bool holdouts = true;
        // Soft sun shadows: penumbrae from the distant light's angle (PCSS
        // on the cascades), with softShadows on.
        bool softSunShadows = true;
        // Volumetric fog (fog.h): height fog lit by every light, over every
        // surface and the sky. Units are the scene's.
        bool fog = false;
        float fogDensity = 0.02f;       // extinction per unit at fogHeight
        float fogHeight = 0.0f;         // base height, along Y (or Z with fogZUp)
        float fogHeightFalloff = 0.0f;  // density * exp(-falloff * (height - fogHeight)); 0: uniform
        float fogAnisotropy = 0.6f;     // Henyey-Greenstein g: 0 even, towards 1 forward (shafts towards lights)
        float fogColor[3] = {1, 1, 1};  // scattering albedo
        bool fogGiAmbient = true;       // with GI: the dome's light in the fog from its probes (interiors: less)
        float fogMaxDistance = 0.0f;    // view depth the froxels reach (then constant); 0: the scene's far end
        uint32_t fogGridSize = 160;     // froxels across (16..512); rows by aspect, 64 slices
        bool fogZUp = false;            // heights along Z (Z-up stages)
        // Reflection probe and planar reflections (probes.h).
        bool reflectionProbe = false;    // one cube captured in the scene, before the dome (with imageBasedLighting)
        uint32_t probeResolution = 128;  // its faces' texels (16..512, a power of two)
        float probePosition[4] = {0, 0, 0, 0};  // where it is captured if w = 1, else the scene's centre
        bool planarReflections = false;  // the mirror mesh reflects the scene rendered mirrored
        float planarScale = 0.5f;        // that render's size, of the frame's (0.1..1)
        uint32_t planarCount = 4;        // mirrors rendered, the largest first (at most 4)
        bool probeNormalize = true;     // probes scaled by local (GI/SSGI) over captured ambient light
        bool authoredProbes = true;     // meshes flagged primvars:eldr:reflectionProbe are the probes (probes.h)
        bool skinning = true;            // false: skinned meshes keep the last frame's joints (reflection renders)
        // OpenVDB volumes, raymarched over the shaded image (VolumePass).
        bool volumes = true;
        VolumePass::Settings volume;
        // Displacement (Material::displacement): off draws the undisplaced
        // surfaces; the scale multiplies every material's offset.
        bool displacement = true;
        float displacementScale = 1.0f;
        // Displacement from shading graphs (run per vertex); off, those
        // materials don't displace.
        bool displacementGraphs = true;
        // Levels of detail (Scene builds them for heavy meshes): off draws
        // every mesh whole; a bias of 1 allows twice the screen error.
        bool lod = true;
        float lodBias = 0.0f;
        // A level of detail change crossfades over a few frames (dithered,
        // the history blends it) instead of popping.
        bool lodFade = true;
        // Film accumulation (with taa; TemporalAA::RecordFilm): once the
        // camera and the scene have been still for kFilmStillFrames, and
        // from the first sample of a final frame, the samples are averaged
        // without TAA's reprojection and clipping, each through a Gaussian
        // pixel filter of filmFilterRadius pixels, until filmSamples
        // (IsConverged). Frames with motion blur stay TAA's.
        bool filmAccumulation = true;
        uint32_t filmSamples = 64;
        // Upscaling (TAAU), interactive only: under 1, while TAA resolves
        // (the camera or the scene moving; still too without the film) the
        // scene renders at this fraction of the width and height (at least
        // 0.25) and TAA reconstructs the full image into its full-size
        // history. The film and final quality always render at full size,
        // as do frames with depth of field, a motion-blur shutter, lens
        // distortion or extra AOVs (their passes are full size).
        float renderScale = 1.0f;
        float filmFilterRadius = 1.5f;
        // Final quality (offline renders): the film starts over at the
        // first sample after any change instead of from the TAA image.
        bool finalQuality = false;
        // Changes whenever the app's scene or settings changed (the Hydra
        // layer: change tracker and settings versions): the film starts over.
        uint64_t sceneVersion = 0;
        // Sample dimensions: every stochastic effect (AO, SSGI, SSR and
        // traced reflections' filter, soft shadows, refraction blur,
        // volumes) turns blue noise by a Halton value of its own per sample
        // (frame.glsl's SampleNoise), so the film converges without screen
        // patterns; off: interleaved gradient noise shifted per frame.
        bool sampleDimensions = true;
        // While TAA resolves (the camera or the scene moves), the sample
        // dimensions loop over their first this many samples (1..256).
        uint32_t interactiveSamples = 8;
        // Adaptive sampling (the film): a pixel whose mean's standard error
        // (exposed, compressed luma: about the displayed value) is under
        // adaptiveThreshold after adaptiveMinSamples takes no more samples,
        // and the film is converged once no pixel is left (or at
        // filmSamples, the most). 0: off. Edges' anti-aliasing is variance
        // too, so most scenes still run to filmSamples; worth it for large
        // flat areas (try 0.002).
        float adaptiveThreshold = 0.0f;
        uint32_t adaptiveMinSamples = 16;
        // Final quality (finalQuality, or a film that started fresh, as
        // usdrecord's): adaptive sampling at this threshold when
        // adaptiveThreshold is 0 (0: off). And the film is converged once
        // this fraction of the pixels has stopped (the rest of the noisy
        // ones would only run it to filmSamples).
        float adaptiveFinalThreshold = 0.01f;
        float adaptivePixelFraction = 0.999f;
        // GI preconverge: with gi, the film waits until fresh probes have
        // settled (RayTracing::Converged), so no sample sees half-filled ones.
        bool giPreconverge = true;
        // Fog under the film: every froxel sampled at a point jittered per
        // sample (sample dimensions) and no history, so the film's average
        // is the medium's (EEVEE: no reprojection when accumulating).
        bool fogFilmJitter = true;
        // Shading pipelines a frame compiles are saved to the device's
        // pipeline cache file (Device::LoadPipelineCache) for later runs.
        bool pipelineCache = true;
        // Compile the shading sets of this frame's features with one of
        // them toggled on a worker thread, so turning one on or off later
        // doesn't stall (Hydra's default: on).
        bool pipelinePrewarm = false;
        // Refracting meshes' back faces (needs refraction): seen from inside
        // the medium (water from below, the far side of a glass ball) with
        // the inverted ior and total internal reflection, drawn before their
        // front faces; off: back faces are discarded.
        bool refractionBackFaces = true;
        // Refraction through several layers (needs refraction): refracting
        // meshes back to front, the opaque colour copied again after each
        // (glass behind glass, a ball's far side), up to this many copies.
        bool multiRefraction = true;
        uint32_t refractionLayers = 4;
        // Ray-traced refraction (needs refraction and Device::rayTracing):
        // the front refracting surface's refracted ray is traced through
        // the glass (back faces, total internal reflection, glass behind
        // glass) up to rtRefractionBounces interfaces and shaded at its hit
        // as traced reflections are, so what is off screen or hidden shows.
        // 0 off, 1 in final quality (film runs started fresh), 2 always.
        uint32_t rtRefraction = 1;
        uint32_t rtRefractionBounces = 4;  // 1..16
        // Translucent holdouts (with holdouts) cut the image's colour and
        // alpha by their opacity; off: they draw nothing.
        bool holdoutAlpha = true;
        // Brightness clamps against fireflies (EEVEE's light clamps): the
        // brightest channel of a pixel's direct light (sun, local lights),
        // and of every bounce light comes in by (SSGI's input, SSR and
        // traced reflection hits, GI probe rays; not the dome), scaled down
        // to at most this scene-referred radiance. 0: off.
        float directClamp = 0.0f;
        float indirectClamp = 10.0f;
        // Thickness estimator in SSR and SSGI (EEVEE's): steps spread
        // quadratically, each at least a pixel on, objects at least a pixel's
        // footprint thick (deep at grazing angles) and a ray that crosses a
        // continuous surface hits it whatever its thickness. Off: the ray
        // steps evenly and any object is 5% of its distance thick.
        bool ssgiThickness = true;
        // SSGI with a visibility bitmask (EEVEE's fast GI) instead of the
        // highest horizon: 32 sectors per slice, objects
        // ssgiBitmaskThickness of ssgiRadius thick, light from the sectors nothing nearer hides,
        // weighted by how much the surface it comes from faces the pixel.
        bool ssgiBitmask = false;
        float ssgiBitmaskThickness = 0.25f;  // the bitmask's object thickness, in ssgiRadius
        // Back-face hits in SSR and SSGI (Blender 5.2's Backface option): a
        // ray that meets the visible surface from behind (it went behind an
        // object) takes its radiance times screenBackfaceScale (0: none,
        // 1: as a front face). Off: SSR counts them fully, SSGI's bitmask
        // takes none and its horizon scan all, as before.
        bool screenBackface = true;
        float screenBackfaceScale = 0.25f;
        // Half-resolution SSGI traces a different pixel of each 2 x 2 tile
        // every frame, in Morton order (EEVEE's), so the accumulation sees
        // all four. Off: always the tile's first, as before.
        bool ssgiTileOrder = true;
        // SSR hits read last frame's image at the mip as wide as the ray's
        // cone there (EEVEE 5.2): glossy reflections without sparkle. The
        // history then keeps mips (6 levels, blitted each frame). Off: one
        // texel, sharp at any roughness.
        bool ssrHitFilter = true;
        // Ray tracing sees displacement (with displacement on): displaced
        // meshes' BLASes are built from their displaced vertices (rebuilt
        // when the scale changes). Off: from their rest vertices.
        bool rtDisplacement = true;
        // Denoised glossy SSR (ssr.h, with ssr; not with rtReflections):
        // one GGX-sampled ray per pixel, faded out from roughness 0.5 to 0.8,
        // through a ratio estimator, temporal reprojection through the
        // reflected point and a variance-guided bilateral filter. Off: one
        // mirror ray, faded out from roughness 0.3 to 0.6.
        bool ssrDenoise = false;
        // Jittered light shapes under the film (EEVEE's jittered soft
        // shadows): each sample renders the shadow maps from a point of the
        // light (the sun's disc, a rect's or disk's area, inside a
        // sphere) and looks them up hard (3x3 PCF), so the film's average
        // is the true penumbra; with softShadows (and softSunShadows for
        // the sun). Only in final quality (the film run started fresh or
        // with finalQuality); TAA frames and interactive films keep PCSS.
        bool shadowJitter = true;
        // Lens jitter under the film (EEVEE's DOF jitter): with depth of
        // field, each sample sees through a point of the lens aperture
        // (the focus plane fixed), so the film's average is the thin lens's
        // blur, with true occlusion; the post DOF is skipped then. TAA
        // frames keep the post DOF.
        bool dofJitter = true;
    };

    // Still frames before the film takes over from TAA (EEVEE's 3).
    static constexpr uint32_t kFilmStillFrames = 3;

    // Host memory a frame is copied to, bottom row first (GL/Hydra order).
    // Null pointers skip that output's readback.
    struct Targets {
        void *color = nullptr;          // RGBA16F, 8 bytes per pixel
        void *linearColor = nullptr;    // Read only: Frame::linearColor's (else as color)
        float *depth = nullptr;         // window depth in [0, 1]
        int32_t *primId = nullptr;      // Mesh::primId, -1 where nothing was drawn
        int32_t *instanceId = nullptr;  // index into Mesh::transforms, -1 likewise
        uint16_t *velocity = nullptr;   // object motion vectors, two halves per pixel (see mesh.frag)
        // Extra AOVs (aov.h), packed floats; with Frame::extraAovs.
        float *aovs[Aovs::kCount] = {};
        uint8_t aovComponents[Aovs::kCount] = {};  // floats per pixel of each (0: Aovs::Components)
    };

    static constexpr VkFormat kColorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kIdFormat = VK_FORMAT_R32_SINT;
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    static constexpr VkFormat kVelocityFormat = VK_FORMAT_R16G16_SFLOAT;
    static constexpr uint32_t kCascades = 4;
    static constexpr uint32_t kMaxRtSkinned = 256;  // Frame::rtSkinnedInstances
    // Shadow atlas for sphere/rect/disk lights, in square tiles
    // (Frame::localShadowResolution).
    static constexpr uint32_t kShadowAtlasSize = 4096;

    explicit Renderer(Device &device);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    // Draws `scene` and blocks until the requested outputs are in `out`.
    void Render(const Scene &scene, const Frame &frame, const Targets &out);

    // False while auto-exposure adapts or TAA is still accumulating a still
    // camera: keep rendering frames.
    bool IsConverged() const;

    // Instances that survived culling in the last frame (main view).
    uint32_t VisibleInstances() const;

    // What the last Render measured.
    struct Stats {
        double cpuMs = 0;  // Render's wall time: recording, the GPU's work, readback
        double gpuMs = 0;  // the frame on the GPU; 0 without timestamps
        // GPU milliseconds per pass, in the order they ran (see Render).
        std::vector<std::pair<const char *, double>> passMs;
        uint64_t triangles = 0;   // the camera drew, after culling
        uint32_t instances = 0;   // the camera drew, after culling
        uint32_t drawCalls = 0;   // the camera's indirect calls (each covers many meshes)
        uint32_t taaSamples = 0;  // frames in the TAA average (taaTarget: converged)
        uint32_t sample = 0;      // the sample dimensions' sample (0: no TAA)
        uint32_t filmActivePixels = 0;  // adaptive film: pixels still taking samples
        uint32_t taaTarget = 0;
        double exposure = 1;      // the tonemap's multiplier of scene radiance
        uint64_t accelerationStructureBytes = 0;  // ray tracing's graphics memory (in Device's too)
        uint32_t prewarmedSets = 0;  // shading sets taken from Frame::pipelinePrewarm's worker, so far
        uint32_t renderWidth = 0, renderHeight = 0;  // the scene's size (Frame::renderScale), else the frame's
    };
    const Stats &GetStats() const { return _stats; }
    // Blocks until Frame::pipelinePrewarm's worker has nothing left to do.
    void WaitForPrewarm();

    // GPU sharing (interop.h): the last frame's images, in
    // TRANSFER_SRC_OPTIMAL until the next Render: colour as Targets::color
    // has it (RGBA16F) and depth (D32); null before a frame.
    VkImage ColorImage() const { return _frameColor; }
    VkImage DepthImage() const { return _frameColor ? (_upscaled ? _up[0].image : _depth.image) : VK_NULL_HANDLE; }
    uint32_t Width() const { return _outWidth; }
    uint32_t Height() const { return _outHeight; }
    // Copies the last frame's outputs (as Render's, extra AOVs aside) to
    // host memory, for consumers that ask after Render.
    void Read(const Targets &out);
    // The last frame's colour target: raw HDR, premultiplied (with
    // Frame::postProcess off), in TRANSFER_SRC_OPTIMAL. Planar reflections
    // sample another renderer's.
    VkImage HdrColorImage() const { return _color.image; }
    VkImageView HdrColorView() const { return _color.view; }

private:
    void _CreateDescriptors();
    // A shading set's pipelines: per pass kVariants of them.
    struct ShadingTargets {
        VkPipeline *opaque, *translucent, *oit, *subsurface, *refractBack;
    };
    void _CreatePipelines(bool displaced = false, uint32_t features = kNone, const ShadingTargets *shading = nullptr,
                          VkPipelineCache cache = VK_NULL_HANDLE);
    uint32_t _ShadingSet(uint32_t features);
    ShadingTargets _SetTargets(uint32_t set)
    {
        const uint32_t base = set * kVariants;
        return {_pipeline + base, _translucentPipeline + base, _oitPipeline + base, _subsurfacePipeline + base,
                _refractBackPipeline + base};
    }
    void _Prewarm(uint32_t features, const Scene::Gpu &gpu, bool save);
    void _PrewarmWorker();
    void _MergePrewarmCache();
    void _CreateConstantImages();
    void _Resize(uint32_t width, uint32_t height);
    void _ResizeScene(uint32_t width, uint32_t height);
    void _CreateShadowMap(uint32_t size);
    void _UpdateDescriptors(const Scene::Gpu &gpu, bool prepass, VkImageView background);
    bool _FitCascades(const Scene::Gpu &gpu, const Frame &frame, const float toLight[3], FrameData *data) const;
    void _FitClusters(const Scene::Gpu &gpu, const Frame &frame, FrameData *data) const;
    bool _SetupFog(const Scene::Gpu &gpu, const Frame &frame, FrameData *data);
    struct ShadowTile;
    std::vector<ShadowTile> _PlanLocalShadows(const Scene::Gpu &gpu, const Frame &frame, float cutoff,
                                              uint32_t *rtLights, uint32_t rtSlots, bool tileTraced,
                                              const float *shapeSample);
    void _RenderLocalShadows(const Scene::Gpu &gpu, const std::vector<ShadowTile> &tiles, bool tint,
                             const VolumePass::Settings *volume);
    void _DrawTintCasters(const Scene::Gpu &gpu, VkDeviceSize commands);
    void _RenderTranslucentDepth(const Scene::Gpu &gpu, const float *viewProj);
    void _RenderSubsurface(const Scene::Gpu &gpu, const Frame &frame, const float *viewProj,
                           const float *invViewProj, VkRenderingAttachmentInfo depth, bool splits);
    void _RenderTranslucency(const Scene::Gpu &gpu, const Frame &frame, FrameData *data, const float *viewProj,
                             uint32_t motion, VkRenderingAttachmentInfo depth);

    Device &_dev;
    Gtao _gtao;
    TemporalAA _taa;
    Lens _lens;
    PostProcess _post;
    Transparency _transparency;
    Subsurface _subsurface;
    SsrDenoise _ssr;
    bool _ssrHistory = false;  // the denoiser ran last frame
    Fog _fog;
    float _fogDepth = 0;  // last frame's froxel depth (Frame::fogMaxDistance or the scene's)
    uint64_t _fogKey = 0; // last frame's lighting and medium (history is kept while they stay)
    std::unique_ptr<Reflections> _reflections;  // made when a frame first asks for them
    // TAA state: frames in the history, jitter sequence, last camera.
    uint32_t _taaFrames = 0, _haltonIndex = 0;
    float _prevViewProj[16] = {};  // camera of the frame in the TAA history
    bool _taaValid = false;
    bool _historyValid = false;    // the TAA history holds last frame's image
    bool _taaWarmup = false;  // accumulating a fresh history for a still camera
    bool _taaConverged = true;
    // Film: samples in it (0: TAA resolves), frames the camera and the
    // scene have been still, what the scene was.
    uint32_t _filmSamples = 0, _stillFrames = 0;
    bool _filmAdaptive = false;
    bool _filmFinal = false;  // the film run started in final mode (fresh, or finalQuality)
    bool _freshRun = false;
    bool _filmFroxels = false;  // this frame's fog: jittered per froxel, no history (Frame::fogFilmJitter)  // nothing changed since a fresh history: a film then starts unseeded  // the last frame's film counted its pixels still sampling
    uint64_t _filmKey = 0, _filmRuns = 0;  // _filmRuns: films started (the AOVs' accumulation follows)
    std::chrono::steady_clock::time_point _lastFrame;
    bool _hasLastFrame = false;
    bool _adapting = false;       // last frame ran auto-exposure
    bool _lastAutoExposure = false;
    VkSampler _samplers[kWrapModes * kWrapModes] = {};  // materials: trilinear, aniso, per wrap s and t
    VkSampler _clampSampler = VK_NULL_HANDLE;   // LUT, environment cube
    VkSampler _shadowSampler = VK_NULL_HANDLE;  // depth compare
    VkSampler _minSampler = VK_NULL_HANDLE;     // minimum of the bilinear footprint (Device::samplerMinmax)
    VkDescriptorSetLayout _setLayout = VK_NULL_HANDLE;
    VkDescriptorPool _descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    VkPipelineLayout _layout = VK_NULL_HANDLE;
    // Mesh pipelines in variants (Scene::Gpu::chunkKind >> 2): bit 0 eight
    // skinning influences, bit 1 dual-quaternion skinning (mesh.vert's
    // kWideSkin, kDualQuaternion); the shading ones also bit 2, a material
    // program (the GRAPH shaders), and bit 3 with it, one with noise
    // (program.glsl's kProcedurals); bit 4, lobes (the LAYERS shaders); bit
    // 5 displaced materials (every pass: displace.glsl's kDisplacement),
    // made once a scene has some (_displacedPipelines). Depth passes use the
    // kDepthVariants bits.
    static constexpr uint32_t kVariants = 64, kDepthVariants = 1 | 2 | 32;
    // The shading pipelines come in sets, each specialized with the optional
    // features (frame.glsl's kCompiledFeatures) _shadingSets has for it, its
    // variants at set * kVariants + variant; a frame draws with the set of
    // its own features (_shadingBase), made when a frame first needs it.
    static constexpr uint32_t kShadingSets = 16;
    std::vector<uint32_t> _shadingSets;
    uint32_t _shadingBase = 0;
    // Frame::pipelinePrewarm: the worker compiles the queued feature sets
    // into its own pipeline cache (merged into the device's after each) and
    // leaves them in _prewarmed for _ShadingSet to take; a set it is on
    // when a frame needs it is waited for, a queued one is taken back.
    struct PrewarmJob {
        uint32_t features;
        bool displaced;  // with the displaced variants too
    };
    struct Prewarmed {
        PrewarmJob job;
        std::vector<VkPipeline> pipelines;  // 4 * kVariants: opaque, translucent, OIT, subsurface
    };
    std::thread _prewarmThread;
    std::mutex _prewarmMutex;
    std::condition_variable _prewarmChanged;
    std::vector<PrewarmJob> _prewarmQueue;
    uint32_t _prewarmCurrent = kNone;  // the features being compiled
    std::vector<Prewarmed> _prewarmed;
    std::atomic<bool> _prewarmStop{false};
    bool _prewarmSave = false;         // Frame::pipelineCache when queued
    bool _prewarmMerge = false;        // the worker compiled something not yet merged into the device's cache
    uint32_t _prewarmFor = kNone;      // the features the queue was made around
    VkPipelineCache _prewarmCache = VK_NULL_HANDLE;
    VkPipeline _prepassPipeline[kVariants] = {};      // depth only
    VkPipeline _translucentPipeline[kShadingSets * kVariants] = {};  // shading, blended over the opaque
    VkPipeline _oitPipeline[kShadingSets * kVariants] = {};  // shading, OIT accumulation (Transparency)
    // Refracting meshes' back faces, seen from inside (transparency.glsl's
    // kShadingPass 3), blended as _translucentPipeline.
    VkPipeline _refractBackPipeline[kShadingSets * kVariants] = {};
    // Translucent shadow casters into Transparency's tint maps: their
    // nearest depth, then their colours multiplied (shadow_tint.frag).
    VkPipeline _tintDepthPipeline[kVariants] = {}, _tintColorPipeline[kVariants] = {};
    VkPipeline _mattePipeline[kVariants] = {};  // translucent holdouts' matte (matte.frag)
    VkPipeline _subsurfacePipeline[kShadingSets * kVariants] = {};  // shading, the subsurface lobe (Subsurface)
    // Cutout materials: prepass and shadows with mask.frag's alpha test.
    VkPipeline _prepassCutoutPipeline[kVariants] = {}, _shadowCutoutPipeline[kVariants] = {};
    VkPipeline _pipeline[kShadingSets * kVariants] = {};  // shading, depth read-only
    VkPipeline _shadowPipeline[kVariants] = {};
    bool _displacedPipelines = false;
    bool _savePipelines = true;  // Frame::pipelineCache
    VkPipeline _skyPipeline = VK_NULL_HANDLE;
    VkPipeline _cullPipeline = VK_NULL_HANDLE;
    VkPipeline _lightsPipeline = VK_NULL_HANDLE;    // light cluster assignment
    VkPipeline _skinPipeline = VK_NULL_HANDLE;      // joint matrices from skinning transforms
    VkPipeline _shutterPipeline = VK_NULL_HANDLE;   // motion blur time samples (shutter.comp)
    Buffer _clusters;                               // light lists, see shaders/lights.glsl
    Buffer _commands;               // culled indirect commands, one per mesh slot
    // Last frame's joint matrices for motion vectors, copied before skin.comp
    // rewrites them; usable while the joint pool is the same buffer.
    Buffer _prevJoints;
    Buffer _lodState;  // the camera's level of detail per instance slot (cull.comp)
    uint64_t _lodScene = 0;                   // the scene _lodInstances belongs to
    VkBuffer _lodInstances = VK_NULL_HANDLE;  // the instance pool _lodState is about
    uint32_t _lodFadeLeft = 0, _lodDrawCount = 0;  // frames crossfades may still run; commands when counted
    float _lodViewProj[16] = {};  // the camera the atlas last drew levels of detail for
    uint32_t _lodFrame = 1;       // frames with levels of detail (cull.comp's lodState); not 0: a cleared state would count as drawn last frame
    VkBuffer _prevJointsSource = VK_NULL_HANDLE;
    uint32_t _lastDrawCount = 0;
    VkCommandPool _pool = VK_NULL_HANDLE;
    VkCommandBuffer _cmd = VK_NULL_HANDLE;
    VkFence _fence = VK_NULL_HANDLE;
    // GPU timestamps: one at the start, then one closing each pass.
    static constexpr uint32_t kMaxTimestamps = 32;
    VkQueryPool _timestamps = VK_NULL_HANDLE;  // null: the queue has no timestamps
    double _timestampMs = 0;                   // milliseconds per tick
    std::vector<const char *> _passes;         // the pass each timestamp after the first closes
    Stats _stats;
    Image _color, _primId, _instanceId, _depth;
    Image _velocity;  // motion vectors (RG16F, uv), read by TAA
    // Film: primId, instanceId and depth of the sample nearest the pixel
    // centre so far (GENERAL; made with the first film frame), and its
    // distance from the centre (the jitter is the same for every pixel).
    Image _filmIds[3];
    float _filmIdDistance = 0.0f;
    Image _shadowMap;                           // kCascades-layer array view
    // The cascades' nearest depths for soft sun shadows (shadow_min.comp):
    // level 0 per 4 x 4 texels, mips to 1 x 1, in GENERAL; a view per level.
    static constexpr uint32_t kMaxShadowMinLevels = 12;
    Image _shadowMin;
    std::vector<VkImageView> _shadowMinLevels;
    VkPipeline _shadowMinPipeline = VK_NULL_HANDLE;
    uint32_t _shadowSize = 0;                   // its width and height (Frame::shadowResolution)
    float _anisotropy = 0;                      // _samplers' (Frame::anisotropy)
    Image _shadowAtlas;                         // local lights, see _PlanLocalShadows
    Buffer _lightShadows, _shadowTiles;         // per light: first tile or kNone; per tile: ShadowTile
    // What the atlas holds: redrawn only when the scene or the tiles change.
    uint64_t _atlasVersion = 0;
    std::vector<float> _atlasKey;               // the tiles' matrices and rects
    VkImageView _shadowLayers[kCascades] = {};  // per-cascade render targets
    Image _brdfLut;
    Image _blackCube;                           // bound when there is no dome
    Image _blueNoise;                           // RGBA8, frame.glsl's SampleNoise
    Buffer _readback, _frameData;
    uint32_t _width = 0, _height = 0;        // the scene's targets (Frame::renderScale)
    uint32_t _outWidth = 0, _outHeight = 0;  // the frame's: TAA's resolve and history, post, outputs
    // Upscaling: depth, primId, instanceId and velocity blitted (nearest)
    // to the frame's size for the outputs; whether the formats can be.
    bool _upscaled = false;
    int _upscaleBlit = -1;  // -1: not checked yet
    Image _up[4];
    LensDistortion _distortion{_dev};
    // Blend shape motion: last frame's blend shape weights, copied after
    // each frame's draws from the scene's pool (_prevBlendSource), which
    // the next frame's motion vectors read while it is the same buffer.
    // ponytail: a mesh whose weights moved within the pool (its shape count
    // changed) reads another mesh's for one frame.
    Buffer _prevBlendWeights;
    VkBuffer _prevBlendSource = VK_NULL_HANDLE;
    bool _prevBlendValid = false;
    // Motion blur time samples (Frame::motionBlurTimeSamples): what the
    // last motion left of the frame before it (shutter.comp's rigid rows per
    // instance slot, joints, blend shape weights; the camera), kept while
    // the film samples the shutter, for the scene's pools and version it
    // was taken of; and this frame's pools, saved to be put back.
    struct Shutter {
        bool valid = false;
        bool joints = false, weights = false;  // joints / weights move too
        uint64_t version = 0;
        VkBuffer instances = VK_NULL_HANDLE, jointPool = VK_NULL_HANDLE, weightPool = VK_NULL_HANDLE;
        uint32_t instanceCapacity = 0;
        float viewProj[16] = {};
        float cameraPos[3] = {};
    } _shutter;
    Buffer _shutterRows, _shutterJoints, _shutterWeights, _savedInstances, _savedJoints, _savedWeights;
    bool _shutterMoved = false;     // the last frame rendered the scene at a shutter time
    float _prevCameraPos[3] = {};   // with _prevViewProj
    Aovs _aovs{_dev};  // extra AOVs
    VkImage _frameColor = VK_NULL_HANDLE;  // GPU sharing: ColorImage()
    VkImage _linearColor = VK_NULL_HANDLE;  // Frame::linearColor's image, else null
    std::unique_ptr<RayTracing> _rt;  // null without Device::rayTracing
    std::unique_ptr<VolumePass> _volumePass;  // needs _setLayout
};

} // namespace vkr
