"""Headless check of HdVk's render statistics (GetRenderStats) and of the
usdview window's rows built from them (usdview/hdvkUsdview).
Run: usdenv.cmd python tests\\test_stats.py"""
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Usd, UsdGeom, UsdImagingGL, UsdLux

import hdvkUsdview

SIZE = 64

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

# Two cubes (12 triangles each), one behind the camera (culled).
stage = Usd.Stage.CreateInMemory()
for name, z in (("front", 0), ("behind", 20)):
    UsdGeom.Cube.Define(stage, f"/{name}").AddTranslateOp().Set(Gf.Vec3d(0, 0, z))
UsdLux.DistantLight.Define(stage, "/sun")
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 10))
frustum = camera.GetCamera(Usd.TimeCode.Default()).frustum

engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
# Houdini's viewport may poll the stats before the first frame: no crash, sane defaults.
engine.SetRendererSetting("houdini:viewport", True)
early = engine.GetRenderStats()
engine.SetRendererSetting("houdini:viewport", False)
# A fresh film runs in final quality: "final", until it converges.
engine.Render(stage.GetPseudoRoot(), params)
first = engine.GetRenderStats()
expect_first = (first.get("rendererStage"), first.get("renderProgressAnnotation", ""))
for _ in range(200):  # until the film converges
    engine.Render(stage.GetPseudoRoot(), params)
    if engine.IsConverged():
        break
stats = engine.GetRenderStats()

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


expect(stats.get("rendererName") == "Eldr", "rendererName")
expect(early.get("rendererName") == "Eldr" and early.get("trianglesDrawn", 0) == 0
       and early.get("rendererStage") in ("interactive", "converged"), "stats before the first frame: %s" % (dict(early),))
expect(expect_first[0] == "final" and expect_first[1].startswith("final 1/"), "a fresh film is final: %s" % (expect_first,))
expect(stats.get("trianglesTotal") == 24 and stats.get("trianglesDrawn") == 12, "triangles drawn after culling")
expect(stats.get("instancesTotal") == 2 and stats.get("instancesDrawn") == 1, "instances drawn after culling")
expect(stats.get("gpuTimeMs", 0) > 0 and "shading" in stats.get("gpuPassMs", {}), "GPU time per pass")
expect(set(stats["gpuPassMs"]) <= {k for k, _ in hdvkUsdview.PASSES},
       "every pass that ran is in PASSES: %s" % (set(stats["gpuPassMs"]) - {k for k, _ in hdvkUsdview.PASSES}))
expect(abs(sum(stats["gpuPassMs"].values()) - stats["gpuTimeMs"]) < 1e-3, "passes add up to the frame")
expect(stats.get("numCompletedSamples") == stats.get("taaTarget") == 64, "film samples")
expect(stats.get("percentDone") == 100.0 and stats.get("rendererStage") == "converged", "converged")
expect(stats.get("gpuMemoryUsed", 0) > stats.get("primvar", 0) > 0, "graphics memory")
# Houdini's viewport text (houdini/UsdRenderers.json maps these).
expect(stats.get("renderProgressAnnotation", "").startswith("final 64/64") and "triangles" in stats.get("renderStatsAnnotation", ""),
       "annotations")
expect(stats.get("totalClockTime", 0) > 0 and stats.get("pixelsConvergedPercent") == 100.0, "time to converge")
# Interactive TAA is not counted up: it reads "interactive 100%".
engine.SetRendererSetting("eldr:filmAccumulation", False)
moved = frustum.ComputeViewMatrix() * Gf.Matrix4d().SetTranslate(Gf.Vec3d(0.3, 0, 0))
engine.SetCameraState(moved, frustum.ComputeProjectionMatrix())
engine.Render(stage.GetPseudoRoot(), params)
tmp = engine.GetRenderStats()
expect(tmp.get("rendererStage") == "interactive" and tmp.get("percentDone") == 100.0, "interactive TAA stage")
engine.SetRendererSetting("eldr:filmAccumulation", True)
# The film that follows a camera move starts from the interactive image: "accumulating", not "final".
stages = set()
for _ in range(100):
    engine.Render(stage.GetPseudoRoot(), params)
    stages.add(engine.GetRenderStats().get("rendererStage"))
    if engine.IsConverged():
        break
print("stages after a move:", sorted(stages))
expect("accumulating" in stages, "a film after a camera move is accumulating")
# A camera jump that also resizes (a fresh history, as a Solaris camera switch)
# stays interactive: only the renderer's first frame starts a final film.
for viewport in (SIZE, SIZE // 2):
    engine.SetRenderViewport(Gf.Vec4d(0, 0, viewport, viewport))
    jump = frustum.ComputeViewMatrix() * Gf.Matrix4d().SetTranslate(Gf.Vec3d(-1.0, 0.5, 0))
    engine.SetCameraState(jump if viewport != SIZE else frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    jumped = set()
    for _ in range(4):
        engine.Render(stage.GetPseudoRoot(), params)
        jumped.add(engine.GetRenderStats().get("rendererStage"))
    expect("final" not in jumped and (viewport == SIZE or "accumulating" in jumped),
           "a camera jump at %d px is not final: %s" % (viewport, sorted(jumped)))
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
rows = dict(hdvkUsdview.Rows(stats))
expect(rows.get("Triangles drawn") == "12" and rows.get("Triangles in scene") == "24" and "Shading" in rows,
       "the usdview window's rows")
# Every stat in the window's table is reported (they become Houdini's viewstats).
for heading, entries in hdvkUsdview.STATS:
    if entries == "PASSES":
        entries = [(label, "gpuPassMs." + key, "ms") for key, label in hdvkUsdview.PASSES]
    for label, key, unit in entries:
        # (loadLine: only after a load over 0.25 s; passes that did not run, and the card's budget without VK_EXT_memory_budget, are absent)
        expect(key.startswith("gpuPassMs.") or key.startswith("vram") or key == "loadLine" or hdvkUsdview.Lookup(stats, key) is not None,
               "%s (%s) is reported" % (label, key))

# Ray-traced shadows (where the GPU has ray tracing pipelines) add their pass
# and acceleration structures; switched off, both are gone again.
if stats.get("rayTracingSupported"):
    engine.SetRendererSetting("eldr:rtShadows", True)
    engine.Render(stage.GetPseudoRoot(), params)
    traced = engine.GetRenderStats()
    expect("rayTracing" in traced["gpuPassMs"] and traced.get("accelerationStructureMemory", 0) > 0,
           "eldr:rtShadows traces")
    engine.SetRendererSetting("eldr:rtShadows", False)
    engine.Render(stage.GetPseudoRoot(), params)
    expect(engine.GetRenderStats().get("accelerationStructureMemory") == 0, "ray tracing off frees its memory")

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
