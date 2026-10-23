"""Headless check that usdview-style picking works with HdVk (primId and
instanceId AOVs). Same engine calls as usdview's stageView.pick().
Run: usdenv.cmd python tests\\test_selection.py

Selection highlighting (HdxColorizeSelectionTask) reads the same AOVs but
can't be read back here: the engine's Python API has no AOV access and
presentation into an offscreen FBO stays black even for Storm. Check it by
clicking in usdview."""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Sdf, Usd, UsdGeom, UsdImagingGL

SIZE = 256
SCENE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "step4.usda")

# The engine needs a current GL context and a bound framebuffer (as usdrecord does).
app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.Open(SCENE)
camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(camera, CameraUtil.Fit, 1.0)
frustum = camera.frustum
viewProj = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()

engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
root = stage.GetPseudoRoot()


def render():
    for _ in range(10):
        engine.Render(root, params)
        if engine.IsConverged():
            break


def ndc(world):
    p = viewProj.Transform(Gf.Vec3d(*world))
    return Gf.Vec2d(p[0], p[1])


def pick(world):
    narrowed = frustum.ComputeNarrowedFrustum(ndc(world), Gf.Vec2d(1.0 / SIZE, 1.0 / SIZE))
    pickParams = UsdImagingGL.Engine.PickParams()
    pickParams.resolveMode = "resolveNearestToCenter"
    hits = engine.TestIntersection(pickParams, narrowed.ComputeViewMatrix(),
                                   narrowed.ComputeProjectionMatrix(), root, params)
    return hits[0] if hits else None


failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what)


render()

hit = pick((-1.6, 0.9, 2.0))
expect(hit and hit.hitPrimPath == Sdf.Path("/chromeBall"), f"plain prim: {hit and hit.hitPrimPath}")

# Native instancing: picking resolves to the instance proxy path.
hit = pick((0.0, 2.0, -4.5))
expect(hit and hit.hitPrimPath == Sdf.Path("/pillars/b/shaft"), f"native instance: {hit and hit.hitPrimPath}")

# Point 3 is the second "box" instance. HdVk writes the per-prototype index
# (1); Hydra must map it back to point 3 of /scatter.
hit = pick((1.0, 0.45, -1.5))
expect(hit and hit.hitPrimPath == Sdf.Path("/scatter/protos/box"), f"point instancer prim: {hit and hit.hitPrimPath}")
context = [(str(path), index) for path, index in (hit.instancerContext if hit else [])]
expect(("/scatter", 3) in context, f"point instancer index: {context}")

# Sky above the horizon: nothing to pick.
expect(pick((0.0, 30.0, -40.0)) is None, "background picks nothing")

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
