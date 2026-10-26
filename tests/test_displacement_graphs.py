"""Displacement graphs and mip levels: a MaterialX displacement whose
`displacement` is a node graph (sin of u, run per vertex) moves the plane,
the same in the depth prepass and the shading pass; and a height map whose
texels alternate far finer than the vertices displaces by its mean (a mip
level as coarse as the vertex spacing), not by whichever texel a vertex hits;
and two instances of a world-space graph cast traced shadows of their own
ridges (a displaced BLAS each).
Run: usdenv.cmd python tests\\test_displacement_graphs.py"""
import os
import sys
import tempfile

from PySide6.QtCore import QSize
from PySide6.QtGui import QColor, QImage, QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux, UsdShade, Vt

SIZE = 128

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

# A 1-texel checker of 0 and 1, 256 x 256.
checker = os.path.join(tempfile.mkdtemp(), "checker.png")
image = QImage(256, 256, QImage.Format_RGB32)
for y in range(256):
    for x in range(256):
        v = 255 * ((x + y) % 2)
        image.setPixelColor(x, y, QColor(v, v, v))
image.save(checker)


def plane(stage, path, n):
    """A unit square in XZ facing +Y, n x n quads, with st."""
    mesh = UsdGeom.Mesh.Define(stage, path)
    points, st, counts, indices = [], [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            points.append(Gf.Vec3f(i / n - 0.5, 0, 0.5 - j / n))
            st.append(Gf.Vec2f(i / n, j / n))
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            counts.append(4)
            indices += [a, a + 1, a + n + 2, a + n + 1]
    mesh.CreatePointsAttr(points)
    mesh.CreateFaceVertexCountsAttr(counts)
    mesh.CreateFaceVertexIndicesAttr(indices)
    mesh.CreateSubdivisionSchemeAttr("none")
    mesh.CreateNormalsAttr([Gf.Vec3f(0, 1, 0)] * len(points))
    mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    UsdGeom.PrimvarsAPI(mesh).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray,
                                            UsdGeom.Tokens.vertex).Set(Vt.Vec2fArray(st))
    return mesh


def scene(kind):
    """A plane under a light, displaced by a graph (sin(12 u) * 0.15) or the checker map (scale 0.3)."""
    stage = Usd.Stage.CreateInMemory()
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    # 15 quads: vertices land on texels of both values (not on a 16 grid)
    mesh = plane(stage, "/plane", 64 if kind == "graph" else 15)
    material = UsdShade.Material.Define(stage, "/mtl")
    surface = UsdShade.Shader.Define(stage, "/mtl/surface")
    surface.CreateIdAttr("ND_standard_surface_surfaceshader")
    material.CreateSurfaceOutput("mtlx").ConnectToSource(surface.ConnectableAPI(), "out")
    disp = UsdShade.Shader.Define(stage, "/mtl/disp")
    disp.CreateIdAttr("ND_displacement_float")
    material.CreateDisplacementOutput("mtlx").ConnectToSource(disp.ConnectableAPI(), "out")
    graph = UsdShade.NodeGraph.Define(stage, "/mtl/graph")
    uv = UsdShade.Shader.Define(stage, "/mtl/graph/uv")
    uv.CreateIdAttr("ND_texcoord_vector2")
    if kind == "graph":
        u = UsdShade.Shader.Define(stage, "/mtl/graph/u")
        u.CreateIdAttr("ND_extract_vector2")
        u.CreateInput("in", Sdf.ValueTypeNames.Float2).ConnectToSource(uv.ConnectableAPI(), "out")
        u.CreateInput("index", Sdf.ValueTypeNames.Int).Set(0)
        scaled = UsdShade.Shader.Define(stage, "/mtl/graph/scaled")
        scaled.CreateIdAttr("ND_multiply_float")
        scaled.CreateInput("in1", Sdf.ValueTypeNames.Float).ConnectToSource(u.ConnectableAPI(), "out")
        scaled.CreateInput("in2", Sdf.ValueTypeNames.Float).Set(12.0)
        result = UsdShade.Shader.Define(stage, "/mtl/graph/sin")
        result.CreateIdAttr("ND_sin_float")
        result.CreateInput("in", Sdf.ValueTypeNames.Float).ConnectToSource(scaled.ConnectableAPI(), "out")
        scale = 0.15
    else:
        result = UsdShade.Shader.Define(stage, "/mtl/graph/height")
        result.CreateIdAttr("ND_image_float")
        result.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(checker)
        result.CreateInput("texcoord", Sdf.ValueTypeNames.Float2).ConnectToSource(uv.ConnectableAPI(), "out")
        scale = 0.3
    out = graph.CreateOutput("out", Sdf.ValueTypeNames.Float)
    out.ConnectToSource(result.ConnectableAPI(), "out")
    disp.CreateInput("displacement", Sdf.ValueTypeNames.Float).ConnectToSource(out)
    disp.CreateInput("scale", Sdf.ValueTypeNames.Float).Set(scale)
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
    sun = UsdLux.DistantLight.Define(stage, "/sun")
    sun.AddRotateXYZOp().Set(Gf.Vec3f(-45, 60, 0))
    return stage


