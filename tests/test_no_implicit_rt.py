"""Ray tracing runs only where a ray traced setting is on. A closed room with
a window in its ceiling, two suns (the second, past the cascades' one, gets an
atlas shadow map), a dome with an HDRI (its extracted sun is a third), an
emissive panel (a shadowed rect light), a sphere light, smoke and a glass pane.
With default settings no ray tracing pipeline compiles and no acceleration
structure exists (the render stats' rayTracingPipelineStarts and
accelerationStructureMemory); with eldr:rtShadows both do (on a ray tracing
GPU). Then, without the dome and the smoke: the second sun lights the floor
only through the window, not through the ceiling.
Run: usdenv.cmd python tests\\test_no_implicit_rt.py"""
import os
import sys

import numpy as np
from OpenGL import GL
from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux, UsdShade, UsdVol

SIZE = 96
VDB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "volume.vdb")

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE), QOpenGLFramebufferObject.NoAttachment,
                               GL.GL_TEXTURE_2D, GL.GL_RGBA32F)
fbo.bind()

stage = Usd.Stage.CreateInMemory()
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)


def box(path, lo, hi):
    cube = UsdGeom.Cube.Define(stage, path)
    cube.CreateSizeAttr(1)
    cube.CreateDisplayColorAttr([(0.6, 0.6, 0.6)])
    cube.AddTranslateOp().Set(Gf.Vec3d(*[(a + b) * 0.5 for a, b in zip(lo, hi)]))
    cube.AddScaleOp().Set(Gf.Vec3f(*[b - a for a, b in zip(lo, hi)]))


def material(path, **inputs):
    m = UsdShade.Material.Define(stage, path)
    shader = UsdShade.Shader.Define(stage, path + "/pbr")
    shader.CreateIdAttr("UsdPreviewSurface")
    for name, value in inputs.items():
        kind = Sdf.ValueTypeNames.Float if isinstance(value, float) else Sdf.ValueTypeNames.Color3f
        shader.CreateInput(name, kind).Set(value)
    m.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
    return m


# Room 10 x 6 x 10 inside, walls 0.4 thick, a 2 x 2 window in the ceiling.
box("/room/floor", (-5.4, -0.4, -5.4), (5.4, 0, 5.4))
box("/room/left", (-5.4, 0, -5.4), (-5, 6, 5.4))
box("/room/right", (5, 0, -5.4), (5.4, 6, 5.4))
box("/room/back", (-5, 0, -5.4), (5, 6, -5))
box("/room/front", (-5, 0, 5), (5, 6, 5.4))
box("/room/ceil1", (-5.4, 6, -5.4), (-1, 6.4, 5.4))
box("/room/ceil2", (1, 6, -5.4), (5.4, 6.4, 5.4))
box("/room/ceil3", (-1, 6, -5.4), (1, 6.4, -1))
box("/room/ceil4", (-1, 6, 1), (1, 6.4, 5.4))

# The first sun (the cascades) is dim; the second, 5 degrees off vertical, bright.
first = UsdLux.DistantLight.Define(stage, "/sun1")
first.CreateIntensityAttr(0.5)
first.AddRotateXYZOp().Set(Gf.Vec3f(-90, 0, 0))
second = UsdLux.DistantLight.Define(stage, "/sun2")
second.CreateIntensityAttr(40)
second.CreateNormalizeAttr(True)
second.AddRotateXYZOp().Set(Gf.Vec3f(-85, 0, 0))

dome = UsdLux.DomeLight.Define(stage, "/dome")
dome.CreateTextureFileAttr(Sdf.AssetPath("StinsonBeach.hdr"))
bulb = UsdLux.SphereLight.Define(stage, "/bulb")
bulb.CreateRadiusAttr(0.1)
bulb.CreateIntensityAttr(50)
bulb.AddTranslateOp().Set(Gf.Vec3d(3, 4, -3))

panel = UsdGeom.Mesh.Define(stage, "/panel")
panel.CreatePointsAttr([(-4.9, 3, -1), (-4.9, 3, 1), (-4.9, 4, 1), (-4.9, 4, -1)])
panel.CreateFaceVertexCountsAttr([4])
panel.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
panel.CreateDoubleSidedAttr(True)
UsdShade.MaterialBindingAPI.Apply(panel.GetPrim()).Bind(material("/emitter", emissiveColor=(20.0, 20.0, 20.0)))

