"""Edits while working in a viewport, timed: each edit is authored on the
live stage's session layer (as Houdini's LOPs edit the stage the viewport
draws) or sent as a render setting, then frames render until converged.
Per edit: the first frame after it (sync, commit, render), the frames until
converged (count, wall time, slowest frame) and, with HDVK_TIMING=2, the
frame timing CSV's sync and commit times of that first frame.
Run: usdenv.cmd python tests\\edit_bench.py --scene file [--camera /path]
     [--size 1280x720] [--out folder] [--ref folder] [--only a,b]
     (crowds: HD_ENABLE_DEFERRED_SKINNING=1; HDVK_TIMING=3 also logs the
     slow parts of every frame and commit to stderr)
--out saves each edit's converged image; --ref compares them to an earlier
run's (mean difference of 255)."""
import argparse
import csv
import os
import sys
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument("--scene", required=True)
parser.add_argument("--camera")
parser.add_argument("--size", default="1280x720")
parser.add_argument("--out")
parser.add_argument("--ref")
parser.add_argument("--only")
parser.add_argument("--max-frames", type=int, default=400)
parser.add_argument("--set", action="append", default=[], help="name=value render setting")
args = parser.parse_args()
W, H = (int(v) for v in args.size.split("x"))
if os.environ.get("HDVK_TIMING", "0") >= "2" and "HDVK_TIMING_CSV" not in os.environ:
    os.environ["HDVK_TIMING_CSV"] = os.path.join(tempfile.gettempdir(), "edit_bench_%d.csv" % os.getpid())
CSV = os.environ.get("HDVK_TIMING_CSV") if os.environ.get("HDVK_TIMING", "0") >= "2" else None

import numpy as np  # noqa: E402
from OpenGL import GL  # noqa: E402
from PySide6.QtCore import QSize  # noqa: E402
from PySide6.QtGui import QImage, QOffscreenSurface, QOpenGLContext  # noqa: E402
from PySide6.QtOpenGL import QOpenGLFramebufferObject  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade  # noqa: E402

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(W, H), QOpenGLFramebufferObject.CombinedDepthStencil)
fbo.bind()
from pxr import UsdImagingGL  # noqa: E402  (after the GL context)

t0 = time.perf_counter()
stage = Usd.Stage.Open(args.scene)
stage.SetEditTarget(stage.GetSessionLayer())
print("opened in %.1f s" % (time.perf_counter() - t0))

engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "Eldr not available"
engine.SetRendererSetting("houdini:viewport", True)
for s in args.set:
    k, v = s.split("=", 1)
    engine.SetRendererSetting(k, True if v == "true" else False if v == "false" else float(v) if "." in v else int(v))
engine.SetRenderViewport(Gf.Vec4d(0, 0, W, H))
params = UsdImagingGL.RenderParams()
params.frame = Usd.TimeCode.EarliestTime()
params.gammaCorrectColors = False
params.showRender = True  # crowds exported with purpose render
root = stage.GetPseudoRoot()

cache = UsdGeom.BBoxCache(Usd.TimeCode.EarliestTime(), [UsdGeom.Tokens.default_, UsdGeom.Tokens.render])
box = cache.ComputeWorldBound(root).ComputeAlignedRange()
centre, size = box.GetMidpoint(), box.GetSize()
radius = max(size[0], size[1], size[2]) * 0.5 or 1.0
if args.camera:
    engine.SetCameraPath(Sdf.Path(args.camera))
else:  # usdview's frame all
    cam = Gf.Camera()
    cam.SetPerspectiveFromAspectRatioAndFieldOfView(W / H, 50.0, Gf.Camera.FOVHorizontal)
    cam.clippingRange = Gf.Range1f(radius * 0.01, radius * 10)
    cam.transform = Gf.Matrix4d().SetTranslate(Gf.Vec3d(0, 0, radius * 2.2)) * Gf.Matrix4d().SetRotate(
        Gf.Rotation(Gf.Vec3d(1, 0, 0), -20)) * Gf.Matrix4d().SetTranslate(centre)
    f = cam.frustum
    engine.SetCameraState(f.ComputeViewMatrix(), f.ComputeProjectionMatrix())


