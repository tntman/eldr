"""Headless check of MaterialX nodes in Eldr's material programs, one inline
graph per quad: each graph's node "out" (color3) drives standard_surface
emission (base and specular off, no lights, no post processing), so the
pixel at the quad's centre (uv 0.5, 0.5) is the graph's value.
Run: usdenv.cmd python tests\\test_mtlx_ops.py [case ...]"""
import math
import os
import struct
import sys
import tempfile
import zlib

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


def c(name, typ, value):
    return f(name, "constant", {"value": (typ, value)}, typ)


def grey(label, node, inputs, value, typ="float"):
    """A case: float `node` of `inputs`, as grey."""
    return (label, f("k", node, inputs, typ) + out_float("k"), (value,) * 3)


def u_times(k, name="h"):
    """`name`: texcoord u times k (float)."""
    return (f(name + "uv", "texcoord", {}, "vector2") +
            f(name + "u", "extract", {"in": ("vector2", "@%suv" % name), "index": ("integer", "0")}) +
            f(name, "multiply", {"in1": ("float", "@%su" % name), "in2": ("float", str(k))}))


# (label, nodes (the last one "out", color3; a vector3 node "nrm" drives the surface's
# normal), expected rgb at uv (0.5, 0.5))
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
    # heighttonormal of h = 16 u: dh/du 16 times 1/16 (Sobel parity) tilts the normal 45 degrees
    # against u, encoded: (-1, 0, 1) / sqrt 2 * 0.5 + 0.5.
    ("heighttonormal", u_times(16) + f("n", "heighttonormal", {"in": ("float", "@h")}, "vector3") +
     f("out", "convert", {"in": ("vector3", "@n")}, "color3"), (0.5 - 0.5 ** 1.5, 0.5, 0.5 + 0.5 ** 1.5)),
    # bump (heighttonormal into normalmap) on the normal compiles (the stats show no fallback).
    ("bump on the normal",
     u_times(4) + f("nrm", "bump", {"height": ("float", "@h"), "scale": ("float", "0.5")}, "vector3") +
     f("out", "constant", {"value": ("color3", "0.25, 0.5, 0.75")}, "color3"), (0.25, 0.5, 0.75)),
    # MaterialX math (genglsl's formulas).
    grey("fract", "fract", {"in": ("float", "2.3")}, 0.3),
    grey("round", "round", {"in": ("float", "0.7")}, 1.0),
    ("sign", f("s", "sign", {"in": ("float", "-2")}) + f("k", "multiply", {"in1": ("float", "@s"), "in2": ("float", "-0.4")})
     + out_float("k"), (0.4,) * 3),
    grey("tan", "tan", {"in": ("float", "0.5")}, math.tan(0.5)),
    grey("asin", "asin", {"in": ("float", "0.5")}, math.asin(0.5)),
    grey("acos", "acos", {"in": ("float", "0.9")}, math.acos(0.9)),
    grey("atan2", "atan2", {"iny": ("float", "1"), "inx": ("float", "2")}, math.atan2(1, 2)),
    grey("magnitude", "magnitude", {"in": ("vector3", "0.3, 0.4, 0")}, 0.5),
    ("crossproduct", f("x", "crossproduct", {"in1": ("vector3", "1, 0, 0"), "in2": ("vector3", "0, 1, 0")}, "vector3") +
     f("out", "convert", {"in": ("vector3", "@x")}, "color3"), (0, 0, 1)),
    ("rotate2d", f("r", "rotate2d", {"in": ("vector2", "0, 0.5"), "amount": ("float", "90")}, "vector2") +
     f("k", "extract", {"in": ("vector2", "@r"), "index": ("integer", "0")}) + out_float("k"), (0.5,) * 3),
    ("rotate3d", f("r", "rotate3d", {"in": ("vector3", "0.5, 0, 0"), "amount": ("float", "-90"),
                                     "axis": ("vector3", "0, 0, 1")}, "vector3") +
     f("out", "convert", {"in": ("vector3", "@r")}, "color3"), (0, 0.5, 0)),
    ("switch, constant which",
     f("out", "switch", {"in1": ("color3", "1, 0, 0"), "in3": ("color3", "0.1, 0.2, 0.3"), "which": ("float", "2")},
       "color3"), (0.1, 0.2, 0.3)),
    ("switch, which 3 u = 1.5", u_times(3) +
     f("out", "switch", {"in1": ("color3", "1, 0, 0"), "in2": ("color3", "0.2, 0.4, 0.6"), "in3": ("color3", "0, 0, 1"),
                         "which": ("float", "@h")}, "color3"), (0.2, 0.4, 0.6)),
    ("logical or, xor, and", f("a", "or", {"in1": ("boolean", "false"), "in2": ("boolean", "true")}, "boolean") +
     f("b", "xor", {"in1": ("boolean", "true"), "in2": ("boolean", "true")}, "boolean") +
     f("n", "not", {"in": ("boolean", "false")}, "boolean") +
     f("d", "and", {"in1": ("boolean", "@n"), "in2": ("boolean", "true")}, "boolean") +
     f("af", "convert", {"in": ("boolean", "@a")}) + f("bf", "convert", {"in": ("boolean", "@b")}) +
     f("df", "convert", {"in": ("boolean", "@d")}) +
     f("out", "combine3", {"in1": ("float", "@af"), "in2": ("float", "@bf"), "in3": ("float", "@df")}, "color3"),
     (1, 0, 1)),
    ("splitlr (center 0.4: right)",
     f("out", "splitlr", {"valuel": ("color3", "1, 0, 0"), "valuer": ("color3", "0, 0.5, 0"), "center": ("float", "0.4")},
       "color3"), (0, 0.5, 0)),
    ("splittb (center 0.6: bottom)",
     f("out", "splittb", {"valuet": ("color3", "1, 0, 0"), "valueb": ("color3", "0, 0, 0.5"), "center": ("float", "0.6")},
       "color3"), (0, 0, 0.5)),
    # Rendered at frame 12 of a 24 fps stage.
    grey("time", "time", {}, 0.5),
    ("frame", f("t", "frame", {}) + f("k", "multiply", {"in1": ("float", "@t"), "in2": ("float", "0.04")}) + out_float("k"),
     (0.48,) * 3),
    ("tangent (world): +x", f("t", "tangent", {"space": ("string", "world")}, "vector3") +
     f("out", "convert", {"in": ("vector3", "@t")}, "color3"), (1, 0, 0)),
    ("bitangent (world): +y", f("t", "bitangent", {"space": ("string", "world")}, "vector3") +
     f("out", "convert", {"in": ("vector3", "@t")}, "color3"), (0, 1, 0)),
    ("viewdirection is a unit vector", f("v", "viewdirection", {}, "vector3") + f("k", "magnitude", {"in": ("vector3", "@v")})
     + out_float("k"), (1,) * 3),
    ("artistic_ior", f("i", "artistic_ior", {"reflectivity": ("color3", "0.5, 0.5, 0.5"),
                                             "edge_color": ("color3", "0.5, 0.5, 0.5")}, "multioutput") +
     '<multiply name="out" type="color3"><input name="in1" type="color3" nodename="i" output="ior"/>'
     '<input name="in2" type="float" value="0.25"/></multiply>',
     (((1 + 0.5 ** 0.5) / (1 - 0.5 ** 0.5) + 1 / 3) / 2 * 0.25,) * 3),  # Gulbrandsen's ior, a quarter
    ("transformmatrix vector3 by matrix33",
     f("m", "transformmatrix", {"in": ("vector3", "0.2, 0.3, 0.4"), "mat": ("matrix33", "0,1,0, 0,0,1, 1,0,0")}, "vector3") +
     f("out", "convert", {"in": ("vector3", "@m")}, "color3"), (0.4, 0.2, 0.3)),
    ("transformmatrix vector3 by matrix44 (a point)",
     f("m", "transformmatrix", {"in": ("vector3", "0.2, 0.3, 0.4"),
                                "mat": ("matrix44", "1,0,0,0, 0,1,0,0, 0,0,1,0, 0.1,0.2,0.3,1")}, "vector3") +
     f("out", "convert", {"in": ("vector3", "@m")}, "color3"), (0.3, 0.5, 0.7)),
]


