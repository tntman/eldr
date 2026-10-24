#include "probes.h"

#include "ibl.h"
#include "vkmath.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace vkr {

namespace {

Mat4 ToMat4(const float *m)
{
    Mat4 r;
    std::memcpy(r.m, m, sizeof(r.m));
    return r;
}

float HalfToFloat(uint16_t h)
{
    const int exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
    const float sign = (h & 0x8000) ? -1.0f : 1.0f;
    if (exponent == 0) {
        return sign * std::ldexp(float(mantissa), -24);
    }
    if (exponent == 31) {
        return mantissa ? 0.0f : sign * 65504.0f;
    }
    return sign * std::ldexp(float(mantissa | 0x400), exponent - 25);
}

// Vulkan's cube faces (+X, -X, +Y, -Y, +Z, -Z): the direction through the
// face's centre and those its s and t coordinates grow along (ibl.cpp's
// CubeDir).
const Vec3 kFaces[6][3] = {
    {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}}, {{-1, 0, 0}, {0, 0, 1}, {0, -1, 0}},
    {{0, 1, 0}, {1, 0, 0}, {0, 0, 1}},   {{0, -1, 0}, {1, 0, 0}, {0, 0, -1}},
    {{0, 0, 1}, {1, 0, 0}, {0, -1, 0}},  {{0, 0, -1}, {-1, 0, 0}, {0, -1, 0}},
};

// A 90 degree camera at `eye` whose NDC (x, y) is the face's (s, t) * 2 - 1,
// so the image's rows (bottom first) are the face's rows.
Mat4 FaceViewProj(int face, Vec3 eye, float n, float f)
{
    const Vec3 forward = kFaces[face][0], s = kFaces[face][1], t = kFaces[face][2];
    Mat4 view;
    const Vec3 columns[3] = {s, t, forward * -1.0f};
    for (int c = 0; c < 3; ++c) {
        view.m[0][c] = columns[c].x;
        view.m[1][c] = columns[c].y;
        view.m[2][c] = columns[c].z;
        view.m[3][c] = -Dot(eye, columns[c]);
    }
    return view * Perspective(1.0f, n, f);
}

// A frame of the second renderer: `frame`'s lighting and quality settings
// without the screen-space and temporal work or reflections of its own.
// Skinned meshes keep the joints the last frame left: their reflections
// lag a frame, and this frame's motion vectors stay right.
Renderer::Frame Nested(const Renderer::Frame &frame, uint32_t width, uint32_t height, const Mat4 &viewProj, Vec3 eye)
{
    Renderer::Frame f = frame;
    f.width = width;
    f.height = height;
    std::memcpy(f.viewProj, viewProj.m, sizeof(f.viewProj));
    const Mat4 inv = Inverse(viewProj);
    std::memcpy(f.invViewProj, inv.m, sizeof(f.invViewProj));
    f.cameraPos[0] = eye.x;
    f.cameraPos[1] = eye.y;
    f.cameraPos[2] = eye.z;
    f.postProcess = f.taa = f.ssr = f.ambientOcclusion = f.depthOfField = f.motionBlur = f.motionBlurTimeSamples = false;
    f.reflectionProbe = f.planarReflections = false;
    f.lensDistortion = f.extraAovs = false;
    // ponytail: reflections are shaded with shadow maps and the dome's
    // light; ray tracing and GI there would need their own TLAS and probes.
    f.rtShadows = f.rtReflections = f.gi = false;
    // ponytail: no volumes in reflections; their stochastic march needs
    // TAA, which these renders don't run.
    f.volumes = false;
    f.skinning = false;
    f.showEnvironment = true;  // reflections see the dome even where the camera doesn't
    std::fill(f.clearColor, f.clearColor + 4, 0.0f);
    f.clearDepth = 1.0f;
    return f;
}

} // namespace

Reflections::Reflections(Device &device)
    : _dev(device)
{
}

Reflections::~Reflections()
{
    vkDeviceWaitIdle(_dev.device);
    _dev.Destroy(_probe);
}

