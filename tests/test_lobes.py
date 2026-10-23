"""Headless check of the lobes beyond the metallic base (brdf.glsl) through
Hydra: tests/scenes/lobes.usda rendered with each lobe's setting on and
off (no post processing), comparing the mean colour over each sphere.
Run: usdenv.cmd python tests\\test_lobes.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject, QOpenGLFramebufferObjectFormat
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Usd, UsdGeom, UsdImagingGL

SIZE = 384

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fboFormat = QOpenGLFramebufferObjectFormat()
fboFormat.setInternalTextureFormat(0x881A)  # GL_RGBA16F: highlights stay above 1
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE), fboFormat)
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


def pixels(image, prim):
    """Linear colours over the middle of a sphere (radius 1)."""
    centre = stage.GetPrimAtPath(prim).GetAttribute("xformOp:translate").Get()
    for dy in range(-16, 17):
        for dx in range(-16, 17):
            p = viewProj.Transform(Gf.Vec3d(centre[0] + dx * 0.05, centre[1] + dy * 0.05, centre[2]))
            c = image.pixelColor(int((p[0] * 0.5 + 0.5) * SIZE), int((0.5 - p[1] * 0.5) * SIZE))
            yield c.redF(), c.greenF(), c.blueF()


def mean(image, prim):
    colours = list(pixels(image, prim))
    return [sum(c[i] for c in colours) / len(colours) for i in range(3)]


def peak(image, prim):
    """The brightest colour (by its sum) over the middle of a sphere."""
    return list(max(pixels(image, prim), key=sum))


failures = []


def expect(ok, what, *values):
    print("ok:  " if ok else "FAIL:", what, *[[round(v, 3) for v in rgb] for rgb in values])
    if not ok:
        failures.append(what)


def compare(setting, prim, measure=mean):
    """The sphere's mean (or peak) colour with `setting` on and off."""
    on = measure(render({setting: True}), prim)
    off = measure(render({setting: False}), prim)
    engine.SetRendererSetting(setting, True)
    return on, off


on, off = compare("hdvk:specularWorkflow", "/specularWorkflow")
expect(on[0] - on[2] > off[0] - off[2] + 0.02, "specular workflow: orange specularColor highlights", on, off)
for prim in ("/clearcoat", "/carPaint", "/amberCoat"):
    on, off = compare("hdvk:clearcoat", prim, peak)
    expect(sum(on) > 1.5 * sum(off), prim[1:] + ": the coat's sharp highlight", on, off)
on, off = compare("hdvk:clearcoat", "/amberCoat")
expect(on[2] < 0.7 * off[2] and on[0] > 0.8 * off[0], "amberCoat: the coat colour tints the base", on, off)
for prim in ("/velvet", "/fuzz"):
    on, off = compare("hdvk:sheen", prim)
    expect(sum(on) > 1.15 * sum(off), prim[1:] + ": sheen brightens the sphere", on, off)

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