def blend(node, fg, bg, mix, rgb, typ="color3"):
    """A compositing case: `node` of fg over bg mixed by mix (color4 shown as its rgb)."""
    k = f("k", node, {"fg": (typ, fg), "bg": (typ, bg), "mix": ("float", mix)}, typ)
    return (node, k + (f("out", "convert", {"in": (typ, "@k")}, "color3") if typ != "color3" else
                       k.replace('name="k"', 'name="out"')), rgb)


# MaterialX's compositing nodes (genglsl's formulas).
CASES += [
    blend("plus", "0.1, 0.2, 0.3", "0.2, 0.2, 0.2", "1", (0.3, 0.4, 0.5)),
    blend("minus", "0.1, 0.2, 0.3", "0.5, 0.5, 0.5", "0.5", (0.45, 0.4, 0.35)),
    blend("difference", "0.2, 0.9, 0.5", "0.7, 0.4, 0.5", "1", (0.5, 0.5, 0.0)),
    # burn 1 - (1 - B) / F, 0 where F is 0; dodge B / (1 - F), 0 where F is 1.
    blend("burn", "0.5, 0, 0.8", "0.75, 0.75, 0.6", "1", (0.5, 0.0, 0.5)),
    blend("dodge", "0.5, 1, 0.5", "0.25, 0.5, 0.2", "0.5", (0.375, 0.0, 0.3)),
    blend("screen", "0.5, 0.2, 0", "0.5, 0.5, 0.4", "1", (0.75, 0.6, 0.4)),
    blend("over", "0.2, 0.4, 0.6, 0.5", "1, 0, 0, 1", "1", (0.7, 0.4, 0.6), "color4"),
    # f + b > 1: F + B (1 - f) / b.
    blend("disjointover", "0.2, 0.2, 0.2, 0.6", "0.5, 0.5, 0.5, 0.8", "1", (0.45,) * 3, "color4"),
    blend("in", "0.4, 0.6, 0.8, 1", "0, 0, 0, 0.5", "1", (0.2, 0.3, 0.4), "color4"),
    blend("matte", "0.4, 0.6, 0.8, 0.25", "0.8, 0.4, 0, 1", "1", (0.7, 0.45, 0.2), "color4"),
    ("premult", f("k", "premult", {"in": ("color4", "0.8, 0.4, 0.2, 0.5")}, "color4") +
     f("out", "convert", {"in": ("color4", "@k")}, "color3"), (0.4, 0.2, 0.1)),
    grey("inside", "inside", {"in": ("float", "0.8"), "mask": ("float", "0.5")}, 0.4),
    grey("outside", "outside", {"in": ("float", "0.8"), "mask": ("float", "0.25")}, 0.6),
]



