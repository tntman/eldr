"""Extra AOVs through Hydra, as usdview's AOV menu shows them: the menu
lists normal, Neye and primvars:st; the normal AOV alone renders (usdview
binds only the AOV it shows) and is the cube's facing normal, as is Karma's
ray:hitN (shown raw); Karma's Cryptomatte object AOV (primvars:__name)
fills the manifests, which name the cube and its material by MurmurHash3.
Run: usdenv.cmd python tests\\test_aovs.py"""
import json
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Usd, UsdGeom, UsdImagingGL, UsdLux, UsdRender, UsdShade

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
cube = UsdGeom.Cube.Define(stage, "/front")
material = UsdShade.Material.Define(stage, "/look")
UsdShade.MaterialBindingAPI.Apply(cube.GetPrim()).Bind(material)
UsdLux.DistantLight.Define(stage, "/sun")
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 10))
frustum = camera.GetCamera(Usd.TimeCode.Default()).frustum

engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def Render(aov):
    engine.SetRendererAov(aov)
    for _ in range(2):
        engine.Render(stage.GetPseudoRoot(), params)
    return fbo.toImage().pixelColor(SIZE // 2, SIZE // 2)


def Murmur3(name):
    data, h, n = name.encode(), 0, len(name.encode())
    rotl = lambda x, r: ((x << r) | (x >> (32 - r))) & 0xFFFFFFFF
    mix = lambda k: (rotl((k * 0xCC9E2D51) & 0xFFFFFFFF, 15) * 0x1B873593) & 0xFFFFFFFF
    for i in range(0, n // 4 * 4, 4):
        h = (rotl(h ^ mix(int.from_bytes(data[i:i + 4], "little")), 13) * 5 + 0xE6546B64) & 0xFFFFFFFF
    if n % 4:
        h ^= mix(int.from_bytes(data[n // 4 * 4:], "little"))
    h ^= n
    h = ((h ^ (h >> 16)) * 0x85EBCA6B) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 0xC2B2AE35) & 0xFFFFFFFF
    h ^= h >> 16
    return h ^ (1 << 23) if (h >> 23) & 0xFF in (0, 255) else h


aovs = [str(a) for a in engine.GetRendererAovs()]
expect(all(a in aovs for a in ("normal", "Neye", "primvars:st")), "the AOV menu lists normal, Neye, primvars:st")

# usdview shows a normal as 0.5 n + 0.5: the front face's +z.
c = Render("normal")
expect(abs(c.redF() - 0.5) < 0.03 and abs(c.greenF() - 0.5) < 0.03 and c.blueF() > 0.97, "normal AOV: +z")

# Karma's names, which usdview shows raw: N (+z) and P (the face at z = 1).
c = Render("ray:hitN")
expect(c.redF() < 0.03 and c.greenF() < 0.03 and c.blueF() > 0.97, "Karma's N (ray:hitN): +z")
c = Render("ray:hitPz")
expect(c.redF() > 0.97, "Karma's depth (ray:hitPz): 9 units away, shown clamped to 1")

Render("primvars:__name")
manifests = engine.GetRenderStats().get("cryptomatte", {})
objects = json.loads(manifests.get("CryptoObject", "{}"))
materials = json.loads(manifests.get("CryptoMaterial", "{}"))
expect(objects.get("/front") == "%08x" % Murmur3("/front"), "Cryptomatte manifest: the cube's id")
expect(materials.get("/look") == "%08x" % Murmur3("/look"), "Cryptomatte manifest: its material's id")

# Houdini's viewport binds a render var by its prim's name alone (no
# sourceName/sourceType): Eldr takes the source from the settings prim's var.
settings = UsdRender.Settings.Define(stage, "/Render/Settings")
product = UsdRender.Product.Define(stage, "/Render/Product")
var = UsdRender.Var.Define(stage, "/Render/Vars/rendervar1")
var.CreateSourceNameAttr("ray:hitN")
var.CreateSourceTypeAttr("raw")
var.CreateDataTypeAttr("float3")
product.GetOrderedVarsRel().AddTarget(var.GetPath())
settings.GetProductsRel().AddTarget(product.GetPath())
engine.SetActiveRenderSettingsPrimPath(settings.GetPath())
Render("normal")  # the settings prim syncs
c = Render("rendervar1")
expect(c.redF() < 0.03 and c.greenF() < 0.03 and c.blueF() > 0.97, "a render var bound by its prim's name: its source (ray:hitN)")

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
