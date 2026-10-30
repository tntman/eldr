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
//   --houdini          Houdini 22's viewport: maps (and copies) colour, depth
//                      and the ids every frame, sends houdini:viewport/interactive, and
//                      moves the scene camera (shutter -0.25..0.25, as its
//                      free camera) on the stage instead of a free camera
//   --set name=value   a render setting (bool 0/1, int, float or string)
//   --csv path         per-frame times (begin, execute, maps, frame)
//   --warmup N         frames before timing (default 10; 0 times a final
//                      quality film from its first sample)
//   --pass-csv path    per-frame GPU pass times: frame,pass,ms
//   --stop N           after the timed (moving) frames, up to N still ones:
//                      how long until full quality (the first frame at full
//                      size without moving reductions), the film and converged
//   --pause N          after the timed (moving) frames, N frames with the
//                      camera paused, then N moving again (a pause shorter
//                      than eldr:stillDelay): per frame ms, GPU ms and size
//   --startup          instead of timing: a cold start (plugin, delegate,
//                      scene, first frames) and frames to convergence
//   --ids              check primId/instanceId against the render index
//   --check            instead of timing: which AOVs render and read back
//                      when the app reads only some (needs normal bound)
//   --nav              instead of timing: what Houdini's camera tools read
//                      (depth vs the scene's /ground at y = 0) and a still
//                      camera's convergence and stability
//   --overlap          instead of timing: eldr:overlapReadback's frames, mapped
//                      in place (eldr:inPlaceReadback), against copied at once
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
#include <array>
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
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // results survive a crash at teardown
    if (argc < 3) {
        std::puts("usage: viewportBench <scene> <camera prim> [--size WxH] [--frames N] [--aovs a,b] [--map a,b] "
                  "[--share 0|1] [--render-purpose] [--still] [--ids] [--check] [--nav] [--overlap] [--no-overlap] [--app-ms N]");
        return 2;
    }
    int width = 1280, height = 720, frames = 120, share = 0;
    double appMs = 0;
    bool still = false, ids = false, check = false, nav = false, overlap = false, noOverlap = false, mapAll = true;
    bool houdini = false, startup = false, renderPurpose = false;
    std::string csvPath, passCsvPath;
    int warmupFrames = 10, stopFrames = 0, pauseFrames = 0;
    std::vector<std::string> aovs = {"color", "normal", "primvars:st"}, mapped, sets;
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
        } else if (a == "--render-purpose") {
            renderPurpose = true;
        } else if (a == "--houdini") {
            houdini = true;
            mapped = {"color", "depth", "primId", "instanceId"}, mapAll = false;
        } else if (a == "--set") {
            sets.push_back(next), ++i;
        } else if (a == "--csv") {
            csvPath = next, ++i;
        } else if (a == "--warmup") {
            warmupFrames = std::atoi(next), ++i;
        } else if (a == "--stop") {
            stopFrames = std::atoi(next), ++i;
        } else if (a == "--pause") {
            pauseFrames = std::atoi(next), ++i;
        } else if (a == "--pass-csv") {
            passCsvPath = next, ++i;
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
        } else if (a == "--startup") {
            startup = true;
        }
    }

    UsdStageRefPtr stage = UsdStage::Open(argv[1]);
    if (!stage) {
        std::printf("FAIL: can't open %s\n", argv[1]);
        return 1;
    }
    const auto tStart = Clock::now();
    HdRendererPluginRegistry &registry = HdRendererPluginRegistry::GetInstance();
    HdRendererPlugin *plugin = registry.GetRendererPlugin(TfToken("HdVkRendererPlugin"));
    const auto tPlugin = Clock::now();
    HdRenderDelegate *delegate = plugin ? plugin->CreateRenderDelegate() : nullptr;
    const auto tDelegate = Clock::now();
    if (!delegate) {
        std::puts("FAIL: no Eldr delegate (PXR_PLUGINPATH_NAME?)");
        return 1;
    }
    delegate->SetRenderSetting(TfToken("eldr:shareFrame"), VtValue(share != 0));
    delegate->SetRenderSetting(TfToken("eldr:overlapReadback"), VtValue(!noOverlap));
    if (houdini) {
        delegate->SetRenderSetting(TfToken("houdini:viewport"), VtValue(true));
        delegate->SetRenderSetting(TfToken("houdini:interactive"), VtValue(TfToken(still ? "normal" : "tumbling")));
    }
    for (const std::string &s : sets) {  // typed like the setting's current value
        const size_t eq = s.find('=');
        const TfToken name(s.substr(0, eq));
        const std::string text = eq == std::string::npos ? "1" : s.substr(eq + 1);
        const VtValue current = delegate->GetRenderSetting(name);
        VtValue v = current.IsHolding<bool>()    ? VtValue(text != "0" && text != "false")
                    : current.IsHolding<int>()   ? VtValue(std::atoi(text.c_str()))
                    : current.IsHolding<float>() ? VtValue(float(std::atof(text.c_str())))
                                                 : VtValue(text);
        delegate->SetRenderSetting(name, v);
    }
    HdRenderIndex *index = HdRenderIndex::New(delegate, {});
    int result = 0;
    {
        UsdImagingDelegate scene(index, SdfPath::AbsoluteRootPath());
        scene.Populate(stage->GetPseudoRoot());
        scene.SetTime(UsdTimeCode::EarliestTime());
        const auto tScene = Clock::now();
        HdxTaskController controller(index, SdfPath("/viewportBench"));
        controller.SetRenderBufferSize(GfVec2i(width, height));
        controller.SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), width, height)));
        TfTokenVector outputs;
        for (const std::string &aov : aovs) {
            outputs.push_back(TfToken(aov));
        }
        controller.SetRenderOutputs(outputs);
        if (renderPurpose) {  // crowds exported with purpose render (Houdini draws them)
            controller.SetRenderTags({HdRenderTagTokens->geometry, HdRenderTagTokens->render});
        }

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
            // Convergence below counts frames, and auto-exposure adapts on
            // wall-clock time (0.5 s time constant, ~2 s to settle): ~300
            // frames under validation, over 400 without (failing since the
            // check was added, b616182). The check is about the lazy AOVs.
            delegate->SetRenderSetting(TfToken("eldr:autoExposure"), VtValue(false));
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
            // eldr:lazyExtraAovs: unread ones wait for the film to converge.
            int waited = 0;
            do {
                passes = frame(2);
            } while (!passes.count("aovs") && ++waited < 400);
            expect(passes.count("aovs") && waited > 0,
                   TfStringPrintf("still: the unread extra AOVs render once converged (%d frames)", waited).c_str());
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
                // The reference copies; overlapped frames are mapped where they were read back.
                delegate->SetRenderSetting(TfToken("eldr:inPlaceReadback"), VtValue(on));
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
            // eldr:staleMovingIds (on): in Houdini's viewport a moving camera's
            // frames keep the last still frame's ids, mapped each frame as
            // Houdini does; a still one reads its own.
            if (HdRenderBuffer *primId = controller.GetRenderOutput(HdAovTokens->primId)) {
                delegate->SetRenderSetting(TfToken("houdini:viewport"), VtValue(true));
                delegate->SetRenderSetting(TfToken("eldr:stillDelay"), VtValue(30.0f));
                auto idsAt = [&](double angle) {
                    controller.SetFreeCameraMatrices(GfMatrix4d().SetRotate(GfRotation(GfVec3d(0, 1, 0), angle)) * view,
                                                     projection);
                    engine.Execute(index, &tasks);
                    const auto *p = static_cast<const int32_t *>(primId->Map());
                    std::vector<int32_t> ids(p, p + size_t(width) * height);
                    primId->Unmap();
                    return ids;
                };
                idsAt(0.0);
                const std::vector<int32_t> still = idsAt(0.0);
                const std::vector<int32_t> moved = idsAt(10.0), moved2 = idsAt(20.0);
                const std::vector<int32_t> held = idsAt(20.0);  // within eldr:stillDelay (30 s): still moving
                delegate->SetRenderSetting(TfToken("eldr:stillDelay"), VtValue(0.0f));
                idsAt(20.0);
                const std::vector<int32_t> stopped = idsAt(20.0);
                expect(moved == still && moved2 == still, "moving frames keep the still frame's ids");
                expect(held == still, "a paused camera (within eldr:stillDelay) keeps them too");
                expect(stopped != still, "a still frame reads its own ids");
            } else {
                expect(false, "--overlap needs primId bound (--aovs color,depth,primId)");
            }
        }
        if (startup) {
            // What each frame took until the film converged: a first still
            // frame, nine moving ones, then still.
            controller.SetFreeCameraMatrices(view, projection);
            std::vector<double> frameMs;
            std::string stage;
            // Depth and primId pixels that changed since the frame before
            // (jitter: Houdini depth-tests its guides and outlines the
            // selection with them).
            HdRenderBuffer *depthBuffer = controller.GetRenderOutput(HdAovTokens->depth);
            HdRenderBuffer *idBuffer = controller.GetRenderOutput(HdAovTokens->primId);
            std::vector<float> lastDepth;
            std::vector<int32_t> lastIds;
            std::vector<std::pair<int, int>> changed;
            const auto tFirst = Clock::now();
            {  // Houdini's viewport may poll before the first frame: sensible defaults, no crash
                const VtDictionary early = delegate->GetRenderStats();
                auto it = early.find("rendererStage");
                std::printf("%s: stats before the first frame (rendererStage %s)\n",
                            it != early.end() && it->second.IsHolding<std::string>() ? "ok" : "FAIL",
                            it != early.end() && it->second.IsHolding<std::string>()
                                ? it->second.UncheckedGet<std::string>().c_str() : "missing");
            }
            while (frameMs.size() < 2000 && stage != "converged") {
                // Frames 2 to 10 orbit, then the camera stops (TAA, then the film).
                const double angle = 0.5 * double(std::min<size_t>(frameMs.size(), 9));
                controller.SetFreeCameraMatrices(GfMatrix4d().SetRotate(GfRotation(GfVec3d(0, 1, 0), angle)) * view,
                                                 projection);
                const auto f0 = Clock::now();
                engine.Execute(index, &tasks);
                for (auto &[name, b] : buffers) {
                    b->Map();
                    b->Unmap();
                }
                frameMs.push_back(Ms(f0, Clock::now()));
                if (depthBuffer && idBuffer) {
                    const auto *d = static_cast<const float *>(depthBuffer->Map());
                    const auto *p = static_cast<const int32_t *>(idBuffer->Map());
                    const size_t n = size_t(width) * height;
                    int dc = 0, ic = 0;
                    for (size_t i = 0; i < n && !lastDepth.empty(); ++i) {
                        dc += std::abs(d[i] - lastDepth[i]) > 1e-4f * std::max(std::abs(d[i]), 1e-3f);
                        ic += p[i] != lastIds[i];
                    }
                    lastDepth.assign(d, d + n);
                    lastIds.assign(p, p + n);
                    depthBuffer->Unmap();
                    idBuffer->Unmap();
                    changed.push_back({dc, ic});
                }
                const VtDictionary stats = delegate->GetRenderStats();
                auto it = stats.find("rendererStage");
                stage = it != stats.end() && it->second.IsHolding<std::string>() ? it->second.UncheckedGet<std::string>()
                                                                                  : "";
            }
            std::printf("startup: plugin %.0f ms, delegate %.0f ms, scene %.0f ms, first frame %.0f ms\n",
                        Ms(tStart, tPlugin), Ms(tPlugin, tDelegate), Ms(tDelegate, tScene), frameMs[0]);
            std::printf("  frames 2-6:");
            for (size_t i = 1; i < std::min<size_t>(frameMs.size(), 6); ++i) {
                std::printf(" %.0f", frameMs[i]);
            }
            std::printf(" ms; %s after %zu frames, %.0f ms from the first frame, %.0f ms from the plugin\n",
                        stage.c_str(), frameMs.size(), Ms(tFirst, Clock::now()), Ms(tStart, Clock::now()));
            {  // where the memory went
                const VtDictionary stats = delegate->GetRenderStats();
                std::printf("  memory MB:");
                for (const char *key : {"gpuMemoryUsed", "textureMemory", "primvar", "topology", "volumeMemory",
                                        "accelerationStructureMemory", "hostMemoryUsed", "peakMemory"}) {
                    const auto it = stats.find(key);
                    if (it != stats.end() && it->second.IsHolding<size_t>()) {
                        std::printf(" %s %.0f", key, double(it->second.UncheckedGet<size_t>()) / 1048576.0);
                    }
                }
                std::puts("");
            }
            std::printf("  depth / primId pixels changed from the frame before:");
            for (size_t i = 1; i < changed.size(); ++i) {
                if ((i >= 9 && i < 30) || i + 3 > changed.size()) {
                    std::printf(" %d/%d", changed[i].first, changed[i].second);
                }
            }
            std::puts("");
        }
        if (!check && !nav && !overlap && !startup) {
            double executeMs = 0, gpuMs = 0, renderMs = 0, total = 0;
            std::map<std::string, double> mapMs, passMs;
            std::vector<double> times;  // per frame: their median resists other GPU users
            std::vector<std::array<double, 4>> rows;  // begin, execute, maps, frame
            const int warmup = warmupFrames;
            FILE *passCsv = passCsvPath.empty() ? nullptr : std::fopen(passCsvPath.c_str(), "w");
            if (passCsv) {
                std::fputs("frame,pass,ms\n", passCsv);
            }
            const SdfPath camPath(argv[2]);
            UsdGeomCamera usdCam(stage->GetPrimAtPath(camPath));
            const GfMatrix4d camXform = usdCam.ComputeLocalToWorldTransform(UsdTimeCode::Default());
            UsdGeomXformOp camOp;
            if (houdini) {
                usdCam.GetShutterOpenAttr().Set(-0.25);
                usdCam.GetShutterCloseAttr().Set(0.25);
                usdCam.ClearXformOpOrder();
                camOp = usdCam.AddTransformOp();
                controller.SetCameraPath(camPath);
            }
            std::vector<uint8_t> appCopy;
            // --stop: the colour of the last moving frame and of the still
            // ones until full quality and 5 after, against the converged one.
            HdRenderBuffer *colorOut = controller.GetRenderOutput(HdAovTokens->color);
            auto grabColor = [&](std::vector<float> *out) {
                out->clear();
                if (!colorOut || colorOut->GetFormat() != HdFormatFloat16Vec4) {
                    return;
                }
                const auto *c = static_cast<const GfHalf *>(colorOut->Map());
                const size_t n = size_t(colorOut->GetWidth()) * colorOut->GetHeight();
                out->resize(n);
                for (size_t i = 0; i < n; ++i) {  // luminance
                    (*out)[i] = 0.2126f * float(c[4 * i]) + 0.7152f * float(c[4 * i + 1]) + 0.0722f * float(c[4 * i + 2]);
                }
                colorOut->Unmap();
            };
            std::vector<float> lastMoving;
            const auto runStart = Clock::now();
            for (int f = 0; f < warmup + frames; ++f) {
                // Orbit about the view's centre line: a camera move every frame.
                const double angle = still ? 0.0 : 0.25 * f;
                const GfMatrix4d orbit = GfMatrix4d().SetRotate(GfRotation(GfVec3d(0, 1, 0), angle));
                const auto t0 = Clock::now();
                if (houdini) {  // Houdini's free camera: a prim it edits (and re-sends) each redraw
                    camOp.Set(camXform * orbit.GetInverse());
                    scene.SetTime(UsdTimeCode::EarliestTime());
                } else {
                    controller.SetFreeCameraMatrices(orbit * view, projection);
                }
                engine.Execute(index, &tasks);
                const auto t1 = Clock::now();
                std::map<std::string, double> frameMaps;
                for (auto &[name, b] : buffers) {
                    const auto m0 = Clock::now();
                    const auto *data = static_cast<const uint8_t *>(b->Map());
                    if (houdini) {  // Houdini copies what it maps (to upload it): reads all of it
                        const size_t bytes = size_t(b->GetWidth()) * b->GetHeight() * HdDataSizeOfFormat(b->GetFormat());
                        appCopy.resize(std::max(appCopy.size(), bytes));
                        std::memcpy(appCopy.data(), data, bytes);
                    } else {
                        (void)*static_cast<const volatile uint8_t *>(data);
                    }
                    b->Unmap();
                    frameMaps[name] = Ms(m0, Clock::now());
                }
                if (stopFrames > 0 && f + 1 == warmup + frames) {
                    grabColor(&lastMoving);
                }
                const auto tMapped = Clock::now();
                for (const auto mapped = Clock::now(); Ms(mapped, Clock::now()) < appMs;) {
                }
                const auto t2 = Clock::now();
                if (f >= warmup) {
                    rows.push_back({Ms(runStart, t0), Ms(t0, t1), Ms(t1, tMapped), Ms(t0, t2)});
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
                            if (passCsv && ms.IsHolding<double>()) {
                                std::fprintf(passCsv, "%d,%s,%.4f\n", f - warmup, name.c_str(), ms.UncheckedGet<double>());
                            }
                        }
                    }
                }
            }
            if (const auto *c = houdini ? static_cast<const HdCamera *>(index->GetSprim(HdPrimTypeTokens->camera, camPath))
                                        : nullptr) {
                std::printf("camera %s, shutter %g to %g\n", camPath.GetText(), c->GetShutterOpen(), c->GetShutterClose());
            }
            if (passCsv) {
                std::fclose(passCsv);
            }
            std::printf("%dx%d, %d frames, aovs %s, shareFrame %d%s, app %.1f ms\n", width, height, frames,
                        TfStringJoin(aovs, ",").c_str(), share, still ? ", still" : "", appMs);
            std::sort(times.begin(), times.end());
            const double median = times[times.size() / 2];
            // Pacing: percentiles, hitches (over twice the median) and the
            // latency from a camera edit to the maps that return its image
            // (moving with eldr:overlapReadback: the next frame's).
            auto pct = [&](double p) { return times[std::min(times.size() - 1, size_t(p * times.size()))]; };
            int hitches = 0;
            for (double t : times) {
                hitches += t > 2.0 * median;
            }
            const bool late = !noOverlap && !still;
            std::vector<double> latency;
            for (size_t i = 0; i + 1 < rows.size(); ++i) {
                latency.push_back(late ? rows[i + 1][0] + rows[i + 1][1] + rows[i + 1][2] - rows[i][0]
                                       : rows[i][1] + rows[i][2]);
            }
            std::sort(latency.begin(), latency.end());
            std::printf("  pacing: p50 %.2f p95 %.2f p99 %.2f max %.2f ms, %d hitches (>2x p50); latency camera->image "
                        "p50 %.2f p95 %.2f ms%s\n", pct(0.5), pct(0.95), pct(0.99), times.back(), hitches,
                        latency.empty() ? 0.0 : latency[latency.size() / 2],
                        latency.empty() ? 0.0 : latency[std::min(latency.size() - 1, size_t(0.95 * latency.size()))],
                        late ? " (one frame late)" : "");
            if (!csvPath.empty()) {
                if (FILE *csv = std::fopen(csvPath.c_str(), "w")) {
                    std::fputs("frame,begin_ms,execute_ms,maps_ms,frame_ms\n", csv);
                    for (size_t i = 0; i < rows.size(); ++i) {
                        std::fprintf(csv, "%zu,%.3f,%.3f,%.3f,%.3f\n", i, rows[i][0], rows[i][1], rows[i][2], rows[i][3]);
                    }
                    std::fclose(csv);
                }
            }
            std::printf("  frame %.2f ms (%.1f fps), median %.2f: execute %.2f (Render %.2f), gpu %.2f", total / frames,
                        1000.0 * frames / total, median, executeMs / frames, renderMs / frames, gpuMs / frames);
            for (auto &[name, ms] : mapMs) {
                std::printf(", map %s %.2f", name.c_str(), ms / frames);
            }
            std::printf("\n  gpu passes:");
            for (auto &[name, ms] : passMs) {
                if (ms / frames >= 0.02) {
                    std::printf(" %s %.2f", name.c_str(), ms / frames);
                }
            }
            std::puts("");
            double lastAngle = still ? 0.0 : 0.25 * (warmup + frames - 1);
            if (pauseFrames > 0) {
                // Move -> pause -> move: the last moving frames, the paused
                // ones and the resumed ones should look and cost the same
                // (eldr:stillDelay holds the moving mode through the pause).
                std::puts("  pause: frame, phase, ms since pause, frame ms, gpu ms, render width, annotation");
                const auto p0 = Clock::now();
                for (int f = -3; f < 2 * pauseFrames + 3; ++f) {
                    const bool paused = f >= 0 && f < pauseFrames;
                    lastAngle += paused || still ? 0.0 : 0.25;
                    const GfMatrix4d orbit = GfMatrix4d().SetRotate(GfRotation(GfVec3d(0, 1, 0), lastAngle));
                    const auto t0 = Clock::now();
                    if (houdini) {
                        camOp.Set(camXform * orbit.GetInverse());
                        scene.SetTime(UsdTimeCode::EarliestTime());
                    } else {
                        controller.SetFreeCameraMatrices(orbit * view, projection);
                    }
                    engine.Execute(index, &tasks);
                    for (auto &[name, b] : buffers) {
                        const auto *data = static_cast<const uint8_t *>(b->Map());
                        const size_t bytes = size_t(b->GetWidth()) * b->GetHeight() * HdDataSizeOfFormat(b->GetFormat());
                        appCopy.resize(std::max(appCopy.size(), bytes));
                        std::memcpy(appCopy.data(), data, bytes);
                        b->Unmap();
                    }
                    const double ms = Ms(t0, Clock::now());
                    const VtDictionary stats = delegate->GetRenderStats();
                    auto get = [&](const char *key) {
                        auto it = stats.find(key);
                        return it == stats.end() ? VtValue() : it->second;
                    };
                    std::printf("  %3d %-6s %7.1f %6.2f %6.2f %5zu  %s\n", f, f < 0 ? "move" : paused ? "pause" : "resume",
                                Ms(p0, t0), ms, get("gpuTimeMs").GetWithDefault<double>(0.0),
                                get("renderWidth").GetWithDefault<size_t>(0),
                                get("renderProgressAnnotation").GetWithDefault<std::string>().c_str());
                }
            }
            if (stopFrames > 0) {
                // The camera stops where the orbit ended: frames and ms (from
                // the first still frame's start) until full quality (full
                // size, no moving reductions: stats interactiveReduced), the
                // film's start and convergence.
                const double angle = lastAngle;
                const GfMatrix4d orbit = GfMatrix4d().SetRotate(GfRotation(GfVec3d(0, 1, 0), angle));
                int full = -1, filmAt = -1, convergedAt = -1;
                double fullMs = 0, filmMs = 0, convergedMs = 0, worst = 0;
                std::vector<double> first;
                std::vector<std::vector<float>> stills;  // colour of the still frames until full + 5
                if (houdini) {
                    delegate->SetRenderSetting(TfToken("houdini:interactive"), VtValue(TfToken("normal")));
                }
                const auto s0 = Clock::now();
                for (int f = 0; f < stopFrames && convergedAt < 0; ++f) {
                    const auto t0 = Clock::now();
                    if (houdini) {
                        camOp.Set(camXform * orbit.GetInverse());
                        scene.SetTime(UsdTimeCode::EarliestTime());
                    } else {
                        controller.SetFreeCameraMatrices(orbit * view, projection);
                    }
                    engine.Execute(index, &tasks);
                    for (auto &[name, b] : buffers) {
                        const auto *data = static_cast<const uint8_t *>(b->Map());
                        (void)*static_cast<const volatile uint8_t *>(data);
                        b->Unmap();
                    }
                    const double ms = Ms(t0, Clock::now()), since = Ms(s0, Clock::now());
                    worst = std::max(worst, ms);
                    first.push_back(ms);
                    const VtDictionary stats = delegate->GetRenderStats();
                    auto get = [&](const char *key) {
                        auto it = stats.find(key);
                        return it == stats.end() ? VtValue() : it->second;
                    };
                    const std::string stageNow = get("rendererStage").GetWithDefault<std::string>();
                    const bool reduced = get("interactiveReduced").GetWithDefault<bool>(false) ||
                                         get("renderWidth").GetWithDefault<size_t>(size_t(width)) != size_t(width);
                    if (full < 0 && !reduced) {
                        full = f + 1, fullMs = since;
                    }
                    if (filmAt < 0 && stageNow != "interactive") {
                        filmAt = f + 1, filmMs = since;
                    }
                    if (stageNow == "converged") {
                        convergedAt = f + 1, convergedMs = since;
                    }
                    if (full < 0 || f < full + 5) {
                        stills.emplace_back();
                        grabColor(&stills.back());
                    }
                }
                std::printf("  stop: full quality after %d frames (%.1f ms), film after %d (%.1f ms), converged after %d "
                            "(%.0f ms); first still frames", full, fullMs, filmAt, filmMs, convergedAt, convergedMs);
                // The first four, then the switch to full quality (eldr:stillDelay
                // holds it back) and its neighbours; hitches over twice the moving p50.
                int over = 0;
                for (size_t i = 0; i < first.size(); ++i) {
                    over += first[i] > 2.0 * median;
                    if (i < 4 || (full > 0 && i + 3 >= size_t(full) && i <= size_t(full) + 1)) {
                        std::printf("%s %.1f", i >= 4 && i + 3 == size_t(full) ? " ..." : "", first[i]);
                    }
                }
                std::printf(" ms, worst %.1f ms, %d over 2x moving p50 (%.1f)\n", worst, over, median);
                // The whole move -> stop -> converge sequence: moving and still frames.
                std::vector<double> all = times;
                all.insert(all.end(), first.begin(), first.end());
                std::sort(all.begin(), all.end());
                const double p50 = all[all.size() / 2], p95 = all[std::min(all.size() - 1, size_t(0.95 * all.size()))];
                int over2 = 0;
                for (double t : all) {
                    over2 += t > 2.0 * p50;
                }
                // Noise: RMS luminance error against the last still frame (converged
                // or the last rendered): the last moving frame's, and the worst of
                // the still frames up to 5 after full quality.
                std::vector<float> ref;
                grabColor(&ref);
                auto rms = [&](const std::vector<float> &img) {
                    if (img.size() != ref.size() || ref.empty()) {
                        return 0.0;
                    }
                    double e = 0;
                    for (size_t i = 0; i < ref.size(); ++i) {
                        e += double(img[i] - ref[i]) * (img[i] - ref[i]);
                    }
                    return std::sqrt(e / double(ref.size()));
                };
                double worstStill = 0;
                for (const auto &img : stills) {
                    worstStill = std::max(worstStill, rms(img));
                }
                const double moved = rms(lastMoving);
                std::printf("  sequence: %zu frames, worst %.1f ms, p50 %.1f p95 %.1f (p95/p50 %.2f), %d over 2x p50, "
                            "full quality %.0f ms; noise rms last moving %.4f, worst still %.4f (x%.2f)\n",
                            all.size(), all.back(), p50, p95, p95 / p50, over2, fullMs, moved, worstStill,
                            moved > 0 ? worstStill / moved : 0.0);
            }
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
