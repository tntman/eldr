"""Transmission through Hydra: standard_surface and OpenPBR transmission
(thin-walled, so nothing bends) tints a white wall behind it, and with
eldr:refraction off it shows straight through, untinted.
Run: usdenv.cmd python tests\\test_transmission.py"""
import sys

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux, UsdShade

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


def quad(path, x0, y0, x1, y1, z):
    mesh = UsdGeom.Mesh.Define(stage, path)
    mesh.CreatePointsAttr([(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateSubdivisionSchemeAttr("none")
    return mesh


def material(path, shaderId, inputs, mtlx=True):
    mat = UsdShade.Material.Define(stage, path)
    shader = UsdShade.Shader.Define(stage, path + "/surface")
    shader.CreateIdAttr(shaderId)
    for name, (type, value) in inputs.items():
        shader.CreateInput(name, type).Set(value)
    out = shader.CreateOutput("out", Sdf.ValueTypeNames.Token)
    if mtlx:
        mat.CreateSurfaceOutput("mtlx").ConnectToSource(out)
    else:
        mat.CreateSurfaceOutput().ConnectToSource(out)
    return mat


def bind(mesh, mat):
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(mat)


F, C, B = Sdf.ValueTypeNames.Float, Sdf.ValueTypeNames.Color3f, Sdf.ValueTypeNames.Bool
bind(quad("/wall", -2, -2, 2, 2, -1),
     material("/mtl/wall", "UsdPreviewSurface",
              {"diffuseColor": (C, (0, 0, 0)), "emissiveColor": (C, (1, 1, 1))}, mtlx=False))
bind(quad("/red", -1, 0, 0, 1, 0),
     material("/mtl/red", "ND_standard_surface_surfaceshader",
              {"transmission": (F, 1.0), "transmission_color": (C, (1, 0, 0)), "thin_walled": (B, True)}))
bind(quad("/blue", -1, -1, 0, 0, 0),
     material("/mtl/blue", "ND_open_pbr_surface_surfaceshader",
              {"transmission_weight": (F, 1.0), "transmission_color": (C, (0, 0, 1)),
               "geometry_thin_walled": (B, True)}))
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.CreateProjectionAttr("orthographic")
camera.CreateHorizontalApertureAttr(40.0)  # tenths of a unit: 4 units across
camera.CreateVerticalApertureAttr(40.0)
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 5))
frustum = camera.GetCamera(Usd.TimeCode.Default()).frustum

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def render(**settings):
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    engine.SetRendererSetting("eldr:postProcess", False)
    for k, v in settings.items():
        engine.SetRendererSetting("eldr:" + k, v)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    params.clearColor = Gf.Vec4f(0, 0, 0, 1)
    for _ in range(2):
        engine.Render(stage.GetPseudoRoot(), params)
    image = fbo.toImage()
    del engine
    return image


def rgb(image, x, y):  # world (x, y) in [-2, 2]
    c = image.pixelColor(int((x + 2) / 4 * SIZE), int((2 - y) / 4 * SIZE))
    return (c.redF(), c.greenF(), c.blueF())


on = render()
red, blue, wall = rgb(on, -0.5, 0.5), rgb(on, -0.5, -0.5), rgb(on, 1, 0)
expect(wall[1] > 0.95, "the wall beside the glass is white")
expect(red[0] > 0.85 and red[1] < 0.05 and red[2] < 0.05, "standard_surface transmission_color tints red")
expect(blue[2] > 0.85 and blue[0] < 0.05 and blue[1] < 0.05, "OpenPBR transmission_color tints blue")
off = render(refraction=False)
red = rgb(off, -0.5, 0.5)
expect(red[1] > 0.85 and red[0] > 0.85, "eldr:refraction off: straight through, untinted")

# UsdPreviewSurface opacityMode: a half-opaque mirror-like quad in a dim sun over
# the white wall. "transparent" fades the diffuse light only, so the highlight
# stays whole; "presence" fades it with the rest; eldr:opacityMode off reads
# "presence" as "transparent".
sunLight = UsdLux.DistantLight.Define(stage, "/sun")
sunLight.CreateIntensityAttr(0.2)
mode = {}
for name, value in (("transparent", "transparent"), ("presence", "presence")):
    bind(quad("/" + name, 0.5 if name == "presence" else -1.5, -1, 1.5 if name == "presence" else -0.5, 0, 0.1),
         material("/mtl/" + name, "UsdPreviewSurface",
                  {"diffuseColor": (C, (1, 1, 1)), "metallic": (F, 1.0), "roughness": (F, 0.5), "opacity": (F, 0.5),
                   "opacityMode": (Sdf.ValueTypeNames.Token, value)}, mtlx=False))
lit, flat = render(), render(opacityMode=False)
t, p, pOff = rgb(lit, -1, -0.5)[0], rgb(lit, 1, -0.5)[0], rgb(flat, 1, -0.5)[0]
print(f"opacityMode: transparent {t:.3f}, presence {p:.3f}, presence with eldr:opacityMode off {pOff:.3f}")
expect(t - p > 0.03, "presence fades the highlight with the opacity, transparent keeps it")
expect(abs(pOff - t) < 0.02, "eldr:opacityMode off: presence shades as transparent")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
