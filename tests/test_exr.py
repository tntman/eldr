"""Render products: usdrecord --renderSettingsPrimPath renders a stage whose
UsdRenderSettings' product lists render vars by Karma's names, and Eldr
writes them into one EXR: beauty, depth, N, P, diffuse and glossy light
splits, emission and Cryptomatte with its manifest metadata.
Run: usdenv.cmd python tests\\test_exr.py <exrinfo>"""
import json
import os
import struct
import subprocess
import sys
import tempfile

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdRender, UsdShade

exrinfo = sys.argv[1]
folder = tempfile.mkdtemp(prefix="eldr_exr_")
scenePath = os.path.join(folder, "scene.usda")
exrPath = os.path.join(folder, "products.exr").replace("\\", "/")

stage = Usd.Stage.CreateNew(scenePath)
UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
cube = UsdGeom.Cube.Define(stage, "/cube")
look = UsdShade.Material.Define(stage, "/look")
shader = UsdShade.Shader.Define(stage, "/look/pbr")
shader.CreateIdAttr("UsdPreviewSurface")
shader.CreateInput("diffuseColor", Sdf.ValueTypeNames.Color3f).Set(Gf.Vec3f(0.5, 0.5, 0.5))
shader.CreateInput("roughness", Sdf.ValueTypeNames.Float).Set(0.3)
look.CreateSurfaceOutput().ConnectToSource(shader.ConnectableAPI(), "surface")
UsdShade.MaterialBindingAPI.Apply(cube.GetPrim()).Bind(look)
sun = UsdLux.DistantLight.Define(stage, "/sun")
sun.CreateIntensityAttr(3.0)
UsdLux.DomeLight.Define(stage, "/sky").CreateIntensityAttr(0.5)
camera = UsdGeom.Camera.Define(stage, "/cam")
camera.AddTranslateOp().Set(Gf.Vec3d(0, 0, 6))

settings = UsdRender.Settings.Define(stage, "/Render/settings")
product = UsdRender.Product.Define(stage, "/Render/product")
product.CreateProductNameAttr(exrPath)
product.CreateCameraRel().SetTargets(["/cam"])
settings.CreateProductsRel().SetTargets([product.GetPath()])
settings.CreateCameraRel().SetTargets(["/cam"])
VARS = [("C", "lpe", "C.*[LO]", "color4f"), ("depth", "raw", "ray:hitPz", "float"),
        ("N", "raw", "ray:hitN", "normal3f"), ("P", "raw", "ray:hitP", "point3f"),
        ("combineddiffuse", "lpe", "C<RD>.*L", "color3f"), ("directdiffuse", "lpe", "C<RD>L", "color3f"),
        ("indirectdiffuse", "lpe", "C<RD>.+L", "color3f"), ("combinedglossyreflection", "lpe", "C<RG>.*[LO]", "color3f"),
        ("directglossy", "lpe", "C<RG>L", "color3f"), ("indirectglossy", "lpe", "C<RG>.+L", "color3f"),
        ("combinedemission", "lpe", "C.*O", "color3f"),
        ("CryptoObject", "primvar", "__name", "float3"), ("CryptoMaterial", "primvar", "__materialname", "float3")]
for name, sourceType, sourceName, dataType in VARS:
    var = UsdRender.Var.Define(stage, "/Render/Vars/" + name)
    var.CreateSourceTypeAttr(sourceType)
    var.CreateSourceNameAttr(sourceName)
    var.CreateDataTypeAttr(dataType)
    product.CreateOrderedVarsRel().AddTarget(var.GetPath())
stage.GetRootLayer().Save()

subprocess.run(["usdrecord", "--renderer", "Eldr", "--camera", "/cam", "--renderSettingsPrimPath", "/Render/settings",
                "-w", "64", scenePath, os.path.join(folder, "unused.png")], check=True, shell=True)

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


expect(os.path.exists(exrPath), "the product's EXR is written")
info = subprocess.run([exrinfo, exrPath, "32", "32"], capture_output=True, text=True).stdout.splitlines()
channels = [l.split(" ", 1)[1] for l in info if l.startswith("channel ")]
meta = dict(l[5:].split("=", 1) for l in info if l.startswith("meta "))
pixel = {l.split()[1]: float(l.split()[2]) for l in info if l.startswith("pixel ")}
for c in ("R", "G", "B", "A", "depth.Z", "N.B", "P.R", "combineddiffuse.G", "directdiffuse.G", "indirectdiffuse.G",
          "combinedglossyreflection.G", "combinedemission.R", "CryptoObject00.R", "CryptoMaterial00.R"):
    expect(c in channels, "channel " + c)


