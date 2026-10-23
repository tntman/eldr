"""Headless check of HdVk's lights and lighting effects through Hydra:
cylinder lights.
Run: usdenv.cmd python tests\\test_lighting.py"""
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Usd, UsdGeom, UsdImagingGL, UsdLux

SIZE = 64

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
    for _ in range(8):
        engine.Render(stage.GetPseudoRoot(), params)
        if engine.IsConverged():
            break
    image = fbo.toImage()
    del engine  # release GL resources while the context is still current
    return image


def gray(image, x, y):
    """0..255 green at pixel (x, y), y down."""
    return image.pixelColor(x, y).green()


def wall_stage():
    """A white wall at z = 0 seen by /cam from z = 5."""
    stage = Usd.Stage.CreateInMemory()
    wall = UsdGeom.Mesh.Define(stage, "/wall")
    wall.CreatePointsAttr([(-4, -4, 0), (4, -4, 0), (4, 4, 0), (-4, 4, 0)])
    wall.CreateFaceVertexCountsAttr([4])
    wall.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    wall.CreateDisplayColorAttr([(1, 1, 1)])
    camera = UsdGeom.Camera.Define(stage, "/cam")
    camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 5))
    return stage


# Cylinder light: a horizontal tube in front of the wall lights a band
# across it; turned upright, the band turns with it.
stage = wall_stage()
tube = UsdLux.CylinderLight.Define(stage, "/tube")
tube.CreateLengthAttr(4)
tube.CreateRadiusAttr(0.05)
tube.CreateIntensityAttr(20)
tube.AddTranslateOp().Set(Gf.Vec3d(0, 0, 0.2))
rotate = tube.AddRotateZOp()
image = render(stage)
expect(gray(image, 32, 32) > 100, "cylinder light lights the wall")
expect(gray(image, 54, 32) > 2 * gray(image, 32, 54), "cylinder light along x: a horizontal band")
rotate.Set(90)
image = render(stage)
expect(gray(image, 32, 54) > 2 * gray(image, 54, 32), "rotated cylinder light: a vertical band")
image = render(stage, {"hdvk:localLights": False})
expect(gray(image, 32, 32) < 5, "hdvk:localLights off: no cylinder light")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
