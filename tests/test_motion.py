"""Motion blur through Hydra, read from render products (EXR): a cube moving
1 unit a frame along x past a camera whose shutter is open half a frame.
- A render product is written again for the next time code when that frame
  converges at once (the post motion blur's), not left at the last one's.
- Sampled motion: a single frame (usdrecord) blurs by the motion through
  the shutter that Hydra's transform samples give, with no frame before it.
- Deformation blur: the same for a mesh whose points move (a point cache),
  through Hydra's primvar samples.
- Karma's per-object Enable Motion Blur (primvars:karma:object:mblur) off
  keeps either sharp.
- A motion blur setting off and on again without a time change blurs again.
- Velocity blur: velocities blur meshes, points and instances only with
  Velocity Blur (karma:object:vblur, instance_vblur; eldr:object:* win).
Run: usdenv.cmd python tests\\test_motion.py <exrinfo>"""
import os
import subprocess
import sys
import tempfile

import exr_info

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux, UsdRender


def run(*args, **kwargs):
    """subprocess.run that fails the test after 10 minutes instead of hanging."""
    kwargs.setdefault("timeout", 600)
    return subprocess.run(*args, **kwargs)


SIZE = 64
exrinfo = sys.argv[1]
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


folder = tempfile.mkdtemp(prefix="eldr_motion_")
scenePath = os.path.join(folder, "scene.usda")
exrPath = os.path.join(folder, "beauty.exr").replace("\\", "/")
stage = Usd.Stage.CreateNew(scenePath)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
stage.SetStartTimeCode(1)
stage.SetEndTimeCode(3)
cube = UsdGeom.Cube.Define(stage, "/cube")
cube.CreateSizeAttr(1.0)
cube.CreateDisplayColorAttr([Gf.Vec3f(1, 1, 1)])
move = cube.AddTranslateOp()
move.Set(Gf.Vec3d(-1, 0, 0), 1)
move.Set(Gf.Vec3d(1, 0, 0), 3)
sun = UsdLux.DistantLight.Define(stage, "/sun")
sun.CreateIntensityAttr(3.0)
sun.CreateNormalizeAttr(True)  # else the intensity is the 0.53 degree disc's radiance
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 8))
# Square, as the image.
camera.CreateFocalLengthAttr(50.0)
camera.CreateHorizontalApertureAttr(20.0)
camera.CreateVerticalApertureAttr(20.0)
camera.CreateShutterOpenAttr(-0.25)
camera.CreateShutterCloseAttr(0.25)
settings = UsdRender.Settings.Define(stage, "/Render/settings")
product = UsdRender.Product.Define(stage, "/Render/product")
product.CreateProductNameAttr(exrPath)
product.CreateResolutionAttr(Gf.Vec2i(SIZE, SIZE))  # USD's fallback is 2048 x 1080
product.CreateCameraRel().SetTargets(["/cam"])
settings.CreateProductsRel().SetTargets([product.GetPath()])
settings.CreateCameraRel().SetTargets(["/cam"])
var = UsdRender.Var.Define(stage, "/Render/Vars/C")
var.CreateSourceTypeAttr("lpe")
var.CreateSourceNameAttr("C.*[LO]")
var.CreateDataTypeAttr("color4f")
product.CreateOrderedVarsRel().AddTarget(var.GetPath())
stage.GetRootLayer().Save()
# Pixels a unit of x spans at the cube's distance (the camera's 8 units).
frustum = camera.GetCamera(Usd.TimeCode.Default()).frustum
PIXELS_PER_UNIT = SIZE / (2 * 8 * frustum.GetWindow().GetMax()[0])


def red(x, y):
    """The product's red at pixel (x, y), y down."""
    lines = exr_info.lines(exrinfo, exrPath, str(x), str(y))
    return {l.split()[1]: float(l.split()[2]) for l in lines if l.startswith("pixel ")}.get("R", -1.0)


def render(times, settings, toggles=()):
    """Renders the stage through /cam at each time in turn, until converged;
    the product is written whenever a frame converged. Then, at the last
    time, each of `toggles` (settings) in turn, again until converged."""
    if os.path.exists(exrPath):
        os.remove(exrPath)
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    engine.SetActiveRenderSettingsPrimPath(Sdf.Path("/Render/settings"))
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraPath(Sdf.Path("/cam"))
    for key, value in {"eldr:autoExposure": False, **settings}.items():
        engine.SetRendererSetting(key, value)
    params = UsdImagingGL.RenderParams()
    for t in times:
        params.frame = Usd.TimeCode(t)
        for _ in range(300):
            engine.Render(stage.GetPseudoRoot(), params)
            if engine.IsConverged():
                break
    for toggle in toggles:
        for key, value in toggle.items():
            engine.SetRendererSetting(key, value)
        for _ in range(300):
            engine.Render(stage.GetPseudoRoot(), params)
            if engine.IsConverged():
                break
    del engine


