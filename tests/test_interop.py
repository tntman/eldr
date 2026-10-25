"""GPU sharing with OpenGL: with eldr:shareFrame the frame reaches the GL
framebuffer as a texture (no readback) and looks the same as with the
readback; picking still finds the prims (ids and depth read back when
mapped); a new size re-imports the shared images.
Run: usdenv.cmd python tests\\test_interop.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Sdf, Usd, UsdGeom, UsdImagingGL

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)

stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "step4.usda"))
camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(camera, CameraUtil.Fit, 1.0)
frustum = camera.frustum
root = stage.GetPseudoRoot()
params = UsdImagingGL.RenderParams()

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def Render(engine, size):
    fbo = QOpenGLFramebufferObject(QSize(size, size))
    fbo.bind()  # colour only: the composite depth-tests against the target's depth
    engine.SetRenderViewport(Gf.Vec4d(0, 0, size, size))
    for _ in range(40):
        engine.Render(root, params)
        if engine.IsConverged():
            break
    image = fbo.toImage()
    fbo.release()
    return image, engine.GetRenderStats().get("frameSharing")


def Engine(share):
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    engine.SetRendererSetting("eldr:shareFrame", share)
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    return engine


shared = Engine(True)
gpu, mode = Render(shared, 64)
expect(mode == "OpenGL", "frames go to OpenGL on the GPU (got %s)" % mode)
readback = Engine(False)
cpu, mode = Render(readback, 64)
expect(mode == "readback", "eldr:shareFrame off: read back")
worst = max(abs(getattr(gpu.pixelColor(x, y), c)() - getattr(cpu.pixelColor(x, y), c)())
            for x in range(64) for y in range(64) for c in ("red", "green", "blue", "alpha"))
expect(gpu.pixelColor(32, 32).getRgb() != (0, 0, 0, 0), "the shared frame shows the scene")
expect(worst <= 1, "shared and read-back frames match (worst difference %d)" % worst)
del readback

# Picking reads ids and depth back on demand.
centre = frustum.ComputeNarrowedFrustum(Gf.Vec2d(0, 0), Gf.Vec2d(1.0 / 64, 1.0 / 64))
pick = UsdImagingGL.Engine.PickParams()
pick.resolveMode = "resolveNearestToCenter"
hits = shared.TestIntersection(pick, centre.ComputeViewMatrix(), centre.ComputeProjectionMatrix(), root, params)
expect(bool(hits) and hits[0].hitPrimPath != Sdf.Path(), "picking with shared frames")

bigger, mode = Render(shared, 96)
expect(mode == "OpenGL" and bigger.width() == 96 and bigger.pixelColor(48, 48) != bigger.pixelColor(0, 0),
       "a new size shares again")

del shared  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
