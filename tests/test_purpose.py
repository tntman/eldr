"""Headless check that HdVk draws only the purposes (render tags) the app
asks for, also after toggling them, as usdview's Display Purposes menu does.
Picking reads HdVk's primId AOV, so a hit means the prim was drawn.
Run: usdenv.cmd python tests\\test_purpose.py"""
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Sdf, Usd, UsdGeom, UsdImagingGL

SIZE = 64

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

# A render-purpose cube left of the camera axis, a proxy-purpose cube right.
stage = Usd.Stage.CreateInMemory()
for name, x, purpose in (("renderCube", -1.5, UsdGeom.Tokens.render), ("proxyCube", 1.5, UsdGeom.Tokens.proxy)):
    cube = UsdGeom.Cube.Define(stage, f"/{name}")
    cube.AddTranslateOp().Set(Gf.Vec3d(x, 0, 0))
    cube.CreatePurposeAttr(purpose)
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 10))
gfCamera = camera.GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(gfCamera, CameraUtil.Fit, 1.0)
frustum = gfCamera.frustum
viewProj = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()

engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
root = stage.GetPseudoRoot()


def drawn(world):
    """The prim HdVk drew at a world point after rendering with params."""
    engine.Render(root, params)
    p = viewProj.Transform(Gf.Vec3d(*world))
    narrowed = frustum.ComputeNarrowedFrustum(Gf.Vec2d(p[0], p[1]), Gf.Vec2d(1.0 / SIZE, 1.0 / SIZE))
    pickParams = UsdImagingGL.Engine.PickParams()
    pickParams.resolveMode = "resolveNearestToCenter"
    hits = engine.TestIntersection(pickParams, narrowed.ComputeViewMatrix(),
                                   narrowed.ComputeProjectionMatrix(), root, params)
    return hits[0].hitPrimPath if hits else None


failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what)


left, right = (-1.5, 0, 1), (1.5, 0, 1)
for showRender, showProxy in ((True, True), (False, True), (True, False), (False, False), (True, True)):
    params.showRender = showRender
    params.showProxy = showProxy
    hit = drawn(left)
    expect((hit == Sdf.Path("/renderCube")) == showRender, f"render purpose {showRender}: {hit}")
    hit = drawn(right)
    expect((hit == Sdf.Path("/proxyCube")) == showProxy, f"proxy purpose {showProxy}: {hit}")

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