def Murmur3(name):
    data, h, n = name.encode(), 0, len(name.encode())
    rotl = lambda x, r: ((x << r) | (x >> (32 - r))) & 0xFFFFFFFF
    mix = lambda k: (rotl((k * 0xCC9E2D51) & 0xFFFFFFFF, 15) * 0x1B873593) & 0xFFFFFFFF
    for i in range(0, n // 4 * 4, 4):
        h = (rotl(h ^ mix(int.from_bytes(data[i:i + 4], "little")), 13) * 5 + 0xE6546B64) & 0xFFFFFFFF
    if n % 4:
        h ^= mix(int.from_bytes(data[n // 4 * 4:], "little"))
    h ^= n
    h = ((h ^ (h >> 16)) * 0x85EBCA6B) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 0xC2B2AE35) & 0xFFFFFFFF
    h ^= h >> 16
    return h ^ (1 << 23) if (h >> 23) & 0xFF in (0, 255) else h


key = "%08x" % Murmur3("CryptoObject")
expect(meta.get("cryptomatte/%s/name" % key[:7]) == "CryptoObject" and
       meta.get("cryptomatte/%s/hash" % key[:7]) == "MurmurHash3_32" and
       meta.get("cryptomatte/%s/conversion" % key[:7]) == "uint32_to_float32", "Cryptomatte metadata")
manifest = json.loads(meta.get("cryptomatte/%s/manifest" % key[:7], "{}"))
cubeId = Murmur3("/cube")
expect(manifest.get("/cube") == "%08x" % cubeId, "Cryptomatte manifest names the cube")
f32 = lambda x: struct.unpack("<f", struct.pack("<f", x))[0]  # exrinfo prints float32 exactly (9 digits)
expect(struct.unpack("<f", struct.pack("<I", cubeId))[0] == f32(pixel.get("CryptoObject00.R", 0)) and
       pixel.get("CryptoObject00.G") == 1.0, "the cube's id at full coverage in the image's centre")
expect(abs(pixel.get("N.B", 0) - 1.0) < 0.01, "N: the cube's front face (+z)")
expect(abs(pixel.get("depth.Z", 0) - 5.0) < 0.05, "depth: the front face is 5 units away")
# Karma's convention: the sun's and the dome's light is direct; on the
# unoccluded front face nothing is left for indirect (no GI by default).
direct, indirect = pixel.get("directdiffuse.G", 0), pixel.get("indirectdiffuse.G", 0)
expect(abs(direct + indirect - pixel.get("combineddiffuse.G", 0)) < 1e-3, "diffuse splits: direct + indirect = combined")
expect(direct > 0 and abs(indirect) < 0.02 * direct, "diffuse splits: the dome's light is direct")
expect(abs(pixel.get("directglossy.G", 0) + pixel.get("indirectglossy.G", 0) -
           pixel.get("combinedglossyreflection.G", 0)) < 1e-3 and pixel.get("directglossy.G", 0) > 0,
       "glossy splits: direct + indirect = combined")


# Products at their own resolution (hdvk:productResolution): usdrecord's
# -w 64 buffers, a 96 x 48 product, the camera 20 units from the cube's
# front face. Its 20.955 x 15.29 aperture expands to 2:1, so the face
# (2 units, 50 mm: 5 mm) is 5 / 15.29 * 48 = 15.7 pixels across both ways.
def record(name):
    path = os.path.join(folder, name + ".exr").replace("\\", "/")
    product.GetProductNameAttr().Set(path)
    stage.GetRootLayer().Save()
    subprocess.run(["usdrecord", "--renderer", "Eldr", "--camera", "/cam", "--renderSettingsPrimPath",
                    "/Render/settings", "-w", "64", scenePath, os.path.join(folder, "unused.png")], check=True,
                   shell=True)
    return path


def probe(path, x, y):
    """(size, window line, pixel) of `path` at (x, y) of its data window."""
    out = subprocess.run([exrinfo, path, str(x), str(y)], capture_output=True, text=True).stdout.splitlines()
    size = next((tuple(map(int, l.split()[1:])) for l in out if l.startswith("size ")), None)
    window = next((l.split()[1:] for l in out if l.startswith("window ")), None)
    return size, window, {l.split()[1]: float(l.split()[2]) for l in out if l.startswith("pixel ")}


def face(path, x, y):
    return abs(probe(path, x, y)[2].get("N.B", 0) - 1.0) < 0.01


UsdGeom.Xformable(camera).GetOrderedXformOps()[0].Set(Gf.Vec3d(0, 0, 21))
product.CreateResolutionAttr(Gf.Vec2i(96, 48))
wide = record("wide")
size, window, _ = probe(wide, 48, 24)
expect(size == (96, 48) and window[:4] == ["0", "0", "96", "48"], "the product's resolution: %s %s" % (size, window))
expect(face(wide, 48, 24) and face(wide, 48 + 4, 24) and face(wide, 48, 24 + 4) and not face(wide, 48 + 11, 24) and
       not face(wide, 48, 24 + 11), "the product's aspect: the cube's face is square")
# Pixels twice as wide as high: the image is 4:1, the face 7.8 pixels wide.
product.CreatePixelAspectRatioAttr(2.0)
squeezed = record("squeezed")
size, window, _ = probe(squeezed, 48, 24)
expect(size == (96, 48) and window and abs(float(window[4]) - 2.0) < 1e-6, "pixel aspect ratio 2: %s" % window)
expect(face(squeezed, 48 + 2, 24) and not face(squeezed, 48 + 6, 24) and face(squeezed, 48, 24 + 4),
       "pixel aspect ratio 2: the face is half as many pixels wide as high")
product.CreatePixelAspectRatioAttr(1.0)
# The data window's top right quarter: 48 x 24 pixels from (48, 0) of the
# 96 x 48 image, its bottom left corner at the face's centre.
product.CreateDataWindowNDCAttr(Gf.Vec4f(0.5, 0.5, 1.0, 1.0))
cropped = record("cropped")
size, window, _ = probe(cropped, 0, 0)
expect(size == (48, 24) and window[:4] == ["48", "0", "96", "48"], "the data window: %s %s" % (size, window))
expect(face(cropped, 2, 21) and not face(cropped, 12, 21) and not face(cropped, 2, 12),
       "the data window shows the image's top right quarter")
product.CreateDataWindowNDCAttr(Gf.Vec4f(0, 0, 1, 1))
# hdvk:productResolution off: the frame's size (exr_record.py's 64 x 64).
off = os.path.join(folder, "off.exr").replace("\\", "/")
product.GetProductNameAttr().Set(off)
stage.GetRootLayer().Save()
subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "exr_record.py"),
                scenePath, "/Render/settings", "/cam", "hdvk:productResolution=false"], check=True)
