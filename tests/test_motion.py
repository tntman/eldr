"""Motion blur through Hydra, read from render products (EXR): a cube moving
1 unit a frame along x past a camera whose shutter is open half a frame.
- A render product is written again for the next time code when that frame
  converges at once (the post motion blur's), not left at the last one's.
- Sampled motion: a single frame (usdrecord) blurs by the motion through
  the shutter that Hydra's transform samples give, with no frame before it.
- Deformation blur: the same for a mesh whose points move (a point cache),
  through Hydra's primvar samples.
Run: usdenv.cmd python tests\\test_motion.py <exrinfo>"""
import os
import subprocess
import sys
import tempfile

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux, UsdRender

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
    lines = subprocess.run([exrinfo, exrPath, str(x), str(y)], capture_output=True, text=True).stdout.splitlines()
    return {l.split()[1]: float(l.split()[2]) for l in lines if l.startswith("pixel ")}.get("R", -1.0)


def render(times, settings):
    """Renders the stage through /cam at each time in turn, until converged;
    the product is written whenever a frame converged."""
    if os.path.exists(exrPath):
        os.remove(exrPath)
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    engine.SetActiveRenderSettingsPrimPath(Sdf.Path("/Render/settings"))
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraPath(Sdf.Path("/cam"))
    for key, value in {"hdvk:autoExposure": False, **settings}.items():
        engine.SetRendererSetting(key, value)
    params = UsdImagingGL.RenderParams()
    for t in times:
        params.frame = Usd.TimeCode(t)
        for _ in range(300):
            engine.Render(stage.GetPseudoRoot(), params)
            if engine.IsConverged():
                break
    del engine


# Time 1 converges over the film's samples; time 2 (the cube moved: the post
# motion blur, final at once) must be in the product, not time 1's image.
render([1, 2], {"hdvk:finalQuality": True, "hdvk:motionBlurTimeSamples": False})
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
subprocess.run(["usdrecord", "--renderer", "Eldr", "--camera", "/cam", "--renderSettingsPrimPath", "/Render/settings",
                "--frames", "2", "-w", str(SIZE), scenePath, os.path.join(folder, "unused.#.png")], check=True,
               shell=True)
blurred = ramp(32)
expect(blurred >= 12, "a single usdrecord frame is motion blurred (%d edge pixels)" % blurred)
# hdvk:sampledMotion off: the same first frame stays sharp.
render([2], {"hdvk:sampledMotion": False})
sharp = ramp(32)
expect(sharp <= 4, "hdvk:sampledMotion off: the first frame is sharp (%d edge pixels)" % sharp)

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
subprocess.run(["usdrecord", "--renderer", "Eldr", "--camera", "/cam", "--renderSettingsPrimPath", "/Render/settings",
                "--frames", "2", "-w", str(SIZE), scenePath, os.path.join(folder, "unused.#.png")], check=True,
               shell=True)
blurred = ramp(32)
expect(blurred >= 12, "a single usdrecord frame of moving points is blurred (%d edge pixels)" % blurred)
render([2], {"hdvk:deformationBlur": False, "hdvk:finalQuality": True})
sharp = ramp(32)
expect(sharp <= 4, "hdvk:deformationBlur off: moving points stay sharp (%d edge pixels)" % sharp)
print("deformation blur: %d edge pixels on, %d off" % (blurred, sharp))

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