def karma_bump(stage, path):
    """A Karma-context material (Houdini's: outputs:kma:surface) whose emission is
    bump(height = -1.8 u) as a colour: Karma's scene units on a 1.8 wide quad
    slope 1, so the world normal tilts 45 degrees towards +x."""
    material = UsdShade.Material.Define(stage, path)

    def shader(name, id_):
        s = UsdShade.Shader.Define(stage, path + "/" + name)
        s.CreateIdAttr(id_)
        return s

    tc = shader("tc", "ND_texcoord_vector2")
    ex = shader("ex", "ND_extract_vector2")
    ex.CreateInput("in", Sdf.ValueTypeNames.Float2).ConnectToSource(tc.ConnectableAPI(), "out")
    ex.CreateInput("index", Sdf.ValueTypeNames.Int).Set(0)
    h = shader("h", "ND_multiply_float")
    h.CreateInput("in1", Sdf.ValueTypeNames.Float).ConnectToSource(ex.ConnectableAPI(), "out")
    h.CreateInput("in2", Sdf.ValueTypeNames.Float).Set(-1.8)
    bump = shader("bump", "ND_bump_vector3")
    bump.CreateInput("height", Sdf.ValueTypeNames.Float).ConnectToSource(h.ConnectableAPI(), "out")
    cv = shader("cv", "ND_convert_vector3_color3")
    cv.CreateInput("in", Sdf.ValueTypeNames.Vector3f).ConnectToSource(bump.ConnectableAPI(), "out")
    surf = shader("surf", "ND_standard_surface_surfaceshader")
    for name, v in (("base", 0.0), ("specular", 0.0), ("emission", 1.0)):
        surf.CreateInput(name, Sdf.ValueTypeNames.Float).Set(v)
    surf.CreateInput("emission_color", Sdf.ValueTypeNames.Color3f).ConnectToSource(cv.ConnectableAPI(), "out")
    material.CreateSurfaceOutput("kma").ConnectToSource(surf.ConnectableAPI(), "out")
    return material


