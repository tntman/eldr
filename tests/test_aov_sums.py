"""Light-split AOVs against the beauty: direct + indirect, diffuse + glossy, plus
emission, must add up to the linear EXR beauty (eldr:productLinear) for the
layered materials too: coat, sheen, subsurface and transmission, plain and
under GI and the denoised SSR, each to within TOLERANCE; with light path
intensities other than 1, the splits scaled by them.
Run: usdenv.cmd python tests\\test_aov_sums.py <exrinfo>"""
import os
import subprocess
import sys
import tempfile


def run(*args, **kwargs):
    """subprocess.run that fails the test after 10 minutes instead of hanging."""
    kwargs.setdefault("timeout", 600)
    return subprocess.run(*args, **kwargs)


exrinfo = sys.argv[1]
folder = tempfile.mkdtemp(prefix="eldr_sums_")
scenePath = os.path.join(folder, "scene.usda")
exrPath = os.path.join(folder, "sums.exr").replace("\\", "/")
SIZE = 192  # exr_record.py's EXR_SIZE; 3 x 2 spheres on an orthographic 12 x 12 view

MTLX = "ND_standard_surface_surfaceshader"
MATERIALS = {  # name: (shader id, inputs)
    "diffuse": ("UsdPreviewSurface", "color3f inputs:diffuseColor = (0.6, 0.35, 0.25)\n float inputs:roughness = 0.8"),
    "coat": ("UsdPreviewSurface", "color3f inputs:diffuseColor = (0.5, 0.05, 0.05)\n float inputs:roughness = 0.6\n"
             " float inputs:clearcoat = 1\n float inputs:clearcoatRoughness = 0.08"),
    "sheen": (MTLX, "color3f inputs:base_color = (0.25, 0.02, 0.05)\n float inputs:specular = 0.2\n"
              " float inputs:specular_roughness = 0.6\n float inputs:sheen = 1\n"
              " color3f inputs:sheen_color = (1, 0.6, 0.7)\n float inputs:sheen_roughness = 0.3"),
    "subsurface": (MTLX, "color3f inputs:base_color = (0.8, 0.55, 0.45)\n float inputs:subsurface = 1\n"
                   " color3f inputs:subsurface_color = (0.9, 0.6, 0.5)\n color3f inputs:subsurface_radius = (1, 0.35, 0.2)\n"
                   " float inputs:subsurface_scale = 0.12\n float inputs:specular_roughness = 0.45"),
    "transmission": (MTLX, "float inputs:transmission = 1\n color3f inputs:transmission_color = (0.25, 0.85, 0.35)\n"
                     " float inputs:transmission_depth = 1\n float inputs:specular_roughness = 0.1"),
    "emissive": ("UsdPreviewSurface", "color3f inputs:diffuseColor = (0.3, 0.3, 0.3)\n color3f inputs:emissiveColor = (0.4, 0.2, 0.1)"),
}
POSITIONS = {name: (-4 + 4 * (i % 3), 2 - 4 * (i // 3)) for i, name in enumerate(MATERIALS)}  # world x, y

text = ['#usda 1.0\n(\n upAxis = "Y"\n)\n']
text.append('def Camera "cam"\n{\n token projection = "orthographic"\n float horizontalAperture = 120\n'
            ' float verticalAperture = 120\n float horizontalApertureOffset = 0\n float focalLength = 50\n'
            ' double3 xformOp:translate = (0, 0, 20)\n uniform token[] xformOpOrder = ["xformOp:translate"]\n}\n')
text.append('def DistantLight "sun"\n{\n float inputs:intensity = 3\n bool inputs:normalize = 1\n'
            ' float3 xformOp:rotateXYZ = (-35, 25, 0)\n uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]\n}\n')
text.append('def DomeLight "dome"\n{\n float inputs:intensity = 0.5\n color3f inputs:color = (0.6, 0.75, 1)\n}\n')
text.append('def Scope "mtl"\n{\n')
for name, (shader, inputs) in MATERIALS.items():
    mtlx = shader != "UsdPreviewSurface"
    output = "outputs:mtlx:surface" if mtlx else "outputs:surface"
    text.append(f' def Material "{name}"\n {{\n token {output}.connect = </mtl/{name}/surface.outputs:{"out" if mtlx else "surface"}>\n'
                f' def Shader "surface"\n {{\n uniform token info:id = "{shader}"\n {inputs}\n'
                f' token outputs:{"out" if mtlx else "surface"}\n }}\n }}\n')
text.append('}\n')
for name, (x, y) in POSITIONS.items():
    text.append(f'def Sphere "{name}" (prepend apiSchemas = ["MaterialBindingAPI"])\n{{\n double radius = 1.5\n'
                f' rel material:binding = </mtl/{name}>\n double3 xformOp:translate = ({x}, {y}, 0)\n'
                f' uniform token[] xformOpOrder = ["xformOp:translate"]\n}}\n')
# A floor-less wall behind the spheres, so GI and SSR have something to bounce from.
text.append('def Cube "wall"\n{\n double size = 1\n double3 xformOp:translate = (0, 0, -3)\n'
            ' double3 xformOp:scale = (14, 14, 0.2)\n uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n}\n')
text.append('def RenderSettings "Render"\n{\n rel camera = </cam>\n rel products = </Render/product>\n'
            f' def RenderProduct "product"\n {{\n uniform token productName = "{exrPath}"\n rel camera = </cam>\n int2 resolution = ({SIZE}, {SIZE})\n'
            ' rel orderedVars = [' + ", ".join(f"</Render/Vars/{n}>" for n, *_ in [
                ("C",), ("directdiffuse",), ("indirectdiffuse",), ("directglossy",), ("indirectglossy",),
                ("emission",)]) + ']\n }\n def Scope "Vars"\n {\n')
for name, source, datatype in [("C", "C.*[LO]", "color4f"), ("directdiffuse", "C<RD>L", "color3f"),
                               ("indirectdiffuse", "C<RD>.+L", "color3f"), ("directglossy", "C<RG>L", "color3f"),
                               ("indirectglossy", "C<RG>.+L", "color3f"), ("emission", "C.*O", "color3f")]:
    text.append(f' def RenderVar "{name}"\n {{\n uniform string sourceName = "{source}"\n'
                f' uniform token sourceType = "lpe"\n uniform token dataType = "{datatype}"\n }}\n')
text.append(' }\n}\n')
open(scenePath, "w").write("".join(text))

TOLERANCE = 0.03  # film noise at edges
POINTS = [(12, -8), (-20, 0), (0, 18)]  # pixels from a sphere's centre: lit, terminator, below

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def pixel(x, y):
    out = run([exrinfo, exrPath, str(x), str(y)], capture_output=True, text=True, timeout=60).stdout.splitlines()
    return {l.split()[1]: float(l.split()[2]) for l in out if l.startswith("pixel ")}


# Traced refraction (on in final quality) is noisy per sample: the colour
# AOVs are filtered with the film's weights, so it sums as well as the rest.
VARIANTS = {"default": ["eldr:rtRefraction=0"], "gi": ["eldr:gi=true", "eldr:rtRefraction=0"],
            "ssrDenoise": ["eldr:ssr=true", "eldr:ssrDenoise=true", "eldr:rtRefraction=0"],
            "rtRefraction": ["eldr:rtRefraction=2"]}
# Light path intensity: the beauty is the direct splits times the direct
# intensity plus the indirect ones times the indirect intensity, refracted
# and subsurface-transmitted light included.
INTENSITIES = ["eldr:directIntensity=1.6", "eldr:indirectIntensity=0.5"]
VARIANTS.update({"intensity": ["eldr:rtRefraction=0"] + INTENSITIES,
                 "intensityRt": ["eldr:rtRefraction=2"] + INTENSITIES})
SPLITS = ("directdiffuse", "indirectdiffuse", "directglossy", "indirectglossy", "emission")
for variant, settings in VARIANTS.items():
    scale = {"directdiffuse": 1.6, "directglossy": 1.6, "indirectdiffuse": 0.5, "indirectglossy": 0.5,
             "emission": 1.0} if INTENSITIES[0] in settings else dict.fromkeys(SPLITS, 1.0)
    run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "exr_record.py"),
                    scenePath, "/Render", "/cam", "eldr:autoExposure=false"] + settings,
                   check=True, env=dict(os.environ, EXR_SIZE=str(SIZE)))
    for name, (x, y) in POSITIONS.items():
        centre = (int((x + 6) / 12 * SIZE), int((6 - y) / 12 * SIZE))
        worst, brightest = 0.0, 0.0
        for dx, dy in POINTS:
            p = pixel(centre[0] + dx, centre[1] + dy)
            beauty = [p.get(c, 0) for c in "RGB"]
            brightness = max(max(beauty), 1e-3)
            brightest = max(brightest, brightness)
            for i, c in enumerate("RGB"):
                residual = (beauty[i] - sum(scale[v] * p.get(f"{v}.{c}", 0) for v in SPLITS)) / brightness
                worst = residual if abs(residual) > abs(worst) else worst
        print(f"{variant:10s} {name:12s} brightest {brightest:.3f} worst residual {worst:+.3f} ")
        expect(brightest > 0.02, f"{variant} {name}: the sphere is lit")
        if name == "transmission":  # what it refracts (traced or not) is indirect glossy
            p = pixel(centre[0], centre[1])
            share = scale["indirectglossy"] * p.get("indirectglossy.G", 0) / max(p.get("G", 0), 1e-6)
            expect(share > 0.6, f"{variant} transmission: refracted light in indirect glossy ({share:.2f})")
        expect(abs(worst) <= TOLERANCE, f"{variant} {name}: the splits and emission add up to the beauty ({worst:+.3f})")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
