"""Invariants on the reflection/refraction scenarios (tests/scenes/scenarios), with Eldr's
default settings and with its traced reflections, refraction and shadows (the scenes'
eldr_rt prim). Not pixel equality with Karma, only what must hold either way:
- mirror_floor_roughness: the red ball shows in the floor where its mirror image projects
  (roughness 0 and 0.2), and beside that image it doesn't (no smear or stretch);
- glass_solid_and_thin: the glass ball, slab and thin pane aren't black;
- furnace_grey (uniform white dome of 1): white diffuse, rough white metal, clear glass and
  coated white read about 1, grey 0.5 reads 0.5, the background 1 (energy neither made nor
  lost). A constant (untextured) dome isn't drawn behind the scene yet: reported as a known bug.
- coloured_glass: the 0.9 m blue block is much bluer than the 0.1 m one, which is visibly blue
  (Beer-Lambert over the path through a scaled cube); the closed thin-walled amber pane tints
  twice (front and back wall);
- glass_behind_glass: the green glass ball seen through the clear pane keeps its shape (darker
  than the wall seen through the pane above it);
- liquid_in_glass (traced): the amber liquid fills the tumbler's inner width, seen through the
  wall off its axis;
- facing_mirrors (traced): past the reflect limit the repeated mirrors are black, as in Karma.
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
    def __init__(self, name, traced):
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
        for key, value in (RT if traced else {}).items():
            engine.SetRendererSetting(key, value)
        engine.SetRenderViewport(Gf.Vec4d(0, 0, W, H))
        engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
        params = UsdImagingGL.RenderParams()
        params.clearColor = Gf.Vec4f(0, 0, 0, 1)
        for _ in range(16):
            engine.Render(self.stage.GetPseudoRoot(), params)
        self.image = fbo.toImage()
        del engine

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

    shot = Shot("coloured_glass", traced)
    ratios = []
    for name, x, z, d in (("0.1 m", -2.2, 0.75, 0.1), ("0.9 m", 0, 0.35, 0.9)):
        c = shot.mean((x, 0.5, z + d / 2))  # the block's front face, the white wall behind
        ratios.append(c[2] / max(c[0], 1e-4))
        print("  block %s %s blue/red %.2f" % (name, fmt(c), ratios[-1]))
    expect(ratios[0] > 1.15, "%s: the 0.1 m blue glass block is blue" % mode)
    expect(ratios[1] > 1.5 * ratios[0], "%s: the 0.9 m blue block is much bluer than the 0.1 m one" % mode)
    c = shot.mean((2.9, 0.7, -0.5))
    print("  amber pane %s" % fmt(c))
    expect(c[1] < 0.5 * c[0], "%s: the closed thin amber pane tints twice (green under half the red)" % mode)

    shot = Shot("glass_behind_glass", traced)
    lum = lambda c: 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]
    ball, above = shot.mean((0.1, 0.7, -0.6), 6), shot.mean((0.1, 1.65, -0.6), 6)  # both through the pane
    print("  green ball %s, wall above it %s" % (fmt(ball), fmt(above)))
    expect(lum(ball) < 0.8 * lum(above), "%s: the green ball shows through the clear pane" % mode)

    if traced:
        shot = Shot("liquid_in_glass", traced)
        c = shot.mean((-0.024, 0.045, 0.028), 2)  # the tumbler's front wall, 0.6 of its radius off axis
        print("  liquid off axis %s" % fmt(c))
        expect(c[0] > 2 * c[2], "%s: the amber liquid fills the tumbler's width" % mode)

        shot = Shot("facing_mirrors", traced)
        c = shot.mean((-1.5, 1.47, 0.89), 2)  # left mirror: a ray bouncing between them past the limit
        print("  mirrors past the limit %s" % fmt(c))
        expect(lum(c) < 0.05, "%s: past the reflect limit the mirrors are black" % mode)

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
