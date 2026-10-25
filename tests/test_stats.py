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
expect(stats.get("trianglesTotal") == 24 and stats.get("trianglesDrawn") == 12, "triangles drawn after culling")
expect(stats.get("instancesTotal") == 2 and stats.get("instancesDrawn") == 1, "instances drawn after culling")
expect(stats.get("gpuTimeMs", 0) > 0 and "shading" in stats.get("gpuPassMs", {}), "GPU time per pass")
expect(abs(sum(stats["gpuPassMs"].values()) - stats["gpuTimeMs"]) < 1e-3, "passes add up to the frame")
expect(stats.get("numCompletedSamples") == stats.get("taaTarget") == 64, "film samples")
expect(stats.get("percentDone") == 100.0 and stats.get("rendererStage") == "converged", "converged")
expect(stats.get("gpuMemoryUsed", 0) > stats.get("primvar", 0) > 0, "graphics memory")
rows = dict(hdvkUsdview.Rows(stats))
expect(rows.get("Triangles") == "12 of 24" and "Shading" in rows, "the usdview window's rows")

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
