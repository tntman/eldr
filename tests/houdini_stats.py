r"""hython tests\houdini_stats.py <build-h22>: every entry of viewstats in the
build's houdini/UsdRenderers.json resolves, through the renderstats pane's own
findStats, against the stats of a frame Eldr rendered, and each usdview
Render Statistics row has one. Run with HOUDINI_PACKAGE_DIR=<build-h22>\houdini
and PXR_PLUGINPATH_NAME=<build-h22>; see docs/houdini.md."""
import json
import sys

from hutil.PySide.QtCore import QSize
from hutil.PySide.QtGui import QOffscreenSurface, QOpenGLContext
from hutil.PySide.QtOpenGL import QOpenGLFramebufferObject
from hutil.PySide.QtWidgets import QApplication
from pxr import Gf, Usd, UsdGeom, UsdImagingGL, UsdLux
from renderstats.widgets import findStats

build = sys.argv[1]
info = json.load(open(build + "/houdini/UsdRenderers.json"))["HdVkRendererPlugin"]

app = QApplication.instance() or QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(64, 64))
fbo.bind()

stage = Usd.Stage.CreateInMemory()
UsdGeom.Cube.Define(stage, "/cube")
UsdLux.DistantLight.Define(stage, "/sun")
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 10))
frustum = camera.GetCamera(Usd.TimeCode.Default()).frustum
engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "Eldr not available (PXR_PLUGINPATH_NAME?)"
engine.SetRenderViewport(Gf.Vec4d(0, 0, 64, 64))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
for _ in range(300):
    engine.Render(stage.GetPseudoRoot(), params)
    if engine.IsConverged():
        break
stats = dict(engine.GetRenderStats())
found = findStats(info["statsdatapaths"], stats)
# vram* need VK_EXT_memory_budget and a pass only reports when it ran (a plain
# frame runs few): those are blank in Houdini until then; the rest resolves.
missing = [n for n in info["viewstats"] if n not in found and "Card:" not in n and not n.startswith("GPU passes:")]
for key in stats["gpuPassMs"]:  # every pass that ran has an entry (add new passes to PASSES)
    assert any(p.endswith(".gpuPassMs." + key) for p in info["statsdatapaths"].values()), key
assert len(info["viewstats"]) > 40, len(info["viewstats"])
assert not missing, missing
for name in ("percentDone", "rendererStage", "totalClockTime", "totalMemory", "peakMemory"):
    assert name in found, name
assert found["rendererStage"] == "converged", found["rendererStage"]
print("OK: %d of %d viewstats resolve" % (len([n for n in info["viewstats"] if n in found]), len(info["viewstats"])))
del engine