def usd_shader(stage, path, id_, **inputs):
    """A shader: inputs name=(Sdf type, value) or name=(shader, output) to connect."""
    s = UsdShade.Shader.Define(stage, path)
    s.CreateIdAttr(id_)
    for name, (a, b) in inputs.items():
        if isinstance(a, UsdShade.Shader):
            s.CreateInput(name, Sdf.ValueTypeNames.Token).ConnectToSource(a.ConnectableAPI(), b)
        else:
            s.CreateInput(name, a).Set(b)
    return s


def emitting(stage, path, node, output="out", convert="ND_convert_float_color3"):
    """A material whose standard_surface emission is `node`'s output (through `convert`)."""
    material = UsdShade.Material.Define(stage, path)
    cv = usd_shader(stage, path + "/cv", convert, **{"in": (node, output)}) if convert else node
    surf = usd_shader(stage, path + "/surf", "ND_standard_surface_surfaceshader", base=(Sdf.ValueTypeNames.Float, 0.0),
                      specular=(Sdf.ValueTypeNames.Float, 0.0), emission=(Sdf.ValueTypeNames.Float, 1.0),
                      emission_color=(cv, "out" if convert else output))
    material.CreateSurfaceOutput("mtlx").ConnectToSource(surf.ConnectableAPI(), "out")
    return material


def surface_material(stage, path, id_, **inputs):
    material = UsdShade.Material.Define(stage, path)
    surf = usd_shader(stage, path + "/surf", id_, **inputs)
    material.CreateSurfaceOutput("mtlx").ConnectToSource(surf.ConnectableAPI(), "out")
    return material


def kma_ramp_float(stage, path):
    """Karma's Ramp Const, monotone cubic over keys 0 and 1 (flat at both ends: smoothstep) at 0.25."""
    T = Sdf.ValueTypeNames
    r = usd_shader(stage, path + "/r", "kma_rampconst_float", t=(T.Float, 0.25), framp=(T.Int, 2),
                   fkeys=(T.FloatArray, [0, 1]), fvalues=(T.FloatArray, [0, 1]),
                   fbasis=(T.TokenArray, ["monotonecubic", "monotonecubic"]))
    return emitting(stage, path, r)


def kma_ramp_color(stage, path):
    """A linear colour Ramp Const at 0.25, its keys out of order (Houdini sorts them)."""
    T = Sdf.ValueTypeNames
    r = usd_shader(stage, path + "/r", "kma_rampconst_color", t=(T.Float, 0.25), vramp=(T.Int, 2),
                   vkeys=(T.FloatArray, [1, 0]), vvalues=(T.Color3fArray, [(0, 0, 1), (1, 0, 0)]),
                   vbasis=(T.TokenArray, ["linear", "linear"]))
    return emitting(stage, path, r, convert=None)


def kma_voronoi(stage, path):
    """Karma's Voronoi Noise 3D without jitter at (0.5, 0.5, 0.5) - (0.25, 0, 0): F1 to the cell's centre, 0.25."""
    T = Sdf.ValueTypeNames
    v = usd_shader(stage, path + "/v", "kma_voronoinoise3d_float", position=(T.Vector3f, (0.5, 0.5, 0.5)),
                   offset=(T.Vector3f, (0.25, 0, 0)), jitter=(T.Float, 0.0), metric=(T.Int, 0))
    return emitting(stage, path, v, "distance")


