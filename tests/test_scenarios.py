"""Invariants on the reflection/refraction scenarios (tests/scenes/scenarios), with Eldr's
default settings and with its traced reflections, refraction and shadows (the scenes'
eldr_rt prim). Not pixel equality with Karma, only what must hold either way:
- mirror_floor_roughness: the red ball shows in the floor where its mirror image projects
  (roughness 0 and 0.2), and beside that image it doesn't (no smear or stretch);
- glass_solid_and_thin: the glass ball, slab and thin pane aren't black;
- furnace_grey (uniform white dome of 1): white diffuse, rough white metal, clear glass and
  coated white read about 1, grey 0.5 reads 0.5, the background 1 (energy neither made nor
  lost). A constant (untextured) dome isn't drawn behind the scene yet: reported as a known bug;
- sun_on_chrome: the colour limit caps the chrome sun highlights (every camera sample, as Karma).
Needs a GPU with ray tracing for the traced half.
Run: usdenv.cmd python tests\\test_scenarios.py"""
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject, QOpenGLFramebufferObjectFormat
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Usd, UsdGeom, UsdImagingGL

W, H = 384, 216
SCENES = os.path.join(os.path.dirname(__file__), "scenes", "scenarios")
RT = {"eldr:rtReflections": True, "eldr:rtRefraction": True, "eldr:rtShadows": True, "eldr:rtHitShadows": True}

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fboFormat = QOpenGLFramebufferObjectFormat()
fboFormat.setInternalTextureFormat(0x881A)  # GL_RGBA16F
fbo = QOpenGLFramebufferObject(QSize(W, H), fboFormat)
fbo.bind()


class Shot:
    def __init__(self, name, traced, settings={}):
        self.stage = Usd.Stage.Open(os.path.join(SCENES, name + ".usda"))
        camera = UsdGeom.Camera(self.stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
        CameraUtil.ConformWindow(camera, CameraUtil.Fit, W / H)
        frustum = camera.frustum
        self.toNdc = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()
        engine = UsdImagingGL.Engine()
        assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
        # Linear radiance with the post steps (screen-space reflections) kept.
        for key, value in {"eldr:tonemapper": "none", "eldr:autoExposure": False, "eldr:bloomIntensity": 0.0}.items():
            engine.SetRendererSetting(key, value)
        for key, value in dict(RT if traced else {}, **settings).items():
            engine.SetRendererSetting(key, value)
        engine.SetRenderViewport(Gf.Vec4d(0, 0, W, H))
        engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
        params = UsdImagingGL.RenderParams()
        params.clearColor = Gf.Vec4f(0, 0, 0, 1)
        for _ in range(16):
            engine.Render(self.stage.GetPseudoRoot(), params)
        self.image = fbo.toImage()
        del engine

    def peak(self):
        """The brightest channel anywhere in the image."""
        return max(max(c.redF(), c.greenF(), c.blueF())
                   for c in (self.image.pixelColor(x, y) for y in range(H) for x in range(W)))

    def pixel(self, point):
        p = self.toNdc.Transform(Gf.Vec3d(*point))
        return int((p[0] + 1) / 2 * W), int((1 - p[1]) / 2 * H)

    def mean(self, point, r=3):
        """Mean RGB over a (2r+1)^2 window around the point's pixel."""
        x0, y0 = self.pixel(point)
        assert r <= x0 < W - r and r <= y0 < H - r, "%s projects off the image" % (point,)
        cs = [self.image.pixelColor(x, y) for y in range(y0 - r, y0 + r + 1) for x in range(x0 - r, x0 + r + 1)]
        return tuple(sum(getattr(c, ch)() for c in cs) / len(cs) for ch in ("redF", "greenF", "blueF"))


failures = []


def expect(ok, what):
    print("ok:  " if ok else "FAIL:", what)
    if not ok:
        failures.append(what)


def known(ok, what):
    """A known Eldr bug (tests/scenes/scenarios/README.md, Known differences): reported, not failed.
    The lane that fixes it turns the call into expect()."""
    print("ok:  " if ok else "KNOWN BUG:", what)


def fmt(c):
    return "(%s)" % ", ".join("%.3f" % v for v in c)


for traced in (False, True):
    mode = "traced" if traced else "default"

    shot = Shot("mirror_floor_roughness", traced)
    for i in (0, 1):  # tiles at roughness 0 and 0.2; ball i at (x - 0.4, 0.6, -0.5), radius 0.6
        x = (i - 1) * 3.2 - 0.4
        image = shot.mean((x, -0.6, -0.5))  # the ball's centre mirrored in the floor
        print("  tile %d: mirrored ball %s" % (i, fmt(image)))
        expect(image[0] > 1.5 * image[1] and image[0] > 1.5 * image[2],
               "%s: the red ball reflects in floor tile %d where its mirror image is" % (mode, i))
    beside = shot.mean((-1.5, -0.6, -0.5))  # 0.5 left of the middle tile's mirrored ball, still on the tile
    print("  beside it %s" % fmt(beside))
    expect(not (beside[0] > 1.5 * beside[1] and beside[0] > 1.5 * beside[2]),
           "%s: no red smear on the floor beside the reflected ball" % mode)

    shot = Shot("glass_solid_and_thin", traced)
    for name, point in (("ball", (-1.8, 0.8, 0)), ("slab", (0, 0.9, 0)), ("pane", (1.8, 0.9, 0))):
        c = shot.mean(point)
        print("  %s %s" % (name, fmt(c)))
        expect(sum(c) / 3 > 0.15, "%s: the glass %s isn't black" % (mode, name))

    shot = Shot("furnace_grey", traced)
    for name, x, lo, hi in (("white", -3.2, 0.95, 1.05), ("grey", -1.6, 0.46, 0.54), ("roughMetal", 0, 0.93, 1.05),
                            ("glass", 1.6, 0.93, 1.06), ("coated", 3.2, 0.93, 1.05)):
        v = sum(shot.mean((x, 0, 0.65), 2)) / 3  # the sphere's point facing the camera
        print("  %s %.3f" % (name, v))
        expect(lo <= v <= hi, "%s: furnace %s reads %.2f-%.2f" % (mode, name, lo, hi))
    v = sum(shot.mean((0, 1.6, 0), 2)) / 3
    known(0.97 <= v <= 1.03, "%s: a constant (untextured) dome shows behind the scene, reads 1 (%.3f)" % (mode, v))

# Karma's colour limit caps every camera sample (chrome sun highlights too), not only
# directly seen emitters. A small limit keeps the check inside the image's range.
for traced in (False, True):
    mode = "traced" if traced else "default"
    limited = Shot("sun_on_chrome", traced, {"eldr:colorLimit": 0.5}).peak()
    full = Shot("sun_on_chrome", traced, {"eldr:colorLimit": 0.0}).peak()
    print("  sun_on_chrome peak: limit 0.5 %.3f, off %.3f" % (limited, full))
    expect(limited <= 0.52 and full > 0.7, "%s: the colour limit caps the chrome highlights" % mode)

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