def render():
    a = time.perf_counter()
    GL.glClearColor(0, 0, 0, 0)
    GL.glClear(GL.GL_COLOR_BUFFER_BIT | GL.GL_DEPTH_BUFFER_BIT)
    engine.Render(root, params)
    GL.glFinish()
    return (time.perf_counter() - a) * 1000.0


def colour():
    data = GL.glReadPixels(0, 0, W, H, GL.GL_RGBA, GL.GL_UNSIGNED_BYTE)
    return np.frombuffer(data, np.uint8).reshape(H, W, 4)[::-1, :, :3].astype(np.float32)


def csv_rows():
    if not CSV or not os.path.exists(CSV):
        return []
    with open(CSV, newline="") as f:
        return list(csv.DictReader(f))


def converge():
    """Frames until converged: their times."""
    times = [render()]  # IsConverged answers for the frame before
    while len(times) < args.max_frames:
        times.append(render())
        if engine.IsConverged():
            break
    return times


def note(text):
    sys.stderr.write("=== %s\n" % text)
    sys.stderr.flush()


# The scene's parts the edits use.
meshes, materials, shaders, lights = [], [], [], []
for prim in stage.Traverse():
    if prim.IsA(UsdGeom.Mesh) and len(meshes) < 50:
        meshes.append(prim)
    elif prim.IsA(UsdShade.Material):
        materials.append(prim)
    elif prim.IsA(UsdShade.Shader):
        sid = UsdShade.Shader(prim).GetIdAttr().Get()
        if sid in ("UsdPreviewSurface", "ND_standard_surface_surfaceshader", "ND_open_pbr_surface_surfaceshader"):
            shaders.append(prim)
    elif prim.HasAPI(UsdLux.LightAPI) or prim.IsA(UsdLux.BoundableLightBase) or prim.IsA(UsdLux.NonboundableLightBase):
        lights.append(prim)
# A mesh to move / hide: the biggest top-level xformable under the root.
movable = None
for prim in root.GetChildren():
    if prim.IsA(UsdGeom.Xformable) and not prim.IsA(UsdGeom.Camera) and not prim.HasAPI(UsdLux.LightAPI):
        movable = movable or prim
deep = meshes[len(meshes) // 2] if meshes else None
print("scene: %d meshes (first 50), %d materials, %d surface shaders, %d lights; move %s, hide %s" % (
    len(meshes), len(materials), len(shaders), len(lights), movable.GetPath() if movable else None,
    deep.GetPath() if deep else None))

EDITS = []


def edit(name):
    def add(fn):
        EDITS.append((name, fn))
        return fn
    return add


@edit("exposure")  # a post setting
def _():
    engine.SetRendererSetting("eldr:exposureCompensation", 0.5)


@edit("toggle_ao")  # a feature toggle
def _():
    engine.SetRendererSetting("eldr:ambientOcclusion", False)


@edit("toggle_clearcoat")  # a shading feature (another shading pipeline set)
def _():
    engine.SetRendererSetting("eldr:clearcoat", False)


@edit("shadow_res")
def _():
    engine.SetRendererSetting("eldr:shadowResolution", 4096)


@edit("light_add")
def _():
    light = UsdLux.SphereLight.Define(stage, "/editBench_light")
    light.CreateIntensityAttr(50.0)
    light.CreateRadiusAttr(radius * 0.02)
    light.AddTranslateOp().Set(centre + Gf.Vec3d(0, size[1] * 0.6 + radius * 0.1, 0))


@edit("light_move")
def _():
    UsdGeom.Xformable(stage.GetPrimAtPath("/editBench_light")).GetOrderedXformOps()[0].Set(
        centre + Gf.Vec3d(radius * 0.3, size[1] * 0.6 + radius * 0.1, 0))


@edit("light_intensity")
def _():
    UsdLux.SphereLight(stage.GetPrimAtPath("/editBench_light")).GetIntensityAttr().Set(80.0)


@edit("light_hide")
def _():
    UsdGeom.Imageable(stage.GetPrimAtPath("/editBench_light")).MakeInvisible()


@edit("prim_add")
def _():
    ball = UsdGeom.Sphere.Define(stage, "/editBench_ball")
    ball.CreateRadiusAttr(radius * 0.1)
    ball.CreateDisplayColorAttr([Gf.Vec3f(0.8, 0.2, 0.1)])
    ball.AddTranslateOp().Set(centre)


@edit("prim_move")
def _():
    UsdGeom.Xformable(stage.GetPrimAtPath("/editBench_ball")).GetOrderedXformOps()[0].Set(
        centre + Gf.Vec3d(radius * 0.2, 0, 0))


@edit("material_new")  # a new material bound to the new prim
def _():
    mat = UsdShade.Material.Define(stage, "/editBench_mat")
    sh = UsdShade.Shader.Define(stage, "/editBench_mat/surface")
    sh.CreateIdAttr("UsdPreviewSurface")
    sh.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.1, 0.4, 0.9))
    sh.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.3)
    mat.CreateSurfaceOutput().ConnectToSource(sh.ConnectableAPI(), "surface")
    UsdShade.MaterialBindingAPI.Apply(stage.GetPrimAtPath("/editBench_ball")).Bind(mat)