VkImage Reflections::PlanarImage() const
{
    return _planar ? _renderer->HdrColorImage() : VK_NULL_HANDLE;
}

VkImageView Reflections::PlanarView() const
{
    return _planar ? _renderer->HdrColorView() : VK_NULL_HANDLE;
}

void Reflections::Update(const Scene &scene, const Renderer::Frame &frame)
{
    const Scene::Gpu &gpu = scene.GetGpu();
    probeMs = planarMs = 0;
    ++_updates;
    _pending = false;
    if (frame.reflectionProbe && frame.imageBasedLighting && gpu.hasBounds) {
        uint32_t size = 16;
        while (size < std::min(frame.probeResolution, 512u)) {
            size *= 2;
        }
        _stableFrames = gpu.version == _lastVersion ? _stableFrames + 1 : 0;
        _lastVersion = gpu.version;
        ++_sinceCapture;
        // Lights, materials, the dome, the size or the place changed: capture
        // now; geometry: once it stopped changing, or now and then while it
        // keeps changing.
        const Vec3 lo{gpu.boundsMin[0], gpu.boundsMin[1], gpu.boundsMin[2]};
        const Vec3 hi{gpu.boundsMax[0], gpu.boundsMax[1], gpu.boundsMax[2]};
        const Vec3 at = frame.probePosition[3] == 1.0f
            ? Vec3{frame.probePosition[0], frame.probePosition[1], frame.probePosition[2]} : (lo + hi) * 0.5f;
        auto lit = [&] {
            return _probe.view && size == _probeSize && gpu.lightingHash == _probeLighting &&
                   at.x == _probePosition[0] && at.y == _probePosition[1] && at.z == _probePosition[2];
        };
        if (!(lit() && gpu.version == _probeVersion) &&
            (!lit() || _stableFrames >= 2 || _sinceCapture >= kRecaptureFrames)) {
            _Capture(scene, frame, size, at);
        }
        _pending = !(lit() && gpu.version == _probeVersion);
    } else if (_probe.view) {
        _dev.Destroy(_probe);
        _probeSize = 0;
    }
    const bool mirror = frame.planarReflections && gpu.mirrorSlot != kNone;
    _planar = mirror && _RenderPlanar(scene, frame);
    if (!frame.reflectionProbe && !mirror) {
        _renderer.reset();  // its targets and shadow maps are the bulk of the memory
    }
}

