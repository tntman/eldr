"""Plays a VDB sequence as usdview's and Houdini's Play do: a volume whose
OpenVDBAsset filePath is time-sampled, one file per frame (make_volume
--sequence writes them: a ball growing each frame, so each grid has a new
bounding box and size), one render per time code, forward twice. Every
frame must load its own grid, a missing and a corrupt frame only drop the
volume, and memory must not grow from one pass to the next. Run under
validation (ctest fails on "Validation Error").
Run: usdenv.cmd python tests\\test_volume_sequence.py <make_volume.exe>"""
import os
import shutil
import subprocess
import sys
import tempfile

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdVol

FRAMES = 12
SIZE = 64
tmp = tempfile.mkdtemp(prefix="hdvk_vdbseq_")
subprocess.check_call([sys.argv[1], "--sequence", tmp, str(FRAMES)])
paths = [os.path.join(tmp, "seq_%04d.vdb" % f) for f in range(FRAMES)]
missing = os.path.join(tmp, "seq_missing.vdb")
corrupt = os.path.join(tmp, "seq_corrupt.vdb")
with open(paths[3], "rb") as src, open(corrupt, "wb") as dst:
    dst.write(src.read()[:200])  # a header and nothing after it

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.CreateInMemory()
volume = UsdVol.Volume.Define(stage, "/smoke")
field = UsdVol.OpenVDBAsset.Define(stage, "/smoke/density")
field.CreateFieldNameAttr("density")
filePath = field.CreateFilePathAttr()
# Frames 0..FRAMES-1, then a missing file and a corrupt one.
timeline = paths + [missing, corrupt]
for t, path in enumerate(timeline):
    filePath.Set(Sdf.AssetPath(path), t)
volume.CreateFieldRelationship("density", field.GetPath())

camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 10))
gfCamera = camera.GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(gfCamera, CameraUtil.Fit, 1.0)
frustum = gfCamera.frustum

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


def play():
    """One render per time code (Play), the stats after each."""
    stats = []
    for t in range(len(timeline)):
        params.frame = Usd.TimeCode(t)
        engine.Render(stage.GetPseudoRoot(), params)
        stats.append(engine.GetRenderStats())
    return stats


first, second = play(), play()
for name, stats in (("first", first), ("second", second)):
    grids = [s.get("volumeMemory", 0) for s in stats[:FRAMES]]
    expect(all(s.get("volumes") == 1 for s in stats[:FRAMES]), "%s pass: a volume every frame" % name)
    expect(all(a < b for a, b in zip(grids, grids[1:])),
           "%s pass: each frame loads its own (growing) grid: %s" % (name, grids))
    expect(stats[FRAMES].get("volumes") == 0, "%s pass: a missing file drops the volume" % name)
    expect(stats[FRAMES + 1].get("volumes") == 0, "%s pass: a corrupt file drops the volume" % name)
gpu = [s.get("gpuMemoryUsed", 0) for s in (first[FRAMES - 1], second[FRAMES - 1])]
host = [s.get("hostMemoryUsed", 0) for s in (first[FRAMES - 1], second[FRAMES - 1])]
print("GPU memory after each pass: %s, host memory: %s" % (gpu, host))
expect(gpu[1] <= gpu[0] and host[1] <= host[0] * 1.05 + (1 << 20),
       "memory doesn't grow from one pass to the next: GPU %s, host %s" % (gpu, host))

# Houdini's Play: each frame the volume LOP cooks again, its layer holding
# filePath sampled at that frame only. UsdImaging's legacy delegate (run
# with USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX=0) makes the field anew on
# that change; the volume must still see the new grid.
filePath.Clear()
cook = Sdf.Layer.CreateAnonymous()
stage.GetRootLayer().subLayerPaths.append(cook.identifier)
grids = []
for t in range(FRAMES):
    cook.Clear()
    spec = Sdf.CreatePrimInLayer(cook, "/smoke/density")
    spec.specifier = Sdf.SpecifierDef
    spec.typeName = "OpenVDBAsset"
    attr = Sdf.AttributeSpec(spec, "filePath", Sdf.ValueTypeNames.Asset)
    cook.SetTimeSample(attr.path, t, Sdf.AssetPath(paths[t]))
    params.frame = Usd.TimeCode(t)
    engine.Render(stage.GetPseudoRoot(), params)
    grids.append(engine.GetRenderStats().get("volumeMemory", 0))
expect(all(a < b for a, b in zip(grids, grids[1:])), "recooked each frame: each frame's own grid: %s" % grids)

del engine  # before the GL context goes
shutil.rmtree(tmp, ignore_errors=True)
if failures:
    sys.exit(1)
print("ok")
