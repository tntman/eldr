"""Displacement (tests/scenes/displacement.usda at Complexity High): the
settings switch it, the depth prepass sees the same displaced positions
as the shading pass, the scale scales it, ray tracing sees it, and a back
face is bumped as its front face.
Run: usdenv.cmd python tests\\test_displacement.py"""
import math
import os
import sys
import tempfile

from PySide6.QtCore import QSize
from PySide6.QtGui import QColor, QImage, QOffscreenSurface, QOpenGLContext
from PySide6.QtOpenGL import QOpenGLFramebufferObject
from PySide6.QtWidgets import QApplication
from pxr import CameraUtil, Gf, Sdf, Usd, UsdGeom, UsdImagingGL, UsdLux, UsdShade

SIZE = 256

app = QApplication(sys.argv)
surface = QOffscreenSurface()
surface.create()
glContext = QOpenGLContext()  # must stay referenced while rendering
glContext.create()
glContext.makeCurrent(surface)
fbo = QOpenGLFramebufferObject(QSize(SIZE, SIZE))
fbo.bind()

stage = Usd.Stage.Open(os.path.join(os.path.dirname(__file__), "scenes", "displacement.usda"))
camera = UsdGeom.Camera(stage.GetPrimAtPath("/cam")).GetCamera(Usd.TimeCode.Default())
CameraUtil.ConformWindow(camera, CameraUtil.Fit, 1.0)
frustum = camera.frustum


def render(**settings):
    """An image with these eldr: settings; noisy features off, so images compare."""
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    for name, value in dict(taa=False, autoExposure=False, ambientOcclusion=False, ssr=False, **settings).items():
        engine.SetRendererSetting("eldr:" + name, value)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix())
    params = UsdImagingGL.RenderParams()
    params.complexity = 1.2
    for _ in range(2):  # textures arrive with the first commit
        engine.Render(stage.GetPseudoRoot(), params)
    image = fbo.toImage()
    global rayTracing
    rayTracing = engine.GetRenderStats().get("rayTracingSupported", False)
    del engine  # release GL resources while the context is still current
    return image


rayTracing = False


def difference(a, b):
    """Fraction of pixels whose colour differs by more than 4/255, and the largest difference."""
    differing, largest = 0, 0.0
    for y in range(SIZE):
        for x in range(SIZE):
            ca, cb = a.pixelColor(x, y), b.pixelColor(x, y)
            d = max(abs(ca.redF() - cb.redF()), abs(ca.greenF() - cb.greenF()), abs(ca.blueF() - cb.blueF()))
            differing += d > 4 / 255
            largest = max(largest, d)
    return differing / SIZE**2, largest


failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


on, off = render(), render(displacement=False)
changed, _ = difference(on, off)
expect(changed > 0.02, f"displacement changes the image ({changed:.3f} of pixels)")
# Without a prepass the shading pass writes its own depth: a prepass that
# saw other positions would leave holes or z-fighting in the other image.
changed, largest = difference(on, render(depthPrepass=False))
expect(changed < 0.002, f"the depth prepass sees the shading pass's positions ({changed:.4f} differ, {largest:.3f})")
changed, _ = difference(off, render(displacementScale=0.0))
expect(changed < 0.002, f"scale 0 is no displacement ({changed:.4f} differ)")
changed, _ = difference(on, render(displacementScale=2.0))
expect(changed > 0.02, f"the scale scales it ({changed:.3f} differ)")
# Traced shadows see the displaced surfaces (their bumps shadow each other)
# with rtDisplacement; without displacement it changes nothing.
if rayTracing:
    traced = render(rtShadows=True)
    changed, _ = difference(traced, render(rtShadows=True, rtDisplacement=False))
    print(f"traced shadows with and without rtDisplacement: {changed:.4f} of pixels differ")
    expect(changed > 0.003, f"traced shadows see displacement ({changed:.4f} differ)")
    changed, _ = difference(render(rtShadows=True, displacement=False),
                            render(rtShadows=True, displacement=False, rtDisplacement=False))
    expect(changed < 0.002, f"rtDisplacement without displacement changes nothing ({changed:.4f} differ)")
else:
    print("no ray tracing on this GPU: its tests skipped")

# A back face's bump: the same rippled surface z = -0.1 t(v), once a front
# face (normals +z towards the camera, offsets -0.1 t) and once a back face
# (normals -z, offsets +0.1 t along them), lit at a slant. The shading
# normals, and so the images, must match; a bump with the front face's sign
# on the back face tilts the ripples' normals the other way.
textures = tempfile.mkdtemp()
ripple = QImage(64, 64, QImage.Format_Grayscale8)
for py in range(64):
    level = 0.5 + 0.5 * math.sin(2 * math.pi * 3 * (1 - (py + 0.5) / 64))
    for px in range(64):
        ripple.setPixel(px, py, QColor.fromRgbF(level, level, level).rgb())
