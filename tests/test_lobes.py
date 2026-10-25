"""Headless check of the lobes beyond the metallic base (brdf.glsl) through
Hydra: tests/scenes/lobes.usda rendered with each lobe's setting on and
off (no post processing), comparing the mean colour over each sphere.
Run: usdenv.cmd python tests\\test_lobes.py"""
import math
import os
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject, QOpenGLFramebufferObjectFormat
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Sdf, Usd, UsdGeom, UsdImagingGL

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
on, off = compare("hdvk:clearcoat", "/previewAmber")
expect(on[2] < 0.7 * off[2] and on[0] > 0.8 * off[0], "previewAmber: eldr:clearcoatColor tints the base", on, off)
for prim in ("/velvet", "/fuzz"):
    on, off = compare("hdvk:sheen", prim)
    expect(sum(on) > 1.15 * sum(off), prim[1:] + ": sheen brightens the sphere", on, off)


def hue(rgb):
    return [c / sum(rgb) for c in rgb]


for prim in ("/oilSlick", "/anodized"):
    on, off = compare("hdvk:thinFilm", prim)
    expect(max(abs(a - b) for a, b in zip(hue(on), hue(off))) > 0.05, prim[1:] + ": the film colours it", on, off)
for prim in ("/brushedSteel", "/brushedGold"):
    on, off = compare("hdvk:anisotropicSpecular", prim, lambda image, prim: list(pixels(image, prim)))
    change = sum(abs(a - b) for p, q in zip(on, off) for a, b in zip(p, q)) / sum(sum(p) for p in off)
    expect(change > 0.1, prim[1:] + ": the highlights stretch (%.2f of the image changes)" % change)


def flat(image, prim):
    """The share of a sphere's pixels in its three most common brightness
    levels (2% wide): toon bands make it large."""
    levels = {}
    for c in pixels(image, prim):
        luminance = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]
        if luminance > 0.005:
            key = int(math.log(luminance) / 0.02)
            levels[key] = levels.get(key, 0) + 1
    return sum(sorted(levels.values())[-3:]) / sum(levels.values())


on, off = compare("hdvk:toon", "/toon", flat)
expect(on > 0.6 and off < 0.3, "toon: eldr:toonBands bands the sphere (%.2f flat, %.2f off)" % (on, off))
plain = flat(render({}), "/plain")
everything = flat(render({"hdvk:toonBands": 3}), "/plain")
engine.SetRendererSetting("hdvk:toonBands", 0)
expect(everything > 0.6 and plain < 0.3, "hdvk:toonBands bands every material (%.2f flat, %.2f off)" % (everything, plain))
on, off = compare("hdvk:hairShading", "/hair", lambda image, prim: list(pixels(image, prim)))
change = sum(abs(a - b) for p, q in zip(on, off) for a, b in zip(p, q)) / sum(sum(p) for p in off)
expect(change > 0.1, "hair: strand highlights (%.2f of the image changes)" % change)
# primvars:eldr:hair = false: cables and ropes shade as ordinary surfaces,
# the same with hair shading on and off.
cables = UsdGeom.PrimvarsAPI(stage.GetPrimAtPath("/hair"))
cables.CreatePrimvar("eldr:hair", Sdf.ValueTypeNames.Bool, UsdGeom.Tokens.constant).Set(False)
on, off = compare("hdvk:hairShading", "/hair", lambda image, prim: list(pixels(image, prim)))
cables.RemovePrimvar("eldr:hair")
change = sum(abs(a - b) for p, q in zip(on, off) for a, b in zip(p, q)) / sum(sum(p) for p in off)
expect(change < 0.001, "primvars:eldr:hair = false: no hair shading (%.4f of the image changes)" % change)


def quadrants(image, prim):
    """Mean colours of a quad's bottom-left, bottom-right, top-left and
    top-right quarters."""
    colours = list(pixels(image, prim))  # rows bottom-up, 33 per row
    def quarter(rows, columns):
        picked = [colours[r * 33 + c] for r in rows for c in columns]
        return [sum(p[i] for p in picked) / len(picked) for i in range(3)]
    low, high = range(4, 14), range(19, 29)
    return [quarter(low, low), quarter(low, high), quarter(high, low), quarter(high, high)]


# A textured specularColor: each quarter reflects in its texel's colour,
# and with material programs off in the 0.04 grey stand-in, never black.
red, green, blue, white = quadrants(render({}), "/texturedSpecular")
expect(red[0] > 2 * red[1] and green[1] > 2 * green[0] and blue[2] > 2 * blue[0] and min(white) > 0.0,
       "textured specularColor: each quarter reflects in its colour", red, green, blue, white)
grey = quadrants(render({"hdvk:materialGraphs": False}), "/texturedSpecular")
engine.SetRendererSetting("hdvk:materialGraphs", True)
expect(all(min(q) > 0.2 * max(white) / 25 and max(q) < 1.5 * min(q) for q in grey),
       "textured specularColor without programs: 0.04 grey, not black", *grey)

# A textured thin_film_weight: the film colours the quarters where the
# texture's red is high (bottom-left, top-right), barely the others; with
# Textured Lobe Inputs off the weight is its default 0, the same metal
# everywhere.
film = quadrants(render({}), "/texturedFilm")
bare = quadrants(render({"hdvk:texturedLobes": False}), "/texturedFilm")
engine.SetRendererSetting("hdvk:texturedLobes", True)
shift = [max(abs(a - b) for a, b in zip(hue(f), hue(g))) for f, g in zip(film, bare)]
expect(shift[0] > 3 * shift[1] and shift[3] > 3 * shift[2],
       "textured thin_film_weight: film where the texture is high (hue shifts %s)" % [round(x, 3) for x in shift])
expect(max(abs(a - b) for q in bare for a, b in zip(hue(q), hue(bare[0]))) < 0.01,
       "hdvk:texturedLobes off: the constant weight everywhere", *bare)


# A textured transmission weight: where it is high (bottom-left,
# top-right) the quad lets the dome through, where it is low it stays
# nearly the opaque white it is everywhere with Textured Transmission off.
def luminance(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


clear = quadrants(render({}), "/texturedTransmission")
opaque = quadrants(render({"hdvk:texturedTransmission": False}), "/texturedTransmission")
engine.SetRendererSetting("hdvk:texturedTransmission", True)
shift = [abs(luminance(a) - luminance(b)) / luminance(b) for a, b in zip(clear, opaque)]
expect(shift[0] > 3 * shift[1] and shift[3] > 3 * shift[2],
       "textured transmission: see-through where the texture is high (changes %s)" % [round(x, 3) for x in shift])
expect(max(luminance(q) for q in opaque) < 1.1 * min(luminance(q) for q in opaque),
       "hdvk:texturedTransmission off: opaque everywhere", *opaque)

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