pane = UsdGeom.Mesh.Define(stage, "/pane")
pane.CreatePointsAttr([(2, 1, -2), (4, 1, -2), (4, 3, -3), (2, 3, -3)])
pane.CreateFaceVertexCountsAttr([4])
pane.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
pane.CreateDoubleSidedAttr(True)
UsdShade.MaterialBindingAPI.Apply(pane.GetPrim()).Bind(
    material("/glass", diffuseColor=(0.9, 0.9, 0.9), opacity=0.3, ior=1.5))

cloud = UsdVol.Volume.Define(stage, "/cloud")
cloud.AddTranslateOp().Set(Gf.Vec3d(-2, 2, -2))
cloud.AddScaleOp().Set(Gf.Vec3f(1.5, 1.5, 1.5))
field = UsdVol.OpenVDBAsset.Define(stage, "/cloud/density")
field.CreateFilePathAttr(Sdf.AssetPath(VDB))
field.CreateFieldNameAttr("density")
cloud.CreateFieldRelationship("density", field.GetPath())

# Inside, above the front wall's middle, looking down at the floor.
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 5.5, 4.6))
camera.AddRotateXOp().Set(-50)
camera.CreateFocalLengthAttr(12)
frustum = camera.GetCamera(Usd.TimeCode.Default()).frustum
viewProj = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()

params = UsdImagingGL.RenderParams()
failures = []


def expect(ok, what):
    print("ok  " if ok else "FAIL", what, file=sys.stderr if not ok else sys.stdout)
    if not ok:
        failures.append(what)


def render(frames=8, **settings):
    """Float RGB after `frames` frames (top row first), and the stats."""
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    for key, value in {"eldr:autoExposure": False, **settings}.items():
        engine.SetRendererSetting(key, value)
    for _ in range(frames):
        engine.Render(stage.GetPseudoRoot(), params)
    stats = engine.GetRenderStats()
    data = GL.glReadPixels(0, 0, SIZE, SIZE, GL.GL_RGBA, GL.GL_FLOAT)
    del engine
    return np.frombuffer(data, np.float32).reshape(SIZE, SIZE, 4)[::-1, :, :3].copy(), stats


def at(image, x, y, z):
    """Mean of the 3 x 3 pixels showing world (x, y, z)."""
    p = viewProj.Transform(Gf.Vec3d(x, y, z))
    px, py = int((p[0] * 0.5 + 0.5) * SIZE), int((0.5 - p[1] * 0.5) * SIZE)
    return float(image[py - 1:py + 2, px - 1:px + 2].mean())


_, stats = render()
starts, accel = stats.get("rayTracingPipelineStarts"), stats.get("accelerationStructureMemory")
print("defaults: ray tracing pipeline compiles %s, acceleration structures %s bytes" % (starts, accel))
expect(starts == 0, "defaults: no ray tracing pipeline compiles")
expect(accel == 0, "defaults: no acceleration structures")
if stats.get("rayTracingSupported"):
    _, stats = render(frames=2, **{"eldr:rtShadows": True})
    starts, accel = stats.get("rayTracingPipelineStarts"), stats.get("accelerationStructureMemory")
    print("eldr:rtShadows: ray tracing pipeline compiles %s, acceleration structures %s bytes" % (starts, accel))
    expect(starts > 0 and accel > 0, "eldr:rtShadows: the counters see ray tracing")
else:
    print("no ray tracing GPU: the traced check is skipped")

# The second sun on the floor: through the window only (its atlas map).
for prim in ("/dome", "/cloud", "/bulb", "/panel", "/pane"):
    stage.GetPrimAtPath(prim).SetActive(False)
image, _ = render()
off, _ = render(**{"eldr:extraSunShadowMaps": False})
beam, shade, leak = at(image, 0, 0, 0), at(image, -3, 0, -2), at(off, -3, 0, -2)
print("floor: beam %.4f, shade %.4f (without the map %.4f)" % (beam, shade, leak))
expect(beam > 0.05, "the second sun lights the floor through the window")
expect(shade < 0.25 * beam, "the second sun's map keeps it out elsewhere")
expect(leak > 0.5 * beam, "without its map the second sun lights through the ceiling (the A/B switch works)")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