void Reflections::_Capture(const Scene &scene, const Renderer::Frame &frame, uint32_t size, Vec3 eye)
{
    const Scene::Gpu &gpu = scene.GetGpu();
    if (!_renderer) {
        _renderer = std::make_unique<Renderer>(_dev);
    }
    const Vec3 lo{gpu.boundsMin[0], gpu.boundsMin[1], gpu.boundsMin[2]};
    const Vec3 hi{gpu.boundsMax[0], gpu.boundsMax[1], gpu.boundsMax[2]};
    const float extent = std::max(Length(hi - lo), 1e-3f);
    const float n = extent * 1e-4f, f = extent * 2.0f + Length(eye - (lo + hi) * 0.5f);
    const size_t texels = size_t(size) * size;
    std::vector<uint16_t> color(texels * 4);
    std::vector<float> depth(texels), faces(6 * texels * 4), distance(6 * texels);
    for (int face = 0; face < 6; ++face) {
        Renderer::Frame cf = Nested(frame, size, size, FaceViewProj(face, eye, n, f), eye);
        cf.skinning = _updates == 1;  // the first frame: no joints were skinned yet
        // A blurry little image: one coarser cascade per face is plenty.
        cf.shadowCascades = 1;
        cf.shadowResolution = std::min(frame.shadowResolution, 1024u);
        if (gpu.environment && gpu.environmentLatlong == kNone) {
            // An untextured dome is uniform: its colour behind everything.
            std::copy(gpu.environmentRadiance, gpu.environmentRadiance + 3, cf.clearColor);
            cf.clearColor[3] = 1.0f;
        }
        Renderer::Targets out;
        out.color = color.data();
        out.depth = depth.data();
        _renderer->Render(scene, cf, out);
        probeMs += _renderer->GetStats().gpuMs;
        for (size_t i = 0; i < texels; ++i) {
            for (int c = 0; c < 4; ++c) {
                faces[(face * texels + i) * 4 + c] = HalfToFloat(color[i * 4 + c]);
            }
            // Distance along the texel's direction (0: nothing, the sky).
            const float sc = (float(i % size) + 0.5f) / float(size) * 2.0f - 1.0f;
            const float tc = (float(i / size) + 0.5f) / float(size) * 2.0f - 1.0f;
            const float z = 2.0f * f * n / ((f + n) - (depth[i] * 2.0f - 1.0f) * (f - n));
            distance[face * texels + i] = depth[i] >= 1.0f ? 0.0f : z * std::sqrt(1.0f + sc * sc + tc * tc);
        }
    }
    EnvironmentMaps maps = BuildEnvironment(faces.data(), size);
    for (size_t i = 0; i < distance.size(); ++i) {
        maps.data[i * 4 + 3] = distance[i];  // level 0 comes first
    }

    // ponytail: prefiltered on the CPU like the dome (~10 ms at 128);
    // a compute prefilter would let it follow animation every frame.
    _dev.Destroy(_probe);
    _probe = _dev.CreateImage(VK_FORMAT_R32G32B32A32_SFLOAT, maps.size, maps.size, 6, maps.levels,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                              VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT);
    Buffer staging;
    _dev.Reserve(staging, maps.data.size() * sizeof(float), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, maps.data.data(), maps.data.size() * sizeof(float));
    _dev.Submit([&](VkCommandBuffer cmd) {
        const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, maps.levels, 0, 6};
        ImageBarrier(cmd, _probe.image, all, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        std::vector<VkBufferImageCopy> copies(maps.levels);
        VkDeviceSize offset = 0;
        for (uint32_t l = 0; l < maps.levels; ++l) {
            const uint32_t s = maps.size >> l;
            copies[l] = {};
            copies[l].bufferOffset = offset;
            copies[l].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 6};
            copies[l].imageExtent = {s, s, 1};
            offset += VkDeviceSize(6) * s * s * 16;
        }
        vkCmdCopyBufferToImage(cmd, staging.buffer, _probe.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               uint32_t(copies.size()), copies.data());
        ImageBarrier(cmd, _probe.image, all, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    });
    _dev.Destroy(staging);
    _probeLevels = maps.levels;
    _probeSize = size;
    _probePosition[0] = eye.x;
    _probePosition[1] = eye.y;
    _probePosition[2] = eye.z;
    std::copy(gpu.boundsMin, gpu.boundsMin + 3, _probeMin);
    std::copy(gpu.boundsMax, gpu.boundsMax + 3, _probeMax);
    _probeVersion = gpu.version;
    _probeLighting = gpu.lightingHash;
    _sinceCapture = 0;
}

