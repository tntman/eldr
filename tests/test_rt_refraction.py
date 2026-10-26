"""Ray-traced refraction (eldr:rtRefraction) through Hydra, with an
orthographic camera:
- a glass ball (ior 1.5) bends rays from its rim far sideways, onto a red
  emissive card that is off screen: traced, the rim shows red; screen
  space (rtRefraction 0) can't show it;
- a slab of glass (ior 1.5, thickness 1) turned 45 degrees in front of a
  white/black edge shifts the edge sideways by t sin(45 - r) / cos(r), r the
  refracted angle (0.329 units); with its ior from a material graph (2,
  where the constant would be 1.5) by 0.440;
- glass seen through glass takes its transmission colour from its graph
  (red, where the constant would be white);
- a sheet of glass rippled by a sine height map bends the edge behind it
  into a wiggle (eldr:rtRefractionDisplacement); flat, it stays straight.
Needs a GPU with ray tracing.
Run: usdenv.cmd python tests\\test_rt_refraction.py"""
import math
import os
import sys
import tempfile

from PySide6.QtCore import QSize
from PySide6.QtGui import QColor, QImage, QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject, QOpenGLFramebufferObjectFormat
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdShade

SIZE = 256  # pixels over 4 units

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fboFormat = QOpenGLFramebufferObjectFormat()
fboFormat.setInternalTextureFormat(0x881A)  # GL_RGBA16F
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE), fboFormat)
fbo.bind()

C, F = Sdf.ValueTypeNames.Color3f, Sdf.ValueTypeNames.Float


def material(stage, path, inputs):
    mat = UsdShade.Material.Define(stage, path)
    shader = UsdShade.Shader.Define(stage, path + "/surface")
    shader.CreateIdAttr("UsdPreviewSurface")
    for name, (type, value) in inputs.items():
        shader.CreateInput(name, type).Set(value)
    mat.CreateSurfaceOutput().ConnectToSource(shader.CreateOutput("surface", Sdf.ValueTypeNames.Token))
    return mat


def mesh(stage, path, points, counts, indices, mat, normals=None):
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreatePointsAttr(points)
    m.CreateFaceVertexCountsAttr(counts)
    m.CreateFaceVertexIndicesAttr(indices)
    m.CreateSubdivisionSchemeAttr("none")
    if normals:
        m.CreateNormalsAttr(normals)
        m.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    UsdShade.MaterialBindingAPI.Apply(m.GetPrim()).Bind(mat)
    return m


def quad(stage, path, x0, y0, x1, y1, z, mat):
    return mesh(stage, path, [(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)], [4], [0, 1, 2, 3], mat)


def stage_with_camera():
    stage = Usd.Stage.CreateInMemory()
    camera = UsdGeom.Camera.Define(stage, "/cam")
    camera.CreateProjectionAttr("orthographic")
    camera.CreateHorizontalApertureAttr(40.0)  # tenths of a unit: 4 units across
    camera.CreateVerticalApertureAttr(40.0)
    camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 5))
    glass = material(stage, "/mtl/glass", {"opacity": (F, 0.0), "ior": (F, 1.5), "roughness": (F, 0.0),
                                            "diffuseColor": (C, (0, 0, 0))})
    return stage, camera.GetCamera(Usd.TimeCode.Default()).frustum, glass


def render(stage, frustum, rt, **settings):
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    engine.SetRendererSetting("eldr:postProcess", False)
    engine.SetRendererSetting("eldr:rtRefraction", 2 if rt else 0)
    for name, value in settings.items():
        engine.SetRendererSetting(name, value)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    params.clearColor = Gf.Vec4f(0, 0, 0, 1)
    for _ in range(3):
        engine.Render(stage.GetPseudoRoot(), params)
    image = fbo.toImage()
    del engine
    return image


def pixel(image, x, y):  # world (x, y) in [-2, 2]
    c = image.pixelColor(min(int((x + 2) / 4 * SIZE), SIZE - 1), min(int((2 - y) / 4 * SIZE), SIZE - 1))
    return c.redF(), c.greenF(), c.blueF()


failures = []


def expect(ok, what):
    print("ok:  " if ok else "FAIL:", what)
    if not ok:
        failures.append(what)


# The ball: radius 1 at the origin, the red card left of the frame.
stage, frustum, glass = stage_with_camera()
rings, segments = 48, 96
points, normals, indices = [], [], []
for i in range(rings + 1):
    theta = math.pi * i / rings
    for j in range(segments):
        phi = 2 * math.pi * j / segments
        n = (math.sin(theta) * math.cos(phi), math.cos(theta), math.sin(theta) * math.sin(phi))
        points.append(n)
        normals.append(n)
