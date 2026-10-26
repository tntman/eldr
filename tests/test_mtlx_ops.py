"""Headless check of MaterialX nodes in Eldr's material programs, one inline
graph per quad: each graph's node "out" (color3) drives standard_surface
emission (base and specular off, no lights, no post processing), so the
pixel at the quad's centre (uv 0.5, 0.5) is the graph's value.
Run: usdenv.cmd python tests\\test_mtlx_ops.py [case ...]"""
import math
import os
import sys
import tempfile

from PySide6.QtCore import QSize
from PySide6.QtGui import QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdShade


def f(name, node, inputs, typ="float"):
    """A node `name` of category `node`: inputs {name: (type, value or '@node')}."""
    xs = []
    for k, (t, v) in inputs.items():
        src = 'nodename="%s"' % v[1:] if isinstance(v, str) and v.startswith("@") else 'value="%s"' % v
        xs.append('<input name="%s" type="%s" %s/>' % (k, t, src))
    return '<%s name="%s" type="%s">%s</%s>' % (node, name, typ, "".join(xs), node)


def out_float(src):
    """`out`: a float node's value as grey."""
    return f("out", "convert", {"in": ("float", "@" + src)}, "color3")


# (label, nodes (the last one "out", color3), expected rgb at uv (0.5, 0.5))
CASES = [
    # A node Eldr can't compile is bypassed: hextiledimage's pass-through is its `default`.
    ("bypass hextiledimage -> default",
     f("c", "constant", {"value": ("color3", "0.2, 0.4, 0.6")}, "color3") +
     f("out", "hextiledimage", {"default": ("color3", "@c")}, "color3"), (0.2, 0.4, 0.6)),
    # Nodegraph-implemented nodes are expanded: blur's is a constant of `in`.
    ("nodegraph blur",
     f("c", "constant", {"value": ("color3", "0.3, 0.2, 0.1")}, "color3") +
     f("out", "blur", {"in": ("color3", "@c")}, "color3"), (0.3, 0.2, 0.1)),
    # contrast: (in - pivot) amount + pivot, pivot from its nodedef (0.5).
    ("nodegraph contrast", f("k", "contrast", {"in": ("float", "0.75"), "amount": ("float", "1.5")}) + out_float("k"),
     (0.875,) * 3),
    # ramp4 at uv (0.5, 0.5), texcoord from its defaultgeomprop UV0: the corners' mean.
    ("nodegraph ramp4 (UV0)",
     f("out", "ramp4", {"valuetl": ("color3", "1, 0, 0"), "valuetr": ("color3", "0, 1, 0"),
                        "valuebl": ("color3", "0, 0, 1"), "valuebr": ("color3", "1, 1, 1")}, "color3"),
     (0.5, 0.5, 0.5)),
    # geompropvalue without a geomprop reads its default.
    ("geompropvalue without a name",
     f("out", "geompropvalue", {"default": ("color3", "0.3, 0.5, 0.7")}, "color3"), (0.3, 0.5, 0.7)),
]

SIZE = 512
COLS = 8
CELL = 2.0

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

only = set(sys.argv[1:])
cases = [c for c in CASES if not only or c[0] in only]
folder = tempfile.mkdtemp()
mtlx = os.path.join(folder, "cases.mtlx")
with open(mtlx, "w") as fh:
    fh.write('<?xml version="1.0"?>\n<materialx version="1.39">\n')
    for i, (label, nodes, _) in enumerate(cases):
        fh.write('<nodegraph name="g%d">%s<output name="o" type="color3" nodename="out"/></nodegraph>\n' % (i, nodes))
        fh.write('<standard_surface name="s%d" type="surfaceshader"><input name="base" type="float" value="0"/>'
                 '<input name="specular" type="float" value="0"/><input name="emission" type="float" value="1"/>'
                 '<input name="emission_color" type="color3" nodegraph="g%d" output="o"/></standard_surface>\n'
                 % (i, i))
        fh.write('<surfacematerial name="m%d" type="material"><input name="surfaceshader" type="surfaceshader" '
                 'nodename="s%d"/></surfacematerial>\n' % (i, i))
    fh.write("</materialx>\n")

stage = Usd.Stage.CreateInMemory()
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
mats = stage.DefinePrim("/Mtlx")
mats.GetReferences().AddReference(mtlx.replace("\\", "/"), "/MaterialX")
rows = math.ceil(len(cases) / COLS)
half = CELL * 0.45


def centre(i):
    return ((i % COLS - (COLS - 1) / 2) * CELL, ((rows - 1) / 2 - i // COLS) * CELL)


for i in range(len(cases)):
    x, y = centre(i)
    mesh = UsdGeom.Mesh.Define(stage, "/q%d" % i)
    mesh.CreatePointsAttr([(x - half, y - half, 0), (x + half, y - half, 0), (x + half, y + half, 0), (x - half, y + half, 0)])
    mesh.CreateFaceVertexCountsAttr([4])
    mesh.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    mesh.CreateNormalsAttr([(0, 0, 1)] * 4)
    mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    mesh.CreateSubdivisionSchemeAttr("none")
    UsdGeom.PrimvarsAPI(mesh).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex).Set([(0, 0), (1, 0), (1, 1), (0, 1)])
    material = UsdShade.Material(stage.GetPrimAtPath("/Mtlx/Materials/m%d" % i))
    assert material, "no material m%d" % i
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)

width = COLS * CELL
cam = Gf.Camera()
cam.projection = Gf.Camera.Orthographic
cam.horizontalAperture = width * 10
cam.verticalAperture = width * 10
cam.clippingRange = Gf.Range1f(1, 100)
cam.transform = Gf.Matrix4d().SetTranslate(Gf.Vec3d(0, 0, 10))
frustum = cam.frustum
viewProj = frustum.ComputeViewMatrix() * frustum.ComputeProjectionMatrix()

engine = UsdImagingGL.Engine()
assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
engine.SetRendererSetting("eldr:postProcess", False)
engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
params = UsdImagingGL.RenderParams()
params.clearColor = Gf.Vec4f(0, 0, 0, 1)
for _ in range(2):  # textures arrive with the first commit
    engine.Render(stage.GetPseudoRoot(), params)
image = fbo.toImage()

failures = []
for i, (label, _, rgb) in enumerate(cases):
    x, y = centre(i)
    p = viewProj.Transform(Gf.Vec3d(x, y, 0))
    px, py = int((p[0] * 0.5 + 0.5) * SIZE), int((0.5 - p[1] * 0.5) * SIZE)
    c = image.pixelColor(px, py)
    got = (c.redF(), c.greenF(), c.blueF())
    if any(abs(g - e) > 0.02 for g, e in zip(got, rgb)):
        failures.append(label)
        print("FAIL: %s: expected %s, got %s" % (label, tuple(round(e, 3) for e in rgb), tuple(round(g, 3) for g in got)))

# The render stats count the bypassed blur and name it (usdview's Eldr > Render Statistics, Houdini's viewstats).
stats = engine.GetRenderStats()
if not only and not (stats.get("materialFallbackInputs") == 1 and "ND_hextiledimage_color3" in stats.get("materialFallbackNodes", "")):
    failures.append("stats")
    print("FAIL: fallback stats", stats.get("materialFallbackInputs"), stats.get("materialFallbackNodes"))

del engine  # release GL resources while the context is still current
print("FAILED" if failures else "OK (%d cases)" % len(cases))
sys.exit(1 if failures else 0)