ripple.save(os.path.join(textures, "ripple.png"))


def sheet(nz, scale):
    s = Usd.Stage.CreateInMemory()
    cam = UsdGeom.Camera.Define(s, "/cam")
    cam.CreateProjectionAttr("orthographic")
    cam.CreateHorizontalApertureAttr(40.0)
    cam.CreateVerticalApertureAttr(40.0)
    cam.AddTranslateOp().Set(Gf.Vec3d(0, 0, 5))
    light = UsdLux.DistantLight.Define(s, "/sun")
    light.CreateIntensityAttr(2.0)
    light.CreateNormalizeAttr(True)
    light.AddRotateXOp().Set(50.0)  # from above the camera, slanted along y
    mat = UsdShade.Material.Define(s, "/mtl")
    pbr = UsdShade.Shader.Define(s, "/mtl/pbr")
    pbr.CreateIdAttr("UsdPreviewSurface")
    pbr.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set((0.8, 0.8, 0.8))
    pbr.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.6)
    tex = UsdShade.Shader.Define(s, "/mtl/height")
    tex.CreateIdAttr("UsdUVTexture")
    tex.CreateInput("file", Sdf.ValueTypeNames.Asset).Set(os.path.join(textures, "ripple.png"))
    tex.CreateInput("sourceColorSpace", Sdf.ValueTypeNames.Token).Set("raw")
    tex.CreateInput("scale", Sdf.ValueTypeNames.Float4).Set((scale, scale, scale, 1))
    st = UsdShade.Shader.Define(s, "/mtl/st")
    st.CreateIdAttr("UsdPrimvarReader_float2")
    st.CreateInput("varname", Sdf.ValueTypeNames.Token).Set("st")
    tex.CreateInput("st", Sdf.ValueTypeNames.Float2).ConnectToSource(st.CreateOutput("result", Sdf.ValueTypeNames.Float2))
    pbr.CreateInput("displacement", Sdf.ValueTypeNames.Float).ConnectToSource(
        tex.CreateOutput("r", Sdf.ValueTypeNames.Float))
    mat.CreateSurfaceOutput().ConnectToSource(pbr.CreateOutput("surface", Sdf.ValueTypeNames.Token))
    mat.CreateDisplacementOutput().ConnectToSource(pbr.CreateOutput("displacement", Sdf.ValueTypeNames.Token))
    cells = 64
    points, uvs, indices = [], [], []
    for j in range(cells + 1):
        for i in range(cells + 1):
            points.append((4 * i / cells - 2, 4 * j / cells - 2, 0))
            uvs.append((i / cells, j / cells))
    for j in range(cells):
        for i in range(cells):
            a = j * (cells + 1) + i
            indices += [a, a + 1, a + cells + 2, a + cells + 1]
    m = UsdGeom.Mesh.Define(s, "/sheet")
    m.CreatePointsAttr(points)
    m.CreateFaceVertexCountsAttr([4] * cells * cells)
    m.CreateFaceVertexIndicesAttr(indices)
    m.CreateSubdivisionSchemeAttr("none")
    m.CreateNormalsAttr([(0, 0, nz)] * len(points))
    m.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    UsdGeom.PrimvarsAPI(m).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray, UsdGeom.Tokens.vertex).Set(uvs)
    UsdShade.MaterialBindingAPI.Apply(m.GetPrim()).Bind(mat)
    return s, cam.GetCamera(Usd.TimeCode.Default()).frustum


def render_sheet(nz, scale, **settings):
    s, f = sheet(nz, scale)
    engine = UsdImagingGL.Engine()
    assert engine.SetRendererPlugin("HdVkRendererPlugin"), "HdVk not available"
    for name, value in dict(taa=False, autoExposure=False, ambientOcclusion=False, ssr=False, **settings).items():
        engine.SetRendererSetting("eldr:" + name, value)
    engine.SetRenderViewport(Gf.Vec4d(0, 0, SIZE, SIZE))
    engine.SetCameraState(f.ComputeViewMatrix(), f.ComputeProjectionMatrix())
    for _ in range(2):
        engine.Render(s.GetPseudoRoot(), UsdImagingGL.RenderParams())
    image = fbo.toImage()
    del engine
    return image


front, back = render_sheet(1, -0.1), render_sheet(-1, 0.1)
bumped, largest = difference(front, render_sheet(1, -0.1, displacement=False))
changed, worst = difference(front, back)
print(f"back-face bump: {changed:.4f} of pixels differ from the front face's (largest {worst:.3f}); "
      f"the bump changes {bumped:.3f} (largest {largest:.3f})")
expect(bumped > 0.2, f"the ripple's bump shades the sheet ({bumped:.3f} differ)")
expect(changed < 0.01, f"a back face bumps as its front face ({changed:.4f} differ, largest {worst:.3f})")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