for i in range(rings):
    for j in range(segments):
        a, b = i * segments + j, i * segments + (j + 1) % segments
        indices += [a, b, b + segments, a + segments]
mesh(stage, "/ball", points, [4] * (rings * segments), indices, glass, normals)
red = material(stage, "/mtl/red", {"diffuseColor": (C, (0, 0, 0)), "emissiveColor": (C, (1, 0, 0))})
quad(stage, "/card", -9, -4, -2.5, 4, -3, red)


def red_pixels(image):
    count = 0
    for py in range(SIZE):
        for px in range(SIZE):
            x, y = px / SIZE * 4 - 2, 2 - py / SIZE * 4
            r, g, b = pixel(image, x, y)
            count += x * x + y * y < 0.95 and r > 0.2 and r > 4 * (g + b)
    return count


traced, screen = red_pixels(render(stage, frustum, True)), red_pixels(render(stage, frustum, False))
print("red pixels in the ball: traced", traced, "screen space", screen)
expect(traced > 200, "traced refraction shows the off-screen card through the ball")
expect(screen == 0, "screen-space refraction can't")

# The slab: 3 x 3 x 1, turned 45 degrees about y, in front of an edge
# (white emissive left of x = 0, black right of it).
stage, frustum, glass = stage_with_camera()
corners = [(x, y, z) for x in (-1.5, 1.5) for y in (-1.5, 1.5) for z in (-0.5, 0.5)]
faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
points, indices = [], []
for f in faces:  # four points per face: flat normals
    indices += [len(points) + k for k in range(4)]
    points += [corners[k] for k in f]
slab = mesh(stage, "/slab", points, [4] * 6, indices, glass)
slab.AddRotateYOp().Set(45.0)
white = material(stage, "/mtl/white", {"diffuseColor": (C, (0, 0, 0)), "emissiveColor": (C, (1, 1, 1))})
black = material(stage, "/mtl/black", {"diffuseColor": (C, (0, 0, 0))})
quad(stage, "/white", -4, -4, 0, 4, -3, white)
quad(stage, "/black", 0, -4, 4, 4, -3, black)


def edge(image):
    """Where the rows through y in [-0.3, 0.3] fall from white to black."""
    rows = [y / 20 for y in range(-6, 7)]
    xs = [px / SIZE * 4 - 2 for px in range(SIZE)]
    profile = [sum(pixel(image, x, y)[1] for y in rows) / len(rows) for x in xs]
    inside = [i for i, x in enumerate(xs) if -0.9 < x < 0.9]
    high = max(profile[i] for i in inside)
    for i in inside:
        if profile[i] < 0.5 * high:
            return xs[i] - 2 / SIZE  # the crossing lies within this pixel
    return None


r = math.asin(math.sin(math.radians(45)) / 1.5)
expected = math.sin(math.radians(45) - r) / math.cos(r)
shift = edge(render(stage, frustum, True))
print("edge shift: traced", shift, "expected", round(expected, 3))
expect(shift is not None and abs(abs(shift) - expected) < 0.05, "the slab shifts the edge by t sin(45 - r) / cos(r)")



def mtlx_glass(stage, path, inputs, graphs):
    """standard_surface glass; graphs: input -> (node id, output type, {input: value})."""
    mat = UsdShade.Material.Define(stage, path)
    shader = UsdShade.Shader.Define(stage, path + "/surface")
    shader.CreateIdAttr("ND_standard_surface_surfaceshader")
    for name, (type, value) in dict(inputs, base=(F, 0.0), transmission=(F, 1.0)).items():
        shader.CreateInput(name, type).Set(value)
    for name, (node, type, args) in graphs.items():
        g = UsdShade.Shader.Define(stage, path + "/" + name)
        g.CreateIdAttr(node)
        for arg, value in args.items():
            g.CreateInput(arg, type).Set(value)
        shader.CreateInput(name, type).ConnectToSource(g.CreateOutput("out", type).GetAttr().GetPath())
    mat.CreateSurfaceOutput("mtlx").ConnectToSource(shader.CreateOutput("out", Sdf.ValueTypeNames.Token))
    return mat


# The same slab with its ior from a graph: 1 + 1.
graphIor = mtlx_glass(stage, "/mtl/graphIor", {"specular_roughness": (F, 0.0)},
                      {"specular_IOR": ("ND_add_float", F, {"in1": 1.0, "in2": 1.0})})
UsdShade.MaterialBindingAPI(slab.GetPrim()).Bind(graphIor)
r = math.asin(math.sin(math.radians(45)) / 2.0)
expected = math.sin(math.radians(45) - r) / math.cos(r)
shift = edge(render(stage, frustum, True))
print("edge shift, ior from a graph: traced", shift, "expected", round(expected, 3))
expect(shift is not None and abs(abs(shift) - expected) < 0.05, "the first refraction bends by the program's ior")