@edit("material_input")  # a shader input on a material the scene's meshes use
def _():
    sh = UsdShade.Shader(shaders[0]) if shaders else UsdShade.Shader(stage.GetPrimAtPath("/editBench_mat/surface"))
    for name in ("diffuseColor", "base_color"):
        if sh.GetInput(name) or sh.GetIdAttr().Get() == "UsdPreviewSurface" and name == "diffuseColor":
            sh.CreateInput(name, Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.9, 0.5, 0.1))
            break
    else:
        sh.CreateInput("base_color", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.9, 0.5, 0.1))


@edit("material_bind")  # an existing mesh to the new material
def _():
    target = deep or stage.GetPrimAtPath("/editBench_ball")
    UsdShade.MaterialBindingAPI.Apply(target).Bind(UsdShade.Material(stage.GetPrimAtPath("/editBench_mat")))


@edit("scene_move")  # a big part of the scene moved (a transform LOP)
def _():
    if movable:
        UsdGeom.Xformable(movable).AddTranslateOp(opSuffix="editBench").Set(Gf.Vec3d(0, radius * 0.02, 0))


@edit("mesh_hide")
def _():
    if deep:
        UsdGeom.Imageable(deep).MakeInvisible()


@edit("mesh_show")
def _():
    if deep:
        UsdGeom.Imageable(deep).MakeVisible()


t0 = time.perf_counter()
first = converge()
print("first converged frame after %.1f s (%d frames)" % (time.perf_counter() - t0, len(first)))
idle = [render() for _ in range(10)]
print("converged redraw: %.1f ms median" % float(np.median(idle)))
only = set(args.only.split(",")) if args.only else None
results, failures = [], []
for name, fn in EDITS:
    if only and name not in only:
        continue
    note("edit %s" % name)
    rowsBefore = len(csv_rows())
    a = time.perf_counter()
    fn()
    applied = (time.perf_counter() - a) * 1000.0
    times = converge()
    total = applied + sum(times)
    rows = csv_rows()[rowsBefore:]
    r = rows[0] if rows else {}
    sync = float(r.get("sync_wall_ms", 0) or 0)
    commit = float(r.get("commit_ms", 0) or 0)
    image = colour()
    diff = None
    if args.out:
        os.makedirs(args.out, exist_ok=True)
        np.save(os.path.join(args.out, name + ".npy"), image.astype(np.uint8))
        QImage(np.ascontiguousarray(image.astype(np.uint8)).data, W, H, 3 * W,
               QImage.Format_RGB888).copy().save(os.path.join(args.out, name + ".png"))
    if args.ref and os.path.exists(os.path.join(args.ref, name + ".npy")):
        diff = float(np.abs(np.load(os.path.join(args.ref, name + ".npy")).astype(np.float32) - image).mean())
    results.append((name, applied, times[0], sync, commit, len(times), total, max(times[1:] or [0]),
                    float(np.median(times[1:] or [0])), diff))
    note("edit %s done" % name)

print("\n%-17s %8s %9s %8s %8s %7s %10s %9s %9s %6s" % (
    "edit", "author", "1st frame", "sync", "commit", "frames", "converged", "max frame", "med frame", "diff"))
for n, ap, f1, sy, cm, nf, tot, mx, md, d in results:
    print("%-17s %8.1f %9.1f %8.1f %8.1f %7d %10.0f %9.1f %9.1f %6s" % (
        n, ap, f1, sy, cm, nf, tot, mx, md, "-" if d is None else "%.2f" % d))
del engine
