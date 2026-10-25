"""Light shapes through Hydra: with eldr:lightShapesVisible two sphere lights
show to the camera as their radiance, except one with Karma's
inputs:karma:light:renderlightgeo = false ("Render Light Geometry").
Run: usdenv.cmd python tests\test_light_shapes.py"""
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux

SIZE = 64

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.CreateInMemory()
for name, x, geo in (("shown", -1.0, None), ("hidden", 1.0, False)):
    light = UsdLux.SphereLight.Define(stage, "/" + name)
    light.CreateRadiusAttr(0.4)
    light.CreateIntensityAttr(20.0)
    light.AddTranslateOp().Set(Gf.Vec3d(x, 0, 0))
    if geo is not None:
        light.GetPrim().CreateAttribute("inputs:karma:light:renderlightgeo", Sdf.ValueTypeNames.Bool).Set(geo)
wall = UsdGeom.Mesh.Define(stage, "/wall")
wall.CreatePointsAttr([(-4, -4, -30), (4, -4, -30), (4, 4, -30), (-4, 4, -30)])
wall.CreateFaceVertexCountsAttr([4])
wall.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
wall.CreateDisplayColorAttr([(0.2, 0.2, 0.2)])
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.CreateProjectionAttr("orthographic")
camera.CreateHorizontalApertureAttr(40.0)
camera.CreateVerticalApertureAttr(40.0)
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 5))
frustum = camera.GetCamera(Usd.TimeCode.Default()).frustum

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRendererSetting("eldr:postProcess", False)
engine.SetRendererSetting("eldr:lightShapesVisible", True)
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
params.clearColor = Gf.Vec4f(0, 0, 0, 1)
for _ in range(8):
    engine.Render(stage.GetPseudoRoot(), params)
    if engine.IsConverged():
        break
image = fbo.toImage()


def lum(x):  # world x on the row of the lights
    c = image.pixelColor(int((x + 2) / 4 * SIZE), SIZE // 2)
    return max(c.redF(), c.greenF(), c.blueF())


expect(lum(-1.0) > 0.5, "a sphere light shows its shape")
expect(lum(1.0) < 0.2, "inputs:karma:light:renderlightgeo = false hides it")
del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
