"""Headless check of the lobes beyond the metallic base (brdf.glsl) through
Hydra: tests/scenes/lobes.usda rendered with each lobe's setting on and
off (no post processing), comparing the mean colour over each sphere.
Run: usdenv.cmd python tests\\test_lobes.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Usd, UsdGeom, UsdImagingGL

SIZE = 384

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "lobes.usda"))
camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(camera, CameraUtil.Fit, 1.0)
frustum = camera.frustum
viewProj = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()

engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRendererSetting("hdvk:postProcess", False)
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
params.clearColor = Gf.Vec4f(0, 0, 0, 1)


def render(settings):
    for key, value in settings.items():
        engine.SetRendererSetting(key, value)
    for _ in range(2):  # textures arrive with the first commit
        engine.Render(stage.GetPseudoRoot(), params)
    return fbo.toImage()


def mean(image, prim):
    """Mean linear colour over the middle of a sphere (radius 1)."""
    centre = stage.GetPrimAtPath(prim).GetAttribute("xformOp:translate").Get()
    total, count = [0.0, 0.0, 0.0], 0
    for dy in range(-6, 7):
        for dx in range(-6, 7):
            p = viewProj.Transform(Gf.Vec3d(centre[0] + dx * 0.1, centre[1] + dy * 0.1, centre[2]))
            c = image.pixelColor(int((p[0] * 0.5 + 0.5) * SIZE), int((0.5 - p[1] * 0.5) * SIZE))
            for i, v in enumerate((c.redF(), c.greenF(), c.blueF())):
                total[i] += v
            count += 1
    return [v / count for v in total]


failures = []


def expect(ok, what, *values):
    print("ok:  " if ok else "FAIL:", what, *[[round(v, 3) for v in rgb] for rgb in values])
    if not ok:
        failures.append(what)


def compare(setting, prim):
    """The sphere's mean colour with `setting` on and off."""
    on = mean(render({setting: True}), prim)
    off = mean(render({setting: False}), prim)
    engine.SetRendererSetting(setting, True)
    return on, off


on, off = compare("hdvk:specularWorkflow", "/specularWorkflow")
expect(on[0] - on[2] > off[0] - off[2] + 0.02, "specular workflow: orange specularColor highlights", on, off)

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
