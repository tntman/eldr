"""Media under a ray-traced sun: a closed box room with a window in its
ceiling, the sun shining down through it. Smoke (tests/scenes/volume.vdb's
cloud, scaled to fill the room), froxel fog and a translucent pane out of the
beam must be lit only where the window lets the sun in, with eldr:rtShadows on
as with shadow maps (the traced image covers opaque pixels only; the sun keeps
its cascades for these). Then the same with a sphere light above the window
(traced local lights keep their atlas tiles). Float captures.
Run: usdenv.cmd python tests\\test_traced_media.py"""
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

sun = UsdLux.DistantLight.Define(stage, "/sun")
sun.CreateIntensityAttr(40)
sun.CreateNormalizeAttr(True)
sun.AddRotateXYZOp().Set(Gf.Vec3f(-90, 0, 0))  # straight down

cloud = UsdVol.Volume.Define(stage, "/cloud")
cloud.AddTranslateOp().Set(Gf.Vec3d(0, 3, 0))
cloud.AddScaleOp().Set(Gf.Vec3f(5, 3.5, 5))
field = UsdVol.OpenVDBAsset.Define(stage, "/cloud/density")
field.CreateFilePathAttr(Sdf.AssetPath(VDB))
field.CreateFieldNameAttr("density")
cloud.CreateFieldRelationship("density", field.GetPath())

camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 3, 4.8))
camera.CreateFocalLengthAttr(14)
gfCamera = camera.GetCamera(Usd.TimeCode.Default())
frustum = gfCamera.frustum
viewProj = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()

params = UsdImagingGL.RenderParams()
failures = []


def expect(ok, what):
    print("ok  " if ok else "FAIL", what, file=sys.stderr if not ok else sys.stdout)
    if not ok:
        failures.append(what)


def render(**settings):
    """Converged float RGB, top row first."""
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    for key, value in {"eldr:autoExposure": False, **settings}.items():
        engine.SetRendererSetting(key, value)
    for _ in range(80):
        engine.Render(stage.GetPseudoRoot(), params)
        if engine.IsConverged():
            break
    data = GL.glReadPixels(0, 0, SIZE, SIZE, GL.GL_RGBA, GL.GL_FLOAT)
    del engine
    return np.frombuffer(data, np.float32).reshape(SIZE, SIZE, 4)[::-1, :, :3].copy()


def at(image, x, y, z):
    """Mean of the 3 x 3 pixels showing world (x, y, z)."""
    p = viewProj.Transform(Gf.Vec3d(x, y, z))
    px, py = int((p[0] * 0.5 + 0.5) * SIZE), int((0.5 - p[1] * 0.5) * SIZE)
    return float(image[py - 1:py + 2, px - 1:px + 2].mean())


def compare(name, maps, traced, beam, shade):
    """Traced within tolerance of maps; the beam bright, the shade dark."""
    rel = float(np.abs(traced - maps).mean() / max(maps.mean(), 1e-6))
    b, s = at(traced, *beam), at(traced, *shade)
    print("%s: maps mean %.4f traced %.4f, mean |diff| %.3f of the mean; beam %.4f shade %.4f (maps %.4f %.4f)" %
          (name, maps.mean(), traced.mean(), rel, b, s, at(maps, *beam), at(maps, *shade)))
    expect(rel < 0.1, "%s: traced matches the shadow maps" % name)
    expect(s < 0.25 * b, "%s: lit only in the window's beam" % name)
    expect(s < at(maps, *shade) * 1.5 + 1e-3, "%s: the walls block the traced light" % name)


# Smoke: the beam crosses the cloud's middle; (-3, 3, 0) is out of it.
# Multiple scattering as the tolerance was set with (single scattering
# halves the mean, the maps-traced difference, about 0.005, stays).
smoke = {"eldr:volumeDensity": 0.1, "eldr:object:volumelimit": 16}
compare("smoke", render(**smoke), render(**{"eldr:rtShadows": True, **smoke}), (0, 3, 0), (-3, 3, 0))

# Froxel fog instead of the smoke.
cloud.GetPrim().SetActive(False)
fog = {"eldr:fog": True, "eldr:fogDensity": 0.15, "eldr:fogAnisotropy": 0.0}
compare("fog", render(**fog), render(**{"eldr:rtShadows": True, **fog}), (0, 3, 0), (-3, 3, 0))

# A translucent pane out of the beam, facing the camera at an angle so the
# sun (straight down) would light it from above: dark under the ceiling.
pane = UsdGeom.Mesh.Define(stage, "/pane")
pane.CreatePointsAttr([(-4, 1, -2), (-2, 1, -2), (-2, 4, -3), (-4, 4, -3)])
pane.CreateFaceVertexCountsAttr([4])
pane.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
pane.CreateDoubleSidedAttr(True)
material = UsdShade.Material.Define(stage, "/glass")
shader = UsdShade.Shader.Define(stage, "/glass/pbr")
shader.CreateIdAttr("UsdPreviewSurface")
shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set((0.9, 0.9, 0.9))
shader.CreateInput("opacity", Sdf.ValueTypeNames.Float).Set(0.5)
material.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
UsdShade.MaterialBindingAPI.Apply(pane.GetPrim()).Bind(material)
maps, traced = render(), render(**{"eldr:rtShadows": True})
UsdGeom.Imageable(stage.GetPrimAtPath("/room/ceil1")).MakeInvisible()
open_ = render(**{"eldr:rtShadows": True})
p, q, o = at(maps, -3, 2.5, -2.5), at(traced, -3, 2.5, -2.5), at(open_, -3, 2.5, -2.5)
print("pane: maps %.4f traced %.4f, without the ceiling %.4f" % (p, q, o))
expect(o > 0.01 and q < 0.1 * o and q < p * 1.5 + 1e-3, "translucent pane: the ceiling blocks the traced sun")

# The same with a traced sphere light above the window instead of the sun:
# traced local lights keep their atlas tiles for media and glass.
UsdGeom.Imageable(stage.GetPrimAtPath("/room/ceil1")).MakeVisible()
sun.GetPrim().SetActive(False)
bulb = UsdLux.SphereLight.Define(stage, "/bulb")
bulb.CreateRadiusAttr(0.2)
bulb.CreateIntensityAttr(4000)
bulb.AddTranslateOp().Set(Gf.Vec3d(0, 9, 0))
pane.GetPrim().SetActive(False)
cloud.GetPrim().SetActive(True)
compare("smoke, sphere light", render(**smoke), render(**{"eldr:rtShadows": True, **smoke}), (0, 3, 0),
        (-3.5, 3, 0))
cloud.GetPrim().SetActive(False)
compare("fog, sphere light", render(**fog), render(**{"eldr:rtShadows": True, **fog}), (0, 3, 0), (-3.5, 3, 0))
pane.GetPrim().SetActive(True)
maps, traced = render(), render(**{"eldr:rtShadows": True})
UsdGeom.Imageable(stage.GetPrimAtPath("/room/ceil1")).MakeInvisible()
open_ = render(**{"eldr:rtShadows": True})
p, q, o = at(maps, -3, 2.5, -2.5), at(traced, -3, 2.5, -2.5), at(open_, -3, 2.5, -2.5)
print("pane, sphere light: maps %.4f traced %.4f, without the ceiling %.4f" % (p, q, o))
expect(o > 0.01 and q < 0.1 * o and q < p * 1.5 + 1e-3, "translucent pane: the ceiling blocks the traced sphere light")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
