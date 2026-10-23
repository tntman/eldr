#pragma once

#include "renderer.h"
#include "vkmath.h"

#include <memory>

namespace vkr {

// Reflections of what the screen doesn't show (SSR's limit), both rendered
// by a second Renderer before the frame (shaders/probes.glsl reads them):
//
// The reflection probe: one cube captured from the centre of the scene's
// bounds (or Frame::probePosition), prefiltered for roughness like the dome
// (BuildEnvironment), with each texel's distance in level 0's alpha for
// parallax correction (the scene's bounds, then the distances). It
// replaces the dome's specular light. There is no USD schema for probes:
// it is automatic, captured again when the lights, materials or dome
// change, and when the geometry does once it has stopped changing (or every
// kRecaptureFrames frames while it keeps changing).
//
// Planar reflections: the scene mirrored in the plane of the mirror mesh
// (Scene::Gpu::mirrorSlot: flagged by primvars:hdvk:planarReflection, or
// the largest flat glossy surface), clipped at it (an oblique near plane)
// and rendered at a fraction of the frame's size every frame the mirror is
// in view; the mirror's pixels take their reflection from it.
class Reflections {
public:
    explicit Reflections(Device &device);
    ~Reflections();
    Reflections(const Reflections &) = delete;
    Reflections &operator=(const Reflections &) = delete;

    // Before the frame records: captures the probe when due, renders the
    // planar reflection (if Frame::planarReflections and a mirror is in
    // view). The GPU is idle on return.
    void Update(const Scene &scene, const Renderer::Frame &frame);

    // The probe: a cube view (null: none), its mip levels, where it was
    // captured, the scene's bounds then; pending: out of date, keep
    // rendering frames.
    VkImageView Probe() const { return _probe.view; }
    uint32_t ProbeLevels() const { return _probeLevels; }
    const float *ProbePosition() const { return _probePosition; }
    const float *ProbeMin() const { return _probeMin; }
    const float *ProbeMax() const { return _probeMax; }
    bool ProbePending() const { return _pending; }

    // This frame's planar reflection (premultiplied HDR, in
    // TRANSFER_SRC_OPTIMAL) and the plane it mirrors (unit normal towards
    // the camera, offset), or no image.
    VkImage PlanarImage() const;
    VkImageView PlanarView() const;
    const float *PlanarPlane() const { return _plane; }

    // GPU milliseconds of this frame's capture and planar render (0: none).
    double probeMs = 0, planarMs = 0;

    static constexpr uint32_t kRecaptureFrames = 120;

private:
    void _Capture(const Scene &scene, const Renderer::Frame &frame, uint32_t size, Vec3 eye);
    bool _RenderPlanar(const Scene &scene, const Renderer::Frame &frame);

    Device &_dev;
    std::unique_ptr<Renderer> _renderer;  // made on first use
    Image _probe;
    uint32_t _probeLevels = 0, _probeSize = 0;
    float _probePosition[3] = {}, _probeMin[3] = {}, _probeMax[3] = {};
    uint64_t _probeVersion = 0, _probeLighting = 0;  // the scene it shows
    uint64_t _lastVersion = 0;                        // last frame's scene version
    uint32_t _stableFrames = 0, _sinceCapture = 0, _updates = 0;
    bool _pending = false;
    bool _planar = false;  // rendered this frame
    float _plane[4] = {};
};

} // namespace vkr