def render(stage, **settings):
    """An image from the side and above, noisy features off."""
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    for name, value in dict(taa=False, autoExposure=False, ambientOcclusion=False, ssr=False, **settings).items():
        engine.SetRendererSetting("eldr:" + name, value)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    frustum = Gf.Frustum()
    frustum.SetPerspective(40, 1.0, 0.1, 100)
    frustum.Transform(Gf.Matrix4d().SetLookAt(Gf.Vec3d(0, 1.2, 1.6), Gf.Vec3d(0, 0, 0), Gf.Vec3d(0, 1, 0)).GetInverse())
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    for _ in range(3):  # textures arrive with the first commit
        engine.Render(stage.GetPseudoRoot(), params)
    result = fbo.toImage()
    del engine  # release GL resources while the context is still current
    return result


def difference(a, b):
    """Fraction of pixels whose colour differs by more than 4/255."""
    differing = 0
    for y in range(SIZE):
        for x in range(SIZE):
            ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
            differing += max(abs(ca.red() - cb.red()), abs(ca.green() - cb.green()), abs(ca.blue() - cb.blue())) > 4
    return differing / SIZE**2


def spread(img):
    """Standard deviation of the luminance over the image's middle."""
    values = [img.pixelColor(x, y).lightnessF() for y in range(SIZE // 3, 2 * SIZE // 3)
              for x in range(SIZE // 3, 2 * SIZE // 3)]
    mean = sum(values) / len(values)
    return (sum((v - mean) ** 2 for v in values) / len(values)) ** 0.5


failures = []


def expect(ok, what):
    print(("" if ok else "FAIL: ") + what)
    if not ok:
        failures.append(what)


graph = scene("graph")
on, off = render(graph), render(graph, displacement=False)
changed = difference(on, off)
expect(changed > 0.05, f"a displacement graph moves the plane ({changed:.3f} of pixels differ)")
changed = difference(on, render(graph, depthPrepass=False))
expect(changed < 0.002, f"the depth prepass runs the same graph ({changed:.4f} differ)")
changed = difference(off, render(graph, displacementGraphs=False))
expect(changed < 0.002, f"eldr:displacementGraphs off: no graph displacement ({changed:.4f} differ)")

# The checker on 15 x 15 quads: its mean, 0.5 * 0.3 everywhere, a flat
# plane (even shading), raised (differs from undisplaced).
flat = scene("checker")
on, off = render(flat), render(flat, displacement=False)
expect(difference(on, off) > 0.05, "the checker map displaces the plane")
expect(spread(on) < 1.5 * spread(off) + 0.01,
       f"a map finer than the mesh displaces by its mean (luminance spread {spread(on):.4f}, flat {spread(off):.4f})")

# Two instances of a plane displaced by a world-space graph,
# 0.03 sin(40 x_world): the raster gives each its own ridges. Ray tracing
# gives each instance a displaced BLAS of its own, so in both halves of
# the image the traced shadows match the shadow maps' as closely (a shared
# BLAS would give the second instance the first one's ridges).
def world_scene():
    stage = Usd.Stage.CreateInMemory()
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.Scope.Define(stage, "/proto")
    mesh = plane(stage, "/proto/plane", 96)
    material = UsdShade.Material.Define(stage, "/mtl")
    surface = UsdShade.Shader.Define(stage, "/mtl/surface")
    surface.CreateIdAttr("ND_standard_surface_surfaceshader")
    material.CreateSurfaceOutput("mtlx").ConnectToSource(surface.ConnectableAPI(), "out")
    disp = UsdShade.Shader.Define(stage, "/mtl/disp")
    disp.CreateIdAttr("ND_displacement_float")
    material.CreateDisplacementOutput("mtlx").ConnectToSource(disp.ConnectableAPI(), "out")
    position = UsdShade.Shader.Define(stage, "/mtl/graph/position")
    position.CreateIdAttr("ND_position_vector3")
    position.CreateInput("space", Sdf.ValueTypeNames.String).Set("world")
    x = UsdShade.Shader.Define(stage, "/mtl/graph/x")
    x.CreateIdAttr("ND_extract_vector3")
    x.CreateInput("in", Sdf.ValueTypeNames.Float3).ConnectToSource(position.ConnectableAPI(), "out")
    x.CreateInput("index", Sdf.ValueTypeNames.Int).Set(0)
    scaled = UsdShade.Shader.Define(stage, "/mtl/graph/scaled")
    scaled.CreateIdAttr("ND_multiply_float")
    scaled.CreateInput("in1", Sdf.ValueTypeNames.Float).ConnectToSource(x.ConnectableAPI(), "out")
    scaled.CreateInput("in2", Sdf.ValueTypeNames.Float).Set(40.0)
    wave = UsdShade.Shader.Define(stage, "/mtl/graph/sin")
    wave.CreateIdAttr("ND_sin_float")
    wave.CreateInput("in", Sdf.ValueTypeNames.Float).ConnectToSource(scaled.ConnectableAPI(), "out")
    disp.CreateInput("displacement", Sdf.ValueTypeNames.Float).ConnectToSource(wave.ConnectableAPI(), "out")
    disp.CreateInput("scale", Sdf.ValueTypeNames.Float).Set(0.03)
    UsdShade.MaterialBindingAPI.Apply(mesh.GetPrim()).Bind(material)
    instancer = UsdGeom.PointInstancer.Define(stage, "/pair")
    instancer.CreatePrototypesRel().SetTargets(["/proto/plane"])
    instancer.CreatePositionsAttr(Vt.Vec3fArray([Gf.Vec3f(-0.27, 0, 0), Gf.Vec3f(0.2925, 0, 0)]))
    instancer.CreateScalesAttr(Vt.Vec3fArray([Gf.Vec3f(0.5, 1, 1)] * 2))
    instancer.CreateProtoIndicesAttr([0, 0])
    UsdGeom.Imageable(stage.GetPrimAtPath("/proto")).MakeInvisible()
    sun = UsdLux.DistantLight.Define(stage, "/sun")
    sun.AddRotateXYZOp().Set(Gf.Vec3f(-10, -80, 0))  # low, across the ridges
    return stage


def halves(a, b):
    """Fraction of differing pixels in the left and the right half."""
    out = []
    for x0, x1 in ((0, SIZE // 2), (SIZE // 2, SIZE)):
        differing = 0
        for y in range(SIZE):
            for x in range(x0, x1):
                ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
                differing += max(abs(ca.red() - cb.red()), abs(ca.green() - cb.green()), abs(ca.blue() - cb.blue())) > 4
        out.append(differing / (SIZE * SIZE / 2))
    return out


pair = world_scene()
maps, traced, rest = render(pair), render(pair, rtShadows=True), render(pair, rtShadows=True, rtDisplacement=False)
near, far = halves(traced, maps), halves(rest, maps)
print(f"world-space graph, two instances: traced shadows against the shadow maps, left {near[0]:.4f} "
      f"right {near[1]:.4f}; with rest BLASes {far[0]:.4f}, {far[1]:.4f}")
expect(far[0] > 0.01 and far[1] > 0.01, "the rest surface's traced shadows differ from the maps")
expect(max(near) < 0.5 * min(far), f"each instance's traced shadows follow its own ridges ({near})")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
