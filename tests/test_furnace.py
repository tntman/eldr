"""White furnace for eldr:multiScatter: a rough white OpenPBR metal sphere
(F0 = 1, roughness 0.8) under a uniform white dome of intensity 1 (tests/
scenes/furnace.usda). With multiple scattering (OpenPBR's spec) it conserves
energy and reads 1; off (single scattering) it is darker.
Run: usdenv.cmd python tests\\test_furnace.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject, QOpenGLFramebufferObjectFormat
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Usd, UsdGeom, UsdImagingGL

SIZE = 256

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fboFormat = QOpenGLFramebufferObjectFormat()
fboFormat.setInternalTextureFormat(0x881A)  # GL_RGBA16F
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE), fboFormat)
fbo.bind()

stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "furnace.usda"))
camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(camera, CameraUtil.Fit, 1.0)
frustum = camera.frustum

engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
for key in ("eldr:postProcess", "eldr:ambientOcclusion", "eldr:domeSun", "eldr:ssr"):
    engine.SetRendererSetting(key, False)
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
params.clearColor = Gf.Vec4f(0, 0, 0, 1)


def ball(multiScatter):
    """Mean radiance over the sphere's middle (facing the camera)."""
    engine.SetRendererSetting("eldr:multiScatter", multiScatter)
    for _ in range(3):
        engine.Render(stage.GetPseudoRoot(), params)
    image = fbo.toImage()
    values = []
    for y in range(SIZE // 2 - 6, SIZE // 2 + 7):
        for x in range(SIZE // 2 - 6, SIZE // 2 + 7):
            c = image.pixelColor(x, y)
            values.append((c.redF() + c.greenF() + c.blueF()) / 3)
    return sum(values) / len(values)


on, off = ball(True), ball(False)
print("multiScatter on %.3f, off %.3f" % (on, off))
failed = not (0.96 <= on <= 1.04) or not (0.5 < off < 0.9 * on)
print("FAILED" if failed else "OK")
sys.exit(1 if failed else 0)
