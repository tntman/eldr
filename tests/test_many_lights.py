"""Many sphere lights over a floor seen in perspective: clustered light lists
(eldr:clusteredLights) give the same image as every light per pixel, across
the whole screen. 200 lights fill each cluster's list (once capped at 63,
which lit only the left third); 300 overflow it (the cluster shades them all).
Run: usdenv.cmd python tests\test_many_lights.py"""
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Usd, UsdGeom, UsdImagingGL, UsdLux

SIZE = 96

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


def render(count, clustered):
    stage = Usd.Stage.CreateInMemory()
    columns = 20
    for i in range(count):  # in index order left to right, row by row
        light = UsdLux.SphereLight.Define(stage, "/l%d" % i)
        light.CreateRadiusAttr(0.05)
        light.CreateIntensityAttr(20.0)
        light.CreateColorAttr(Gf.Vec3f(1.0, 0.6 + 0.4 * (i % 3) / 2, 0.6 + 0.2 * (i % 5) / 4))
        light.AddTranslateOp().Set(Gf.Vec3d(-5 + 0.5 * (i % columns), -1.7, -2 - 0.5 * (i // columns)))
    floor = UsdGeom.Mesh.Define(stage, "/floor")
    floor.CreatePointsAttr([(-20, -2, -40), (20, -2, -40), (20, -2, 5), (-20, -2, 5)])
    floor.CreateFaceVertexCountsAttr([4])
    floor.CreateFaceVertexIndicesAttr([3, 2, 1, 0])
    floor.CreateDisplayColorAttr([(0.5, 0.5, 0.5)])
    cam = UsdGeom.Camera.Define(stage, "/cam")
    cam.CreateFocalLengthAttr(15.0)
    cam.AddTranslateOp().Set(Gf.Vec3d(0, 0, 4))
    frustum = cam.GetCamera(Usd.TimeCode.Default()).frustum
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    for k, v in {"eldr:taa": False, "eldr:autoExposure": False, "eldr:postProcess": False,
                 "eldr:clusteredLights": clustered}.items():
        engine.SetRendererSetting(k, v)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    params.clearColor = Gf.Vec4f(0, 0, 0, 1)
    for _ in range(4):
        engine.Render(stage.GetPseudoRoot(), params)
    image = fbo.toImage()
    del engine
    return image


def mean(img, x0, x1):  # the floor (the lower half), columns x0..x1
    px = [img.pixelColor(x, y) for y in range(SIZE // 2, SIZE) for x in range(x0, x1)]
    return sum(c.redF() + c.greenF() + c.blueF() for c in px) / len(px)


third = SIZE // 3
for count in (200, 300):
    on, off = render(count, True), render(count, False)
    for name, x0, x1 in (("left", 0, third), ("middle", third, 2 * third), ("right", 2 * third, SIZE)):
        a, b = mean(on, x0, x1), mean(off, x0, x1)
        print("%d lights, %s third: clustered %.4f, unclustered %.4f" % (count, name, a, b))
        expect(b > 0.05, "%d lights: the %s third is lit" % (count, name))
        expect(abs(a - b) <= 0.01 * b, "%d lights: clustered within 1%% of unclustered (%s third)" % (count, name))
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