# Time 1 converges over the film's samples; time 2 (the cube moved: the post
# motion blur, final at once) must be in the product, not time 1's image.
render([1, 2], {"eldr:finalQuality": True, "eldr:motionBlurTimeSamples": False})
then = int(SIZE / 2 - PIXELS_PER_UNIT)  # the cube's centre at time 1
expect(red(32, 32) > 0.1 and red(then, 32) < 0.02,
       "the product shows time 2 (cube at the centre: %.3f, where it was at 1: %.3f)" % (red(32, 32), red(then, 32)))



def ramp(row):
    """Pixels of row `row` between 10% and 90% of the cube's brightest: its
    blurred edges (a sharp, antialiased cube has a pixel or two per edge)."""
    values = [red(x, row) for x in range(SIZE)]
    top = max(values)
    return sum(0.1 * top < v < 0.9 * top for v in values)


# Sampled motion: one usdrecord frame, no frame before it, is blurred by the
# cube's motion through the shutter (time samples: 0.5 units, about 11
# pixels per edge here).
if os.path.exists(exrPath):
    os.remove(exrPath)
run(["usdrecord", "--renderer", "Eldr", "--camera", "/cam", "--renderSettingsPrimPath", "/Render/settings",
                "--frames", "2", "-w", str(SIZE), scenePath, os.path.join(folder, "unused.#.png")], check=True,
               shell=True)
blurred = ramp(32)
expect(blurred >= 12, "a single usdrecord frame is motion blurred (%d edge pixels)" % blurred)
# eldr:sampledMotion off: the same first frame stays sharp.
render([2], {"eldr:sampledMotion": False})
sharp = ramp(32)
expect(sharp <= 4, "eldr:sampledMotion off: the first frame is sharp (%d edge pixels)" % sharp)
render([2], {"eldr:finalQuality": True})
blurred = ramp(32)
expect(blurred >= 12, "a final frame through the engine is motion blurred (%d edge pixels)" % blurred)
# A blur setting off and on again, the time unchanged: it blurs again at once.
for key in ("eldr:motionBlur", "eldr:motionBlurTimeSamples", "eldr:sampledMotion"):
    render([2], {"eldr:finalQuality": True}, [{key: False}, {key: True}])
    blurred = ramp(32)
    expect(blurred >= 12, "%s off and on again: the cube blurs again (%d edge pixels)" % (key, blurred))
UsdGeom.PrimvarsAPI(cube).CreatePrimvar("karma:object:mblur", Sdf.ValueTypeNames.Bool).Set(False)
render([2], {"eldr:finalQuality": True})
sharp = ramp(32)
expect(sharp <= 4, "primvars:karma:object:mblur off: the moving cube is sharp (%d edge pixels)" % sharp)

# Deformation blur: the same cube as a mesh whose points move (no transform
# animation), one usdrecord frame.
cube.GetPrim().SetActive(False)
mesh = UsdGeom.Mesh.Define(stage, "/deforming")
corners = [Gf.Vec3f(x, y, z) for z in (-0.5, 0.5) for y in (-0.5, 0.5) for x in (-0.5, 0.5)]
mesh.CreateFaceVertexCountsAttr([4] * 6)
mesh.CreateFaceVertexIndicesAttr([0, 2, 3, 1, 4, 5, 7, 6, 0, 1, 5, 4, 2, 6, 7, 3, 0, 4, 6, 2, 1, 3, 7, 5])
mesh.CreateDisplayColorAttr([Gf.Vec3f(1, 1, 1)])
mesh.CreateSubdivisionSchemeAttr("none")
points = mesh.CreatePointsAttr()
for t, dx in ((1, -1.0), (3, 1.0)):
    points.Set([p + Gf.Vec3f(dx, 0, 0) for p in corners], t)
stage.GetRootLayer().Save()
if os.path.exists(exrPath):
    os.remove(exrPath)
run(["usdrecord", "--renderer", "Eldr", "--camera", "/cam", "--renderSettingsPrimPath", "/Render/settings",
                "--frames", "2", "-w", str(SIZE), scenePath, os.path.join(folder, "unused.#.png")], check=True,
               shell=True)
blurred = ramp(32)
expect(blurred >= 12, "a single usdrecord frame of moving points is blurred (%d edge pixels)" % blurred)
render([2], {"eldr:deformationBlur": False, "eldr:finalQuality": True})
sharp = ramp(32)
expect(sharp <= 4, "eldr:deformationBlur off: moving points stay sharp (%d edge pixels)" % sharp)
print("deformation blur: %d edge pixels on, %d off" % (blurred, sharp))
render([2], {"eldr:finalQuality": True})
blurred = ramp(32)
expect(blurred >= 12, "a final frame of moving points through the engine is blurred (%d edge pixels)" % blurred)
for key in ("eldr:deformationBlur", "eldr:motionBlur"):
    render([2], {"eldr:finalQuality": True}, [{key: False}, {key: True}])
    blurred = ramp(32)
    expect(blurred >= 12, "%s off and on again: moving points blur again (%d edge pixels)" % (key, blurred))
UsdGeom.PrimvarsAPI(mesh).CreatePrimvar("karma:object:mblur", Sdf.ValueTypeNames.Bool).Set(False)
render([2], {"eldr:finalQuality": True})
sharp = ramp(32)
expect(sharp <= 4, "primvars:karma:object:mblur off: moving points stay sharp (%d edge pixels)" % sharp)