def kma_hextiling(stage, path):
    """Karma's hex tiling of a uniform texture: its colour, whatever the tiles' weights."""
    T = Sdf.ValueTypeNames
    h = usd_shader(stage, path + "/h", "kma_color_hextiling", file=(T.Asset, UNIFORM_PNG),
                   sourceColorSpace=(T.Token, "raw"), size=(T.Float, 0.3), contrast=(T.Float, 0.725))
    return emitting(stage, path, h, convert="ND_convert_color4_color3")


def unlit(stage, path):
    T = Sdf.ValueTypeNames
    return surface_material(stage, path, "ND_surface_unlit", emission=(T.Float, 0.5),
                            emission_color=(T.Color3f, (0.4, 0.8, 1.0)))


def gltf(stage, path):
    """glTF PBR: unlit but for its emissive (no lights): base black, no specular."""
    T = Sdf.ValueTypeNames
    return surface_material(stage, path, "ND_gltf_pbr_surfaceshader", base_color=(T.Color3f, (0, 0, 0)),
                            specular=(T.Float, 0.0), emissive=(T.Color3f, (0.3, 0.5, 0.7)),
                            emissive_strength=(T.Float, 1.0))


def disney(stage, path):
    """Disney principled has no emission: black without lights (and read: no fallback in the stats)."""
    return surface_material(stage, path, "ND_disney_principled", baseColor=(Sdf.ValueTypeNames.Color3f, (0.8, 0.2, 0.2)))


# Cases authored as USD (a function making the material, expected rgb).
USD_CASES = [("bump in scene units (Karma context)", karma_bump, (0.5 ** 0.5, 0, 0.5 ** 0.5)),
             ("kma_rampconst monotone cubic", kma_ramp_float, (0.15625,) * 3),
             ("kma_rampconst colour", kma_ramp_color, (0.75, 0, 0.25)),
             ("kma_voronoinoise3d", kma_voronoi, (0.25,) * 3),
             ("kma_color_hextiling", kma_hextiling, (64 / 255, 128 / 255, 192 / 255)),
             ("surface_unlit", unlit, (0.2, 0.4, 0.5)),
             ("gltf_pbr emissive", gltf, (0.3, 0.5, 0.7)),
             ("disney_principled", disney, (0, 0, 0))]

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
cases = [c for c in CASES + USD_CASES if not only or c[0] in only]
folder = tempfile.mkdtemp()
# A uniform 8-bit texture (64, 128, 192) for the hex tiling case.
UNIFORM_PNG = os.path.join(folder, "uniform.png").replace("\\", "/")
with open(UNIFORM_PNG, "wb") as fh:
    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))
    fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 2, 0, 0, 0)) +
             chunk(b"IDAT", zlib.compress(b"".join(b"\x00" + bytes((64, 128, 192)) * 8 for _ in range(8)))) +
             chunk(b"IEND", b""))
mtlx = os.path.join(folder, "cases.mtlx")
with open(mtlx, "w") as fh:
    fh.write('<?xml version="1.0"?>\n<materialx version="1.39">\n')
    for i, (label, nodes, _) in enumerate(cases):
        if callable(nodes):
            continue
        normal = 'name="nrm"' in nodes
        fh.write('<nodegraph name="g%d">%s<output name="o" type="color3" nodename="out"/>%s</nodegraph>\n'
                 % (i, nodes, '<output name="n" type="vector3" nodename="nrm"/>' if normal else ""))
        fh.write('<standard_surface name="s%d" type="surfaceshader"><input name="base" type="float" value="0"/>'
                 '<input name="specular" type="float" value="0"/><input name="emission" type="float" value="1"/>'
                 '<input name="emission_color" type="color3" nodegraph="g%d" output="o"/>%s</standard_surface>\n'
                 % (i, i, '<input name="normal" type="vector3" nodegraph="g%d" output="n"/>' % i if normal else ""))
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
    material = cases[i][1](stage, "/Usd/m%d" % i) if callable(cases[i][1]) else \
        UsdShade.Material(stage.GetPrimAtPath("/Mtlx/Materials/m%d" % i))
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
params.frame = Usd.TimeCode(12)  # MaterialX time and frame
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
