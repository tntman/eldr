// Times Eldr frames with a viewport's AOV bindings, as Houdini's Solaris
// viewport sets them up (HdxTaskController render outputs, CPU maps after
// each frame), while the camera orbits; and checks the id AOVs against the
// render index. Not part of ctest's Python suite: run it by hand.
//
// viewportBench <scene> <camera prim> [options]
//   --size WxH         render size (default 1280x720)
//   --frames N         frames timed (default 120, after 10 warm-up frames)
//   --aovs a,b,...     render outputs (default Houdini's: color,normal,primvars:st)
//   --map a,b,...      buffers mapped after each frame (default: all bound)
//   --share 0|1        eldr:shareFrame (default 0: read back, as in Houdini)
//   --still            don't move the camera
//   --ids              check primId/instanceId against the render index
#include "pxr/base/gf/frustum.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/imaging/hd/engine.h"
#include "pxr/imaging/hd/renderBuffer.h"
#include "pxr/imaging/hd/renderDelegate.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/rendererPlugin.h"
#include "pxr/imaging/hd/rendererPluginRegistry.h"
#include "pxr/imaging/hd/task.h"
#include "pxr/imaging/hdx/taskController.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usdImaging/usdImaging/delegate.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a, Clock::time_point b)
{
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        std::puts("usage: viewportBench <scene> <camera prim> [--size WxH] [--frames N] [--aovs a,b] [--map a,b] "
                  "[--share 0|1] [--still] [--ids]");
        return 2;
    }
    int width = 1280, height = 720, frames = 120, share = 0;
    bool still = false, ids = false, mapAll = true;
    std::vector<std::string> aovs = {"color", "normal", "primvars:st"}, mapped;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--size") {
            std::sscanf(next, "%dx%d", &width, &height), ++i;
        } else if (a == "--frames") {
            frames = std::atoi(next), ++i;
        } else if (a == "--aovs") {
            aovs = TfStringSplit(next, ","), ++i;
        } else if (a == "--map") {
            mapped = TfStringSplit(next, ","), mapAll = false, ++i;
        } else if (a == "--share") {
            share = std::atoi(next), ++i;
        } else if (a == "--still") {
            still = true;
        } else if (a == "--ids") {
            ids = true;
        }
    }

    UsdStageRefPtr stage = UsdStage::Open(argv[1]);
    if (!stage) {
        std::printf("FAIL: can't open %s\n", argv[1]);
        return 1;
    }
    HdRendererPluginRegistry &registry = HdRendererPluginRegistry::GetInstance();
    HdRendererPlugin *plugin = registry.GetRendererPlugin(TfToken("HdVkRendererPlugin"));
    HdRenderDelegate *delegate = plugin ? plugin->CreateRenderDelegate() : nullptr;
    if (!delegate) {
        std::puts("FAIL: no Eldr delegate (PXR_PLUGINPATH_NAME?)");
        return 1;
    }
    delegate->SetRenderSetting(TfToken("eldr:shareFrame"), VtValue(share != 0));
    HdRenderIndex *index = HdRenderIndex::New(delegate, {});
    int result = 0;
    {
        UsdImagingDelegate scene(index, SdfPath::AbsoluteRootPath());
        scene.Populate(stage->GetPseudoRoot());
        scene.SetTime(UsdTimeCode::EarliestTime());
        HdxTaskController controller(index, SdfPath("/viewportBench"));
        controller.SetRenderBufferSize(GfVec2i(width, height));
        controller.SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), width, height)));
        TfTokenVector outputs;
        for (const std::string &aov : aovs) {
            outputs.push_back(TfToken(aov));
        }
        controller.SetRenderOutputs(outputs);

        GfCamera camera = UsdGeomCamera(stage->GetPrimAtPath(SdfPath(argv[2]))).GetCamera(UsdTimeCode::Default());
        camera.SetPerspectiveFromAspectRatioAndFieldOfView(double(width) / height, camera.GetFieldOfView(GfCamera::FOVHorizontal),
                                                           GfCamera::FOVHorizontal);
        const GfFrustum frustum = camera.GetFrustum();
        const GfMatrix4d view = frustum.ComputeViewMatrix(), projection = frustum.ComputeProjectionMatrix();

        HdTaskSharedPtrVector tasks;  // the render tasks only: presenting needs a GPU context
        for (const HdTaskSharedPtr &task : controller.GetRenderingTasks()) {
            if (TfStringStartsWith(task->GetId().GetName(), "renderTask")) {
                tasks.push_back(task);
            }
        }
        // What is bound: the controller adds depth and the ids beside colour.
        std::vector<std::pair<std::string, HdRenderBuffer *>> buffers;
        for (const TfToken &name : {HdAovTokens->color, HdAovTokens->depth, HdAovTokens->primId,
                                    HdAovTokens->instanceId, HdAovTokens->elementId}) {
            if (std::find(outputs.begin(), outputs.end(), name) == outputs.end()) {
                outputs.push_back(name);
            }
        }
        for (const TfToken &name : outputs) {
            HdRenderBuffer *b = controller.GetRenderOutput(name);
            const bool map = mapAll || std::find(mapped.begin(), mapped.end(), name.GetString()) != mapped.end();
            if (b && map) {
                buffers.push_back({name.GetString(), b});
            }
        }
        HdEngine engine;
        double executeMs = 0, gpuMs = 0, renderMs = 0, total = 0;
        std::map<std::string, double> mapMs, passMs;
        std::vector<double> times;  // per frame: their median resists other GPU users
        const int warmup = 10;
        for (int f = 0; f < warmup + frames; ++f) {
            // Orbit about the view's centre line: a camera move every frame.
            const double angle = still ? 0.0 : 0.25 * f;
            const GfMatrix4d orbit = GfMatrix4d().SetRotate(GfRotation(GfVec3d(0, 1, 0), angle));
            controller.SetFreeCameraMatrices(orbit * view, projection);
            const auto t0 = Clock::now();
            engine.Execute(index, &tasks);
            const auto t1 = Clock::now();
            std::map<std::string, double> frameMaps;
            for (auto &[name, b] : buffers) {
                const auto m0 = Clock::now();
                b->Map();
                b->Unmap();
                frameMaps[name] = Ms(m0, Clock::now());
            }
            const auto t2 = Clock::now();
            if (f >= warmup) {
                executeMs += Ms(t0, t1);
                total += Ms(t0, t2);
                times.push_back(Ms(t0, t2));
                for (auto &[name, ms] : frameMaps) {
                    mapMs[name] += ms;
                }
                const VtDictionary stats = delegate->GetRenderStats();
                auto it = stats.find("gpuTimeMs");
                gpuMs += it != stats.end() && it->second.IsHolding<double>() ? it->second.UncheckedGet<double>() : 0.0;
                it = stats.find("renderTimeMs");
                renderMs += it != stats.end() && it->second.IsHolding<double>() ? it->second.UncheckedGet<double>() : 0.0;
                it = stats.find("gpuPassMs");
                if (it != stats.end() && it->second.IsHolding<VtDictionary>()) {
                    for (const auto &[name, ms] : it->second.UncheckedGet<VtDictionary>()) {
                        passMs[name] += ms.IsHolding<double>() ? ms.UncheckedGet<double>() : 0.0;
                    }
                }
            }
        }
        std::printf("%dx%d, %d frames, aovs %s, shareFrame %d%s\n", width, height, frames,
                    TfStringJoin(aovs, ",").c_str(), share, still ? ", still" : "");
        std::sort(times.begin(), times.end());
        const double median = times[times.size() / 2];
        std::printf("  frame %.2f ms (%.1f fps), median %.2f: execute %.2f (Render %.2f), gpu %.2f", total / frames,
                    1000.0 * frames / total, median, executeMs / frames, renderMs / frames, gpuMs / frames);
        for (auto &[name, ms] : mapMs) {
            std::printf(", map %s %.2f", name.c_str(), ms / frames);
        }
        std::printf("\n  gpu passes:");
        for (auto &[name, ms] : passMs) {
            if (ms / frames >= 0.1) {
                std::printf(" %s %.2f", name.c_str(), ms / frames);
            }
        }
        std::puts("");

        int failures = 0;
        if (ids) {
            // Every primId names an rprim of the index, and an instanceId is
            // in range of that rprim's instancer (Houdini's render keys).
            HdRenderBuffer *primId = controller.GetRenderOutput(HdAovTokens->primId);
            HdRenderBuffer *instanceId = controller.GetRenderOutput(HdAovTokens->instanceId);
            if (!primId || !instanceId) {
                std::puts("FAIL: no primId/instanceId buffers");
                std::exit(1);
            }
            const auto *p = static_cast<const int32_t *>(primId->Map());
            const auto *n = static_cast<const int32_t *>(instanceId->Map());
            std::map<int, std::set<int>> seen;
            for (size_t i = 0, count = size_t(width) * height; i < count; ++i) {
                seen[p[i]].insert(n[i]);
            }
            primId->Unmap();
            instanceId->Unmap();
            for (const auto &[id, instances] : seen) {
                if (id < 0) {
                    continue;
                }
                const SdfPath path = index->GetRprimPathFromPrimId(id);
                SdfPath sdId, instancer;
                HdSceneDelegate *sd = path.IsEmpty() ? nullptr : index->GetSceneDelegateForRprim(path);
                const bool ok = sd && index->GetSceneDelegateAndInstancerIds(path, &sdId, &instancer);
                const int maxInstance = *instances.rbegin();
                const SdfPath scenePath = ok ? sd->GetScenePrimPath(path, std::max(maxInstance, 0)) : SdfPath();
                std::printf("  primId %d -> %s, instances %zu (max %d) -> %s\n", id,
                            path.IsEmpty() ? "(none)" : path.GetText(), instances.size(), maxInstance,
                            scenePath.GetText());
                failures += !ok || scenePath.IsEmpty() || (instancer.IsEmpty() && maxInstance > 0);
            }
            std::printf("%s: %zu prim ids, %d bad\n", failures ? "FAIL" : "OK", seen.size(), failures);
        }
        result = failures ? 1 : 0;
    }  // the controller and scene delegate remove their prims
    delete index;
    plugin->DeleteRenderDelegate(delegate);
    registry.ReleasePlugin(plugin);
    return result;
}
