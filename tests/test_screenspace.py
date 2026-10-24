"""Headless check of HdVk's screen-space effects through Hydra: the
thickness estimator (SSR at grazing angles).
Run: usdenv.cmd python tests\\test_screenspace.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Usd, UsdGeom, UsdImagingGL

SIZE = 400

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def render(stage, settings={}):
    """The stage through /cam with HdVk, as a QImage; no TAA, fixed exposure."""
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    for key, value in {"hdvk:taa": False, "hdvk:autoExposure": False, **settings}.items():
        engine.SetRendererSetting(key, value)
    frustum = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default()).frustum
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    params.clearColor = Gf.Vec4f(0, 0, 0, 1)
    for _ in range(4):
        engine.Render(stage.GetPseudoRoot(), params)
    image = fbo.toImage()
    del engine  # release GL resources while the context is still current
    return image


def mean(image, x0, y0, x1, y1):
    """Mean 0..255 green over [x0, x1) x [y0, y1) (y down)."""
    total = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            total += image.pixelColor(x, y).green()
    return total / ((x1 - x0) * (y1 - y0))


# The Cornell box from floor height: the chrome sphere's lower half
# reflects the floor, a strip a few pixels high on screen. Marching even
# steps, SSR skips over it on most rays (black grain); the estimator
# searches the jump in depth past it pixel by pixel.
stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "cornell.usda"))
cam = UsdGeom.Xformable(stage.GetPrimAtPath("/cam"))
cam.ClearXformOpOrder()
cam.AddTranslateOp().Set(Gf.Vec3d(0.2, 0.06, 2.6))
region = (200, 160, 280, 200)  # the sphere's lower half
off = mean(render(stage, {"hdvk:ssgiThickness": False}), *region)
on = mean(render(stage, {"hdvk:ssgiThickness": True}), *region)
print("SSR at floor height: the floor's reflection %.1f without the thickness estimator, %.1f with it" % (off, on))
expect(on > 1.3 * off, "the thickness estimator fills SSR's grain at grazing angles")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