# Glass behind glass: a clear pane in front of a pane whose transmission
# colour is a graph's red (1, 0, 0); the white card behind both shows red.
stage, frustum, glass = stage_with_camera()


def box(path, z0, z1, mat):
    corners = [(x, y, z) for x in (-1.5, 1.5) for y in (-1.5, 1.5) for z in (z0, z1)]
    points, indices = [], []
    for f in faces:
        indices += [len(points) + k for k in range(4)]
        points += [corners[k] for k in f]
    return mesh(stage, path, points, [4] * 6, indices, mat)


red = mtlx_glass(stage, "/mtl/red", {"specular_roughness": (F, 0.0)},
                 {"transmission_color": ("ND_add_color3", C, {"in1": (0.5, 0, 0), "in2": (0.5, 0, 0)})})
box("/front", 0.5, 1.0, glass)
box("/back", -1.0, -0.5, red)
white = material(stage, "/mtl/white", {"diffuseColor": (C, (0, 0, 0)), "emissiveColor": (C, (1, 1, 1))})
quad(stage, "/card", -4, -4, 4, 4, -3, white)
c = pixel(render(stage, frustum, True), 0.1, 0.1)
print("through both panes:", [round(x, 3) for x in c])
expect(c[0] > 0.3 and c[1] < 0.1 * c[0] and c[2] < 0.1 * c[0], "the second pane tints by its graph's colour")

# Displaced glass (eldr:rtRefractionDisplacement): a sheet of glass at z = 0
# rippled by a height map h = 0.05 (1 + sin(2 pi (x + y))) in front of the
# white/black edge. The ripple tilts the surface by up to 0.31 in x, which
# moves the edge by up to about 0.3 units after 3 units: the edge wiggles
# along y. Without displacement, or with the setting off (the displaced
# BLAS but its rest normals), the flat sheet leaves it straight.
textures = tempfile.mkdtemp()
ripple = QImage(256, 256, QImage.Format_Grayscale8)
for py in range(256):
    for px in range(256):
        u, v = (px + 0.5) / 256, 1 - (py + 0.5) / 256  # image rows run down, v up
        ripple.setPixel(px, py, QColor.fromRgbF(*[0.5 + 0.5 * math.sin(2 * math.pi * 4 * (u + v))] * 3).rgb())
ripple.save(os.path.join(textures, "ripple.png"))
stage, frustum, _ = stage_with_camera()
rippled = UsdShade.Material.Define(stage, "/mtl/rippled")
pbr = UsdShade.Shader.Define(stage, "/mtl/rippled/pbr")
pbr.CreateIdAttr("UsdPreviewSurface")
for name, value in {"opacity": 0.0, "ior": 1.5, "roughness": 0.0}.items():
    pbr.CreateInput(name, F).Set(value)
pbr.CreateInput("diffuseColor", C).Set((0, 0, 0))
height = UsdShade.Shader.Define(stage, "/mtl/rippled/height")
height.CreateIdAttr("UsdUVTexture")
height.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(os.path.join(textures, "ripple.png"))
height.CreateInput("sourceColorSpace", Sdf.ValueTypeNames.Token).Set("raw")
height.CreateInput("scale", Sdf.ValueTypeNames.Float4).Set((0.1, 0.1, 0.1, 1))
st = UsdShade.Shader.Define(stage, "/mtl/rippled/st")
st.CreateIdAttr("UsdPrimvarReader_float2")
st.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("st")
height.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(st.CreateOutput("result", Sdf.ValueTypeNames.Float2))
pbr.CreateInput("displacement", F).ConnectToSource(height.CreateOutput("r", F))
rippled.CreateSurfaceOutput().ConnectToSource(pbr.CreateOutput("surface", Sdf.ValueTypeNames.Token))
rippled.CreateDisplacementOutput().ConnectToSource(pbr.CreateOutput("displacement", Sdf.ValueTypeNames.Token))
cells = 128
points, uvs, indices = [], [], []
for j in range(cells + 1):
    for i in range(cells + 1):
        points.append((4 * i / cells - 2, 4 * j / cells - 2, 0))
        uvs.append((i / cells, j / cells))
for j in range(cells):
    for i in range(cells):
        a = j * (cells + 1) + i
        indices += [a, a + 1, a + cells + 2, a + cells + 1]
sheet = mesh(stage, "/sheet", points, [4] * cells * cells, indices, rippled, [(0, 0, 1)] * len(points))
UsdGeom.PrimvarsAPI(sheet).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray,
                                         UsdGeom.Tokens.vertex).Set(uvs)
quad(stage, "/white", -4, -4, 0, 4, -3, material(stage, "/mtl/white", {"diffuseColor": (C, (0, 0, 0)),
                                                                        "emissiveColor": (C, (1, 1, 1))}))