expect(probe(off, 0, 0)[0] == (64, 64), "hdvk:productResolution off: the frame's size")
UsdGeom.Xformable(camera).GetOrderedXformOps()[0].Set(Gf.Vec3d(0, 0, 6))
product.GetResolutionAttr().Clear()



# The beauty is linear and untonemapped by default (hdvk:productLinear): the
# same under ACES, AgX and "none". Off, it is the displayed, tonemapped one.
def beauty(tonemapper, linear):
    path = os.path.join(folder, "beauty_%s_%s.exr" % (tonemapper, linear)).replace("\\", "/")
    product.GetProductNameAttr().Set(path)
    stage.GetRootLayer().Save()
    subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "exr_record.py"),
                    scenePath, "/Render/settings", "/cam", "hdvk:tonemapper=" + tonemapper,
                    "hdvk:productLinear=" + linear], check=True)
    out = subprocess.run([exrinfo, path, "32", "32"], capture_output=True, text=True).stdout.splitlines()
    return {l.split()[1]: float(l.split()[2]) for l in out if l.startswith("pixel ")}


aces, agx, none = (beauty(t, "true") for t in ("aces", "agx", "none"))
expect(aces and all(abs(aces[c] - none[c]) < 1e-3 * max(1.0, abs(none[c])) and
                    abs(agx[c] - none[c]) < 1e-3 * max(1.0, abs(none[c])) for c in "RGB"),
       "linear beauty: the same under aces, agx and none")
expect(none.get("G", 0) > 0.05, "linear beauty: lit")
tonemapped = beauty("agx", "false")
expect(abs(tonemapped["G"] - none["G"]) > 0.02, "hdvk:productLinear off: the tonemapped beauty")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
