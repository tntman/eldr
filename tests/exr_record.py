"""Renders a stage's active render settings' products with chosen renderer
settings until they converge (tests/test_exr.py's helper; usdrecord has no
way to set them).
Run: usdenv.cmd python tests\\exr_record.py <scene> <settings prim> <camera> key=value..."""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL

SIZE = int(os.environ.get("EXR_SIZE", 64))
scene, settingsPrim, cameraPath = sys.argv[1:4]

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.Open(scene)
frustum = UsdGeom.Camera(stage.GetPrimAtPath(cameraPath)).GetCamera(Usd.TimeCode.Default()).frustum
engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
engine.SetActiveRenderSettingsPrimPath(Sdf.Path(settingsPrim))
for pair in sys.argv[4:]:
    key, value = pair.split("=", 1)
    try:  # ints and floats as such (int settings ignore strings)
        value = {"true": True, "false": False}[value]
    except KeyError:
        for kind in (int, float):
            try:
                value = kind(value)
                break
            except ValueError:
                pass
    engine.SetRendererSetting(key, value)
params = UsdImagingGL.RenderParams()
for _ in range(400):
    engine.Render(stage.GetPseudoRoot(), params)
    if engine.IsConverged():
        break
engine.Render(stage.GetPseudoRoot(), params)  # a converged frame writes the products
del engine  # before the GL context goes
