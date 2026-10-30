#pragma once

#include "renderer.h"
#include "vkmath.h"

#include <memory>

namespace vkr {

// Reflections of what the screen doesn't show (SSR's limit), both rendered
// by a second Renderer before the frame (shaders/probes.glsl reads them):
//
// Reflection probes: cubes prefiltered for roughness like the dome
// (BuildEnvironment), with each texel's distance in level 0's alpha for
// parallax correction (the probe's box, then the distances), blended by
// influence over the dome's specular light. There is no UsdLux schema for
// probes. Authored ones (Frame::authoredProbes) are meshes flagged with
// primvars:eldr:reflectionProbe, typically an invisible Cube: never drawn,
// their world bounds are the influence and parallax box and its centre the
// capture point. Without any, one automatic probe at the centre of the
// scene's bounds (or Frame::probePosition) covers everything. They are
// captured again when the lights, materials or dome change, and when the
// geometry does once it has stopped changing (or every kRecaptureFrames
// frames while it keeps changing).
//
// Planar reflections: for each mirror mesh (Scene::Gpu::mirrors: flagged
// by primvars:eldr:planarReflection, or the largest flat glossy surface;
// up to Frame::planarCount) the scene mirrored in its plane, clipped at it
// (an oblique near plane) and rendered by a renderer of its own at a
// fraction of the frame's size every frame the mirror is in view; the
// mirror's pixels take their reflection from it.
class Reflections {
public:
    explicit Reflections(Device &device);
    ~Reflections();
    Reflections(const Reflections &) = delete;
    Reflections &operator=(const Reflections &) = delete;

    // Before the frame records: captures the probes when due. The GPU is
    // idle on return. Probes show the scene at the frame's time, also
    // under motion blur time samples: a capture is six renders and a CPU
    // prefilter (~10 ms at 128), too slow per film sample.
    void Update(const Scene &scene, const Renderer::Frame &frame);

    // After Update, once the frame's camera is known (its time sample's,
    // its lens point's): renders the planar reflections (if
    // Frame::planarReflections and a mirror is in view). shutter >= 0: at
    // that motion blur time sample (Frame::nestedShutter, nestedCapture).
    // The GPU is idle on return.
    void RenderPlanars(const Scene &scene, const Renderer::Frame &frame, float shutter, bool capture);

    // The probes, the smallest influence box first: each a cube (world
    // directions; level 0 alpha: distance, 0 for sky) prefiltered by
    // roughness, its capture point, influence and parallax box, the
    // distance its influence fades in over from the box's faces (0:
    // everywhere) and its irradiance in SH L0 and L1 (BuildEnvironment's
    // basis order); pending: out of date, keep rendering frames.
    static constexpr uint32_t kMaxProbes = 8;
    struct Probe {
        Image cube;
        uint32_t levels = 0, size = 0;
        float center[3] = {}, min[3] = {}, max[3] = {};
        float blend = 0;
        float sh[4][3] = {};
        uint64_t version = 0, lighting = 0;  // the scene it shows
    };
    const std::vector<Probe> &Probes() const { return _probes; }
    bool ProbePending() const { return _pending; }

    // This frame's planar reflections: each image (premultiplied HDR, in
    // TRANSFER_SRC_OPTIMAL), the plane it mirrors (unit normal towards the
    // camera, offset) and the mirror's mesh slot.
    struct Planar {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        float plane[4] = {};
        uint32_t slot = 0;
    };
    const std::vector<Planar> &Planars() const { return _planars; }

    // GPU milliseconds of this frame's capture and planar render (0: none).
    double probeMs = 0, planarMs = 0;

    static constexpr uint32_t kRecaptureFrames = 120;

private:
    void _Capture(const Scene &scene, const Renderer::Frame &frame, Probe &probe);
    bool _RenderPlanar(const Scene &scene, const Renderer::Frame &frame, const Scene::Gpu::Mirror &mirror,
                       Renderer &renderer, float plane[4], float shutter, bool capture);

    Device &_dev;
    std::unique_ptr<Renderer> _renderer;  // made on first use
    std::vector<Probe> _probes;
    uint64_t _lastVersion = 0;                        // last frame's scene version
    uint32_t _stableFrames = 0, _sinceCapture = 0, _updates = 0;
    bool _pending = false;
    std::vector<std::unique_ptr<Renderer>> _planarRenderers;  // one per mirror
    std::vector<Planar> _planars;                             // rendered this frame
};

} // namespace vkr
