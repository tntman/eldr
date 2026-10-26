// Times Eldr frames with a viewport's AOV bindings, as Houdini's Solaris
// viewport sets them up (HdxTaskController render outputs, CPU maps after
// each frame), while the camera orbits; and checks the id AOVs against the
// render index. ctest runs --check --ids (test viewport_bindings).
//
// viewportBench <scene> <camera prim> [options]
//   --size WxH         render size (default 1280x720)
//   --frames N         frames timed (default 120, after 10 warm-up frames)
//   --aovs a,b,...     render outputs (default Houdini's: color,normal,primvars:st)
//   --map a,b,...      buffers mapped after each frame (default: all bound)
//   --share 0|1        eldr:shareFrame (default 0: read back, as in Houdini)
//   --still            don't move the camera
//   --no-overlap       eldr:overlapReadback off (each frame waits for its own)
//   --app-ms N         the app's own work after each frame's maps (busy, N ms:
//                      Houdini's viewport uploads, composites and draws)
//   --ids              check primId/instanceId against the render index
//   --check            instead of timing: which AOVs render and read back
//                      when the app reads only some (needs normal bound)
//   --nav              instead of timing: what Houdini's camera tools read
//                      (depth vs the scene's /ground at y = 0) and a still
//                      camera's convergence and stability
//   --overlap          instead of timing: eldr:overlapReadback's frames
//                      (colour, depth) against read back at once
#include "pxr/base/gf/frustum.h"
#include "pxr/base/gf/rotation.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/imaging/cameraUtil/conformWindow.h"
#include "pxr/imaging/hd/camera.h"
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

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
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
                  "[--share 0|1] [--still] [--ids] [--check] [--nav] [--overlap] [--no-overlap] [--app-ms N]");
        return 2;
    }
    int width = 1280, height = 720, frames = 120, share = 0;
    double appMs = 0;
    bool still = false, ids = false, check = false, nav = false, overlap = false, noOverlap = false, mapAll = true;
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
        } else if (a == "--app-ms") {
            appMs = std::atof(next), ++i;
        } else if (a == "--still") {
            still = true;
        } else if (a == "--ids") {
            ids = true;
        } else if (a == "--check") {
            check = true;
        } else if (a == "--nav") {
            nav = true;
        } else if (a == "--overlap") {
            overlap = true;
        } else if (a == "--no-overlap") {
            noOverlap = true;
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
    delegate->SetRenderSetting(TfToken("eldr:overlapReadback"), VtValue(!noOverlap));
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
        int failures = 0;
        if (check) {
            // Houdini's pattern: colour read every frame, the rest now and then.
            auto frame = [&](double angle) {
                controller.SetFreeCameraMatrices(GfMatrix4d().SetRotate(GfRotation(GfVec3d(0, 1, 0), angle)) * view,
                                                 projection);
                engine.Execute(index, &tasks);
                const VtDictionary stats = delegate->GetRenderStats();
                auto it = stats.find("gpuPassMs");
                return it != stats.end() && it->second.IsHolding<VtDictionary>() ? it->second.UncheckedGet<VtDictionary>()
                                                                                : VtDictionary();
            };
            auto expect = [&](bool ok, const char *what) {
                std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
                failures += !ok;
            };
            auto read = [](HdRenderBuffer *b) {
                b->Map();
                b->Unmap();
            };
            HdRenderBuffer *color = controller.GetRenderOutput(HdAovTokens->color);
            HdRenderBuffer *normal = controller.GetRenderOutput(HdAovTokens->normal);
            HdRenderBuffer *primId = controller.GetRenderOutput(HdAovTokens->primId);
            if (!color || !normal || !primId) {
                std::puts("FAIL: --check needs colour, normal and the ids bound");
                std::exit(1);
            }
            VtDictionary passes = frame(0);
            expect(!passes.empty(), "GPU pass times are there");
            expect(passes.count("aovs"), "new buffers: the extra AOVs render");
            read(color);
            passes = frame(1);
            expect(!passes.count("aovs"), "moving: unread extra AOVs are left out");
            const auto *p = static_cast<const int32_t *>(primId->Map());  // not read with the frame: fetched now
            expect(std::any_of(p, p + size_t(width) * height, [](int32_t id) { return id >= 0; }),
                   "an id AOV read after the frame has the ids");
            primId->Unmap();
            read(color);
            read(normal);  // a frame old; rendered from the next one
            passes = frame(2);
            expect(passes.count("aovs"), "moving: a read extra AOV renders");
            passes = frame(2);
            expect(passes.count("aovs"), "still: the extra AOVs render, read or not");
        }
        if (nav) {
            // Houdini's camera tools: the pivot and pan scale come from the
            // depth under the cursor (UsdRenderers.json "depthstyle": "opengl",
            // window depth in [0, 1] under the projection Houdini passed).
            HdRenderBuffer *color = controller.GetRenderOutput(HdAovTokens->color);
            HdRenderBuffer *depth = controller.GetRenderOutput(HdAovTokens->depth);
            HdRenderBuffer *primId = controller.GetRenderOutput(HdAovTokens->primId);
            const SdfPath ground("/ground");
            auto expect = [&](bool ok, const std::string &what) {
                std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
                failures += !ok;
            };
            auto render = [&](const GfMatrix4d &v) {
                controller.SetFreeCameraMatrices(v, projection);
                engine.Execute(index, &tasks);
            };
            // The ground's pixels (y = 0) back-projected through the app's matrices.
            auto groundError = [&](const GfMatrix4d &v, const GfMatrix4d &proj) {
                const auto *d = static_cast<const float *>(depth->Map());
                const auto *p = static_cast<const int32_t *>(primId->Map());
                const GfMatrix4d inv = (v * proj).GetInverse();
                const GfVec3d eye = v.GetInverse().ExtractTranslation();
                double worst = 0;
                size_t n = 0;
                for (int y = 0; y < height; y += 4) {
                    for (int x = 0; x < width; x += 4) {
                        const size_t i = size_t(y) * width + x;
                        if (p[i] < 0 || index->GetRprimPathFromPrimId(p[i]) != ground) {
                            continue;
                        }
                        // Buffers are bottom row first (GL).
                        const GfVec3d ndc(2.0 * (x + 0.5) / width - 1.0, 2.0 * (y + 0.5) / height - 1.0, 2.0 * d[i] - 1.0);
                        const GfVec3d world = inv.Transform(ndc);
                        worst = std::max(worst, std::abs(world[1]) / (world - eye).GetLength());
                        ++n;
                    }
                }
                depth->Unmap();
                primId->Unmap();
                return n ? worst : 1e9;
            };
            auto readColor = [&](std::vector<GfHalf> *pixels) {
                const auto *c = static_cast<const GfHalf *>(color->Map());
                pixels->assign(c, c + size_t(width) * height * 4);
                color->Unmap();
            };
            auto meanDiff = [](const std::vector<GfHalf> &a, const std::vector<GfHalf> &b) {
                double s = 0;
                for (size_t i = 0; i < a.size(); ++i) {
                    s += std::abs(float(a[i]) - float(b[i]));
                }
                return s / double(a.size());
            };
            auto stat = [&](const char *key) {
                const VtDictionary stats = delegate->GetRenderStats();
                auto it = stats.find(key);
                return it == stats.end() ? VtValue() : it->second;
            };
            const GfMatrix4d moved = GfMatrix4d().SetRotate(GfRotation(GfVec3d(0, 1, 0), 3.0)) * view;
            render(view);
            expect(groundError(view, projection) < 0.01,
                   TfStringPrintf("depth read with the frame: the ground at y = 0 (error %.2g)",
                                  groundError(view, projection)));
            // husk (and maybe Houdini's viewport) binds depth with clear value
            // 0: still drawn, 0 where nothing was hit (the sky's top row).
            HdAovDescriptor desc = controller.GetRenderOutputSettings(HdAovTokens->depth);
            desc.clearValue = VtValue(0.0f);
            controller.SetRenderOutputSettings(HdAovTokens->depth, desc);
            render(view);
            const auto *cleared = static_cast<const float *>(depth->Map());
            const auto sky = std::count(cleared, cleared + size_t(width) * height, 0.0f),
                       unhit = std::count(cleared, cleared + size_t(width) * height, 1.0f);
            depth->Unmap();
            expect(groundError(view, projection) < 0.01 && sky > 0 && unhit == 0,
                   TfStringPrintf("depth cleared to 0: ground error %.2g, %d sky pixels 0, %d at 1",
                                  groundError(view, projection), int(sky), int(unhit)));
            desc.clearValue = VtValue(1.0f);
            controller.SetRenderOutputSettings(HdAovTokens->depth, desc);
            // Houdini maps the depth on a click only: frames with colour read, then depth.
            std::vector<GfHalf> a, b;
            for (int f = 0; f < 5; ++f) {
                render(f % 2 ? view : moved);
                readColor(&a);
            }
            expect(groundError(moved, projection) < 0.01,
                   TfStringPrintf("depth mapped later is the last frame's (error %.2g)", groundError(moved, projection)));
            // A still camera, as Houdini redraws: the film runs to convergence
            // without starting over, and frames converge.
            auto converge = [&](const std::function<void()> &frame, int *restarts) {
                double previous = -1;
                std::string stage;
                int frames = 0;
                for (*restarts = 0; frames < 400 && stage != "converged"; ++frames) {
                    frame();
                    const VtValue done = stat("percentDone");
                    const double pct = done.IsHolding<double>() ? done.UncheckedGet<double>() : -1;
                    // The film's percentage (after interactive TAA's 100) only grows.
                    *restarts += stage != "interactive" && previous >= 0 && pct < previous;
                    previous = pct;
                    stage = stat("rendererStage").GetWithDefault<std::string>();
                }
                return frames;
            };
            render(view);
            readColor(&a);
            double first = -1, last = -1;
            int restarts = 0;
            int frames = converge([&] {
                render(view);
                readColor(&b);
                last = meanDiff(a, b);
                first = first < 0 ? last : first;
                a.swap(b);
            }, &restarts);
            expect(frames < 400 && restarts == 0,
                   TfStringPrintf("still camera: converged after %d frames, %d restarts", frames, restarts));
            expect(last <= first * 0.1, TfStringPrintf("still camera: mean change between frames %.3g first, %.3g last",
                                                       first, last));
            expect(groundError(view, projection) < 0.01,
                   TfStringPrintf("converged film: depth error %.2g", groundError(view, projection)));
            // Houdini's free camera is a scene prim (/__HoudiniFreeCamera__,
            // its FreeCamera.usda: shutter -0.25 to 0.25) it sends again,
            // unchanged, on each redraw: a dirty camera.
            const SdfPath cam(argv[2]);
            UsdGeomCamera usdCam(stage->GetPrimAtPath(cam));
            usdCam.GetShutterOpenAttr().Set(-0.25);
            usdCam.GetShutterCloseAttr().Set(0.25);
            for (const bool touch : {false, true}) {
                controller.SetCameraPath(SdfPath());
                render(moved);
                controller.SetCameraPath(cam);
                frames = converge([&] {
                    if (touch) {
                        index->GetChangeTracker().MarkSprimDirty(cam, HdChangeTracker::AllDirty);
                    }
                    engine.Execute(index, &tasks);
                }, &restarts);
                expect(frames < 400 && restarts == 0,
                       TfStringPrintf("scene camera%s: converged after %d frames, %d restarts",
                                      touch ? " dirtied each frame" : "", frames, restarts));
                const auto *sprim = static_cast<const HdCamera *>(index->GetSprim(HdPrimTypeTokens->camera, cam));
                const GfMatrix4d camView = sprim->GetTransform().GetInverse();
                const GfMatrix4d camProj = CameraUtilConformedWindow(sprim->ComputeProjectionMatrix(), CameraUtilFit,
                                                                     double(width) / height);
                expect(groundError(camView, camProj) < 0.01,
                       TfStringPrintf("its depth after convergence: error %.2g", groundError(camView, camProj)));
            }
        }
        if (overlap) {
            // eldr:overlapReadback: a moving camera's frames arrive one frame
            // late but unchanged, a still camera's at once. TAA off: each
            // frame's image is its camera's alone, whatever came before.
            HdRenderBuffer *color = controller.GetRenderOutput(HdAovTokens->color);
            HdRenderBuffer *depth = controller.GetRenderOutput(HdAovTokens->depth);
            delegate->SetRenderSetting(TfToken("eldr:taa"), VtValue(false));
            using Image = std::pair<std::vector<GfHalf>, std::vector<float>>;
            auto run = [&](bool on) {
                delegate->SetRenderSetting(TfToken("eldr:overlapReadback"), VtValue(on));
                std::vector<Image> images;
                for (int f = 0; f <= 6; ++f) {  // moving, then still at the last angle
                    controller.SetFreeCameraMatrices(
                        GfMatrix4d().SetRotate(GfRotation(GfVec3d(0, 1, 0), 3.0 * std::min(f, 5))) * view, projection);
                    engine.Execute(index, &tasks);
                    const auto *c = static_cast<const GfHalf *>(color->Map());
                    const auto *d = static_cast<const float *>(depth->Map());
                    images.push_back({{c, c + size_t(width) * height * 4}, {d, d + size_t(width) * height}});
                    color->Unmap();
                    depth->Unmap();
                }
                return images;
            };
            const std::vector<Image> sync = run(false), overlapped = run(true);
            auto colourDiff = [](const Image &a, const Image &b) {
                double c = 0;
                for (size_t i = 0; i < a.first.size(); ++i) {
                    c += std::abs(float(a.first[i]) - float(b.first[i]));
                }
                return c / double(a.first.size());
            };
            auto expect = [&](bool ok, const std::string &what) {
                std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str());
                failures += !ok;
            };
            // 0..5 move: each shows the frame before (0 the first run's
            // last, already in the buffers); 6 is still and its own. The
            // same camera's depth is exact; colour differs by its frame's
            // noise and history (AO, shadows, exposure after the jump to
            // frame 0), less than between cameras.
            auto shows = [&](const Image &got, const Image &want, const Image &other, const std::string &what) {
                const double same = colourDiff(got, want), apart = colourDiff(got, other);
                expect(got.second == want.second && same < 0.5 * apart,
                       TfStringPrintf("%s: depth %s, colour off by %.2g (%.2g from another camera's)", what.c_str(),
                                      got.second == want.second ? "exact" : "differs", same, apart));
            };
            shows(overlapped[0], sync[6], sync[0], "the first deferred frame keeps the image before");
            for (int f = 1; f <= 5; ++f) {
                shows(overlapped[f], sync[f - 1], sync[f], TfStringPrintf("moving frame %d shows frame %d", f, f - 1));
            }
            shows(overlapped[6], sync[6], sync[4], "a still camera's frame is its own");
        }
        if (!check && !nav && !overlap) {
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
                for (const auto mapped = Clock::now(); Ms(mapped, Clock::now()) < appMs;) {
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
            std::printf("%dx%d, %d frames, aovs %s, shareFrame %d%s, app %.1f ms\n", width, height, frames,
                        TfStringJoin(aovs, ",").c_str(), share, still ? ", still" : "", appMs);
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
        }

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
