"""Volume velocity blur (Karma's Volume Velocity Blur, Lookup Offset) and
field roles, read from render products (EXR): a density ball moving 24
units a second (1 a frame) along x, its velocity a Vec3f grid "vel" and
Houdini's three float volumes vel.x/.y/.z (make_volume --moving), seen by
a camera whose shutter is open half a frame.
- Without Velocity Blur (karma:object:vblur) the ball is sharp; with it
  (Karma's or eldr:object:vblur) it is half a unit wider, from the Vec3f
  grid or from the three float grids (bound as vel_x, vel_y, vel_z).
- Volume Velocity Blur Scale 0 (karma:/eldr:object:volumevblurscale) and
  the render setting eldr:volumeVelocityBlur off keep it sharp.
- A nameless field (SOP Import's field_0) is the density beside a
  colour field that sorts before it.
Run: usdenv.cmd python tests\\test_volume_motion.py <make_volume> <exrinfo>"""
import os
import subprocess
import sys
import tempfile

import exr_info

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux, UsdRender, UsdVol

SIZE = 64
folder = tempfile.mkdtemp(prefix="eldr_volume_motion_")
vdb = os.path.join(folder, "moving.vdb")
subprocess.check_call([sys.argv[1], "--moving", vdb], timeout=600)
exrinfo = sys.argv[2]
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


scenePath = os.path.join(folder, "scene.usda")
exrPath = os.path.join(folder, "beauty.exr").replace("\\", "/")
stage = Usd.Stage.CreateNew(scenePath)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
stage.SetTimeCodesPerSecond(24)
volume = UsdVol.Volume.Define(stage, "/smoke")


def bind(binding, grid):
    field = UsdVol.OpenVDBAsset.Define(stage, "/smoke/" + binding)
    field.CreateFilePathAttr(Sdf.AssetPath(vdb))
    field.CreateFieldNameAttr(grid)
    volume.CreateFieldRelationship(binding, field.GetPath())


def unbind(binding):
    volume.BlockFieldRelationship(binding)


bind("density", "density")
bind("vel", "vel")
sun = UsdLux.DistantLight.Define(stage, "/sun")
sun.CreateIntensityAttr(3.0)
sun.CreateNormalizeAttr(True)
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 8))
camera.CreateFocalLengthAttr(50.0)
camera.CreateHorizontalApertureAttr(20.0)
camera.CreateVerticalApertureAttr(20.0)
camera.CreateShutterOpenAttr(-0.25)
camera.CreateShutterCloseAttr(0.25)
settings = UsdRender.Settings.Define(stage, "/Render/settings")
product = UsdRender.Product.Define(stage, "/Render/product")
product.CreateProductNameAttr(exrPath)
product.CreateResolutionAttr(Gf.Vec2i(SIZE, SIZE))
product.CreateCameraRel().SetTargets(["/cam"])
settings.CreateProductsRel().SetTargets([product.GetPath()])
settings.CreateCameraRel().SetTargets(["/cam"])
var = UsdRender.Var.Define(stage, "/Render/Vars/C")
var.CreateSourceTypeAttr("lpe")
var.CreateSourceNameAttr("C.*[LO]")
var.CreateDataTypeAttr("color4f")
product.CreateOrderedVarsRel().AddTarget(var.GetPath())
stage.GetRootLayer().Save()
primvars = UsdGeom.PrimvarsAPI(volume)


def red(x, y):
    lines = exr_info.lines(exrinfo, exrPath, str(x), str(y))
    return {l.split()[1]: float(l.split()[2]) for l in lines if l.startswith("pixel ")}.get("R", -1.0)


def width(primvarValues={}, settings={}):
    """Pixels of the middle row the ball covers in one final frame, after
    setting these primvars (None removes one) and with these settings."""
    for name, value in primvarValues.items():
        if value is None:
            primvars.RemovePrimvar(name)
        else:
            kind = Sdf.ValueTypeNames.Int if name.endswith("vblur") else Sdf.ValueTypeNames.Float
            primvars.CreatePrimvar(name, kind).Set(value)
    if os.path.exists(exrPath):
        os.remove(exrPath)
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    engine.SetActiveRenderSettingsPrimPath(Sdf.Path("/Render/settings"))
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraPath(Sdf.Path("/cam"))
    for key, value in {"eldr:autoExposure": False, "eldr:finalQuality": True, **settings}.items():
        engine.SetRendererSetting(key, value)
    params = UsdImagingGL.RenderParams()
    params.frame = Usd.TimeCode(1)
    for _ in range(300):
        engine.Render(stage.GetPseudoRoot(), params)
        if engine.IsConverged():
            break
    del engine
    return sum(red(x, SIZE // 2) > 0.01 for x in range(SIZE))


# A unit at the ball's distance: SIZE / (2 * 8 * 0.2) = 20 pixels; the blur
# adds half a unit (10 pixels).
sharp = width()
expect(15 <= sharp <= 26, "without Velocity Blur the ball is sharp (%d pixels wide)" % sharp)
blurred = width({"karma:object:vblur": 1})
expect(blurred >= sharp + 7, "karma:object:vblur 1: the ball blurs along vel (%d pixels, %d sharp)" % (blurred, sharp))
off = width({"eldr:object:vblur": 0})
expect(off <= sharp + 2, "eldr:object:vblur 0 wins over Karma's 1: sharp (%d pixels)" % off)
scaled = width({"eldr:object:vblur": None, "karma:object:volumevblurscale": 0.0})
expect(scaled <= sharp + 2, "karma:object:volumevblurscale 0: sharp (%d pixels)" % scaled)
doubled = width({"karma:object:volumevblurscale": None, "eldr:object:volumevblurscale": 2.0})
expect(doubled >= blurred + 6, "eldr:object:volumevblurscale 2: twice the blur (%d pixels)" % doubled)
setting = width({"eldr:object:volumevblurscale": None}, {"eldr:volumeVelocityBlur": False})
expect(setting <= sharp + 2, "eldr:volumeVelocityBlur off: sharp (%d pixels)" % setting)
# Houdini's vector volumes: three float grids.
unbind("vel")
for axis in "xyz":
    bind("vel_" + axis, "vel." + axis)
components = width()
expect(abs(components - blurred) <= 2, "vel.x/.y/.z blur as the Vec3f grid (%d pixels, %d)" % (components, blurred))
# A nameless field is the density, also beside a colour field sorting first.
primvars.RemovePrimvar("karma:object:vblur")
unbind("density")
bind("field_0", "density")
bind("Cd", "vel")
nameless = width()
expect(abs(nameless - sharp) <= 2, "field_0 is the density beside Cd (%d pixels, %d)" % (nameless, sharp))

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