quad(stage, "/black", 0, -4, 4, 4, -3, material(stage, "/mtl/black", {"diffuseColor": (C, (0, 0, 0))}))


def wiggle(**settings):
    """The spread (standard deviation) of the edge's x over the rows in y in [-1, 1]."""
    image = render(stage, frustum, True, **settings)
    xs = []
    for py in range(SIZE // 4, 3 * SIZE // 4):
        y = 2 - (py + 0.5) / SIZE * 4
        row = [pixel(image, px / SIZE * 4 - 2, y)[1] for px in range(SIZE)]
        crossing = next((px for px in range(SIZE // 8, 7 * SIZE // 8) if row[px] < 0.5 * max(row)), None)
        xs.append(crossing / SIZE * 4 - 2 if crossing is not None else 0.0)
    mean = sum(xs) / len(xs)
    return math.sqrt(sum((x - mean) ** 2 for x in xs) / len(xs))


bent, flat, rest = wiggle(), wiggle(**{"eldr:displacement": False}), wiggle(**{"eldr:rtRefractionDisplacement": False})
print(f"edge spread through rippled glass: displaced {bent:.3f}, no displacement {flat:.3f}, rest normals {rest:.3f}")
expect(bent > 0.08, "displaced glass bends the traced refraction (the edge wiggles)")
expect(flat < 0.02 and rest < 0.02, "flat glass leaves the edge straight")

# The same ripple from a MaterialX displacement graph (0.05 sin(2 pi 4 (u + v)),
# run per vertex for the BLAS and at the hit): the edge wiggles as well.
stage, frustum, _ = stage_with_camera()
graphGlass = mtlx_glass(stage, "/mtl/graphRipple", {"specular_roughness": (F, 0.0)}, {})
disp = UsdShade.Shader.Define(stage, "/mtl/graphRipple/disp")
disp.CreateIdAttr("ND_displacement_float")
graphGlass.CreateDisplacementOutput("mtlx").ConnectToSource(disp.ConnectableAPI(), "out")
uvNode = UsdShade.Shader.Define(stage, "/mtl/graphRipple/uv")
uvNode.CreateIdAttr("ND_texcoord_vector2")
parts = []
for i in range(2):
    part = UsdShade.Shader.Define(stage, f"/mtl/graphRipple/c{i}")
    part.CreateIdAttr("ND_extract_vector2")
    part.CreateInput("in", Sdf.ValueTypeNames.Float2).ConnectToSource(uvNode.ConnectableAPI(), "out")
    part.CreateInput("index", Sdf.ValueTypeNames.Int).Set(i)
    parts.append(part)
summed = UsdShade.Shader.Define(stage, "/mtl/graphRipple/sum")
summed.CreateIdAttr("ND_add_float")
summed.CreateInput("in1", F).ConnectToSource(parts[0].ConnectableAPI(), "out")
summed.CreateInput("in2", F).ConnectToSource(parts[1].ConnectableAPI(), "out")
scaled = UsdShade.Shader.Define(stage, "/mtl/graphRipple/scaled")
scaled.CreateIdAttr("ND_multiply_float")
scaled.CreateInput("in1", F).ConnectToSource(summed.ConnectableAPI(), "out")
scaled.CreateInput("in2", F).Set(2 * math.pi * 4)
wave = UsdShade.Shader.Define(stage, "/mtl/graphRipple/sin")
wave.CreateIdAttr("ND_sin_float")
wave.CreateInput("in", F).ConnectToSource(scaled.ConnectableAPI(), "out")
disp.CreateInput("displacement", F).ConnectToSource(wave.ConnectableAPI(), "out")
disp.CreateInput("scale", F).Set(0.05)
sheet = mesh(stage, "/sheet", points, [4] * cells * cells, indices, graphGlass, [(0, 0, 1)] * len(points))
UsdGeom.PrimvarsAPI(sheet).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray,
                                         UsdGeom.Tokens.vertex).Set(uvs)
quad(stage, "/white", -4, -4, 0, 4, -3, material(stage, "/mtl/white", {"diffuseColor": (C, (0, 0, 0)),
                                                                        "emissiveColor": (C, (1, 1, 1))}))
quad(stage, "/black", 0, -4, 4, 4, -3, material(stage, "/mtl/black", {"diffuseColor": (C, (0, 0, 0))}))
bent, rest = wiggle(), wiggle(**{"eldr:displacementGraphs": False})
print(f"edge spread through graph-rippled glass: displaced {bent:.3f}, graphs off {rest:.3f}")
expect(bent > 0.08, "a displacement graph bends the traced refraction (the edge wiggles)")
expect(rest < 0.02, "without the graph the edge stays straight")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