# Velocity blur: still points with velocities (24 units a second, a unit a
# frame as above) blur only where Velocity Blur asks (Karma's default off);
# Eldr's primvar wins over Karma's.
mesh.GetPrim().SetActive(False)
still = UsdGeom.Mesh.Define(stage, "/velocities")
for attr, value in ((still.CreateFaceVertexCountsAttr(), [4] * 6),
                    (still.CreateFaceVertexIndicesAttr(), mesh.GetFaceVertexIndicesAttr().Get()),
                    (still.CreateDisplayColorAttr(), [Gf.Vec3f(1, 1, 1)]),
                    (still.CreateSubdivisionSchemeAttr(), "none"),
                    (still.CreatePointsAttr(), corners),
                    (still.CreateVelocitiesAttr(), [Gf.Vec3f(24, 0, 0)] * 8)):
    attr.Set(value)
stillPrimvars = UsdGeom.PrimvarsAPI(still)


def velocityBlur(karma=None, eldr=None, instance=False):
    """Edge pixels of one final frame at time 2 with these vblur primvars on
    stillPrimvars' prim."""
    prefix = "instance_vblur" if instance else "vblur"
    for ns, value in (("karma", karma), ("eldr", eldr)):
        name = "%s:object:%s" % (ns, prefix)
        if value is None:
            stillPrimvars.RemovePrimvar(name)
        else:
            stillPrimvars.CreatePrimvar(name, Sdf.ValueTypeNames.Int).Set(value)
    render([2], {"eldr:finalQuality": True})
    return ramp(32)


edges = velocityBlur()
expect(edges <= 4, "velocities without Velocity Blur: the mesh is sharp (%d edge pixels)" % edges)
edges = velocityBlur(karma=1)
expect(edges >= 12, "karma:object:vblur 1: the mesh blurs by its velocities (%d edge pixels)" % edges)
edges = velocityBlur(karma=1, eldr=0)
expect(edges <= 4, "eldr:object:vblur 0 wins over Karma's 1: sharp (%d edge pixels)" % edges)
# A subdivision surface re-dicing (a subdivision setting) bumps the scene's
# version without uploading the velocity mesh: it must blur again.
ball = UsdGeom.Mesh.Define(stage, "/ball")  # catmullClark, USD's default
ball.CreateFaceVertexCountsAttr([4] * 6)
ball.CreateFaceVertexIndicesAttr(mesh.GetFaceVertexIndicesAttr().Get())
ball.CreatePointsAttr([p * 0.6 + Gf.Vec3f(0, 1.2, 0) for p in corners])  # above the rows measured
ball.CreateDisplayColorAttr([Gf.Vec3f(1, 1, 1)])
velocityBlur(karma=1)
render([2], {"eldr:finalQuality": True}, [{"eldr:subdivision": False}])
edges = ramp(32)
expect(edges >= 12, "velocities blur after a subdivision setting re-dices another mesh (%d edge pixels)" % edges)
ball.GetPrim().SetActive(False)
# Points (curves share their code): the same velocities.
still.GetPrim().SetActive(False)
points = UsdGeom.Points.Define(stage, "/pointsVelocities")
points.CreatePointsAttr([Gf.Vec3f(0, 0, 0)])
points.CreateWidthsAttr([1.0])
points.CreateDisplayColorAttr([Gf.Vec3f(1, 1, 1)])
points.CreateVelocitiesAttr([Gf.Vec3f(24, 0, 0)])
stillPrimvars = UsdGeom.PrimvarsAPI(points)
# A sphere's shading ramps too: measure how wide it shows instead.
def covered(row):
    return sum(red(x, row) > 0.01 for x in range(SIZE))


velocityBlur()
sharp = covered(32)
velocityBlur(eldr=2)
blurred = covered(32)
expect(blurred >= sharp + 6, "eldr:object:vblur 2: points blur by their velocities (%d pixels wide, %d without)"
       % (blurred, sharp))
# A PointInstancer's instances: instance_vblur.
points.GetPrim().SetActive(False)
instancer = UsdGeom.PointInstancer.Define(stage, "/instancer")
proto = UsdGeom.Cube.Define(stage, "/instancer/proto")
proto.CreateSizeAttr(1.0)
proto.CreateDisplayColorAttr([Gf.Vec3f(1, 1, 1)])
instancer.CreatePrototypesRel().SetTargets([proto.GetPath()])
instancer.CreateProtoIndicesAttr([0])
instancer.CreatePositionsAttr([Gf.Vec3f(0, 0, 0)])
instancer.CreateVelocitiesAttr([Gf.Vec3f(24, 0, 0)])
stillPrimvars = UsdGeom.PrimvarsAPI(instancer)
edges = velocityBlur(instance=True)
expect(edges <= 4, "instance velocities without Instance Velocity Blur: sharp (%d edge pixels)" % edges)
edges = velocityBlur(karma=1, instance=True)
expect(edges >= 12, "karma:object:instance_vblur 1: instances blur by velocities (%d edge pixels)" % edges)

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