bool Reflections::_RenderPlanar(const Scene &scene, const Renderer::Frame &frame)
{
    const Scene::Gpu &gpu = scene.GetGpu();
    const Mat4 viewProj = ToMat4(frame.viewProj);
    // The mirror's bounds in view: not all corners beyond one clip plane.
    int outside[6] = {};
    for (int k = 0; k < 8; ++k) {
        const float p[3] = {(k & 1) ? gpu.mirrorMax[0] : gpu.mirrorMin[0], (k & 2) ? gpu.mirrorMax[1] : gpu.mirrorMin[1],
                            (k & 4) ? gpu.mirrorMax[2] : gpu.mirrorMin[2]};
        float clip[4];
        for (int j = 0; j < 4; ++j) {
            clip[j] = p[0] * viewProj.m[0][j] + p[1] * viewProj.m[1][j] + p[2] * viewProj.m[2][j] + viewProj.m[3][j];
        }
        for (int a = 0; a < 3; ++a) {
            outside[a * 2] += clip[a] < -clip[3];
            outside[a * 2 + 1] += clip[a] > clip[3];
        }
    }
    const Vec3 eye{frame.cameraPos[0], frame.cameraPos[1], frame.cameraPos[2]};
    float plane[4];
    std::copy(gpu.mirrorPlane, gpu.mirrorPlane + 4, plane);
    const Vec3 normal{plane[0], plane[1], plane[2]};
    const float side = Dot(normal, eye) + plane[3];
    const float extent = Length(Vec3{gpu.boundsMax[0] - gpu.boundsMin[0], gpu.boundsMax[1] - gpu.boundsMin[1],
                                     gpu.boundsMax[2] - gpu.boundsMin[2]});
    if (std::any_of(outside, outside + 6, [](int o) { return o == 8; }) || std::abs(side) <= 1e-5f * extent) {
        return false;
    }
    if (side < 0.0f) {
        std::transform(plane, plane + 4, plane, [](float x) { return -x; });  // facing the camera
    }
    // The scene mirrored in the plane, seen by this camera: x - 2 (n.x + d) n
    // first. Its near plane is then the mirror itself (Lengyel's oblique
    // clipping): nothing behind the mirror shows in it.
    Mat4 mirror;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            mirror.m[i][j] -= 2.0f * plane[i] * plane[j];
        }
        mirror.m[3][i] = -2.0f * plane[3] * plane[i];
    }
    Mat4 mirrored = mirror * viewProj;
    const Mat4 inv = Inverse(mirrored);
    // Clipped just above the mirror, which would otherwise cover its own
    // reflection (and beyond the flatness Scene allows it).
    const float mirrorSize = Length(Vec3{gpu.mirrorMax[0] - gpu.mirrorMin[0], gpu.mirrorMax[1] - gpu.mirrorMin[1],
                                         gpu.mirrorMax[2] - gpu.mirrorMin[2]});
    const float clip[4] = {plane[0], plane[1], plane[2], plane[3] - 2e-3f * mirrorSize};
    float c[4] = {}, q[4] = {};
    for (int j = 0; j < 4; ++j) {
        for (int k = 0; k < 4; ++k) {
            c[j] += inv.m[j][k] * clip[k];  // the plane in clip space
        }
    }
    const float corner[4] = {c[0] >= 0.0f ? 1.0f : -1.0f, c[1] >= 0.0f ? 1.0f : -1.0f, 1.0f, 1.0f};
    for (int k = 0; k < 4; ++k) {
        for (int j = 0; j < 4; ++j) {
            q[k] += corner[j] * inv.m[j][k];
        }
    }
    const float scale = 2.0f / (clip[0] * q[0] + clip[1] * q[1] + clip[2] * q[2] + clip[3] * q[3]);
    for (int i = 0; i < 4; ++i) {
        mirrored.m[i][2] = scale * clip[i] - mirrored.m[i][3];
    }

    if (!_renderer) {
        _renderer = std::make_unique<Renderer>(_dev);
    }
    const float fraction = std::clamp(frame.planarScale, 0.1f, 1.0f);
    const uint32_t width = std::max(1u, uint32_t(std::lround(float(frame.width) * fraction)));
    const uint32_t height = std::max(1u, uint32_t(std::lround(float(frame.height) * fraction)));
    const Vec3 mirroredEye = eye - Vec3{plane[0], plane[1], plane[2]} * (2.0f * std::abs(side));
    Renderer::Frame pf = Nested(frame, width, height, mirrored, mirroredEye);
    // ponytail: clusters and cascades assume the near plane faces the
    // camera; the oblique one doesn't, so every light per pixel and one
    // cascade over the whole view.
    pf.clusteredLights = false;
    pf.shadowCascades = 1;
    pf.skinning = _updates == 1;  // the first frame: no joints were skinned yet
    _renderer->Render(scene, pf, Renderer::Targets{});
    planarMs = _renderer->GetStats().gpuMs;
    std::copy(plane, plane + 4, _plane);
    return true;
}

} // namespace vkr
