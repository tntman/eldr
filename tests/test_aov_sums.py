"""Light-split AOVs against the beauty: direct + indirect diffuse and glossy,
transmission, subsurface, volume and emission must add up to the linear EXR
beauty (eldr:productLinear) for the layered materials too: coat, sheen,
subsurface and transmission, plain and under GI and the denoised SSR, each to
within TOLERANCE; with light path intensities other than 1, the splits scaled
by them. Karma's other splits: the light groups (C.*<L.'tag'>, the sun and
the dome tagged) are the direct light, so with the indirect splits they add
up to the beauty too; the coat is part of glossy, the subsurface its own;
the shadow AOV holds the sun's shadow on the wall, and on a holdout wall
(eldr:object:holdoutmode) the holdout shadow does instead.
Per-AOV sample filters: splits that ask for the beauty's filter by name
(driver:parameters:aov:eldr:filter "beauty", Karma's ["ubox",{}]) still add
up; one that asks for another warns; on the depth AOVs at a sphere's
silhouette min and closest take the sphere, max and farthest the wall,
nearest one sample, box a blend.
Run: usdenv.cmd python tests\\test_aov_sums.py <exrinfo>"""
import os
import subprocess
import sys
import tempfile

import exr_info


def run(*args, **kwargs):
    """subprocess.run that fails the test after 10 minutes instead of hanging."""
    kwargs.setdefault("timeout", 600)
    return subprocess.run(*args, **kwargs)


exrinfo = sys.argv[1]
folder = tempfile.mkdtemp(prefix="eldr_sums_")
scenePath = os.path.join(folder, "scene.usda")
exrPath = os.path.join(folder, "sums.exr").replace("\\", "/")
VDB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "volume.vdb").replace("\\", "/")
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
VARS = [("C", "C.*[LO]", "color4f"), ("directdiffuse", "C<RD>L", "color3f"),
        ("indirectdiffuse", "C<RD>.+L", "color3f"), ("directglossy", "C<RG>L", "color3f"),
        ("indirectglossy", "C<RG>.+L", "color3f"), ("emission", "C.*O", "color3f"),
        ("transmission", "C<TG>.*L", "color3f"), ("sss", "C<TD>.*L", "color3f"),
        ("volume", "CV.*L", "color3f"), ("coat", "C<...'coat'>.*L", "color3f"),
        ("shadow", "shadow;C.*[LO]", "color3f"), ("holdoutshadow", "holdouts;shadow;CDL", "color3f"),
        ("key", "C.*<L.'key'>", "color3f"), ("env", "C.*<L.'env'>", "color3f")]
# A cloud below the spheres, in front of the wall: its light and what it
# hides of the wall's.
CLOUD = (0, -4.8)


def write_scene(holdoutWall, filters=None, extraVars=()):
    """filters: var name: (namespace, value) of its driver:parameters:aov:<ns>:filter."""
    filters = filters or {}
    allVars = VARS + list(extraVars)
    text = ['#usda 1.0\n(\n upAxis = "Y"\n)\n']
    text.append('def Camera "cam"\n{\n token projection = "orthographic"\n float horizontalAperture = 120\n'
                ' float verticalAperture = 120\n float horizontalApertureOffset = 0\n float focalLength = 50\n'
                ' double3 xformOp:translate = (0, 0, 20)\n uniform token[] xformOpOrder = ["xformOp:translate"]\n}\n')
    text.append('def DistantLight "sun"\n{\n float inputs:intensity = 3\n bool inputs:normalize = 1\n'
                ' string inputs:karma:light:lpetag = "key"\n'
                ' float3 xformOp:rotateXYZ = (-35, 25, 0)\n uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]\n}\n')
    # The dome's tag through Eldr's own attribute (eldr: over karma:).
    text.append('def DomeLight "dome"\n{\n float inputs:intensity = 0.5\n color3f inputs:color = (0.6, 0.75, 1)\n'
                ' string inputs:karma:light:lpetag = "wrong"\n string inputs:eldr:light:lpetag = "env"\n}\n')
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
    holdout = ' int primvars:eldr:object:holdoutmode = 1\n' if holdoutWall else ''
    text.append(f'def Cube "wall"\n{{\n double size = 1\n{holdout} double3 xformOp:translate = (0, 0, -3)\n'
                ' double3 xformOp:scale = (14, 14, 0.2)\n uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n}\n')
    text.append(f'def Volume "cloud"\n{{\n rel field:density = </cloud/density>\n double3 xformOp:translate = ({CLOUD[0]}, {CLOUD[1]}, 1)\n'
                ' float3 xformOp:scale = (0.5, 0.5, 0.5)\n uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:scale"]\n'
                f' def OpenVDBAsset "density"\n {{\n asset filePath = @{VDB}@\n token fieldName = "density"\n'
                ' int fieldIndex = 0\n }\n}\n')
    text.append('def RenderSettings "Render"\n{\n rel camera = </cam>\n rel products = </Render/product>\n'
                f' def RenderProduct "product"\n {{\n uniform token productName = "{exrPath}"\n rel camera = </cam>\n int2 resolution = ({SIZE}, {SIZE})\n'
                ' rel orderedVars = [' + ", ".join(f"</Render/Vars/{n}>" for n, *_ in allVars) + ']\n }\n def Scope "Vars"\n {\n')
    for name, source, datatype in allVars:
        kind = "lpe" if (name, source, datatype) in VARS else "raw"
        f = filters.get(name)
        f = f' string driver:parameters:aov:{f[0]}:filter = "{f[1].replace(chr(34), chr(92) + chr(34))}"\n' if f else ""
        text.append(f' def RenderVar "{name}"\n {{\n uniform string sourceName = "{source}"\n'
                    f' uniform token sourceType = "{kind}"\n uniform token dataType = "{datatype}"\n{f} }}\n')
    text.append(' }\n}\n')
    open(scenePath, "w").write("".join(text))


TOLERANCE = 0.03  # film noise at edges
POINTS = [(12, -8), (-20, 0), (0, 18)]  # pixels from a sphere's centre: lit, terminator, below
# The wall in the diffuse sphere's sun shadow (the sun's direction from its
# rotation), clear of the other spheres.
SHADOWED = (int((-5.5 + 6) / 12 * SIZE), int((6 + 0.15) / 12 * SIZE))

failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def pixel(x, y):
    return exr_info.pixel(exrinfo, exrPath, x, y)


def render(settings):
    """Renders the scene; returns what the run printed (stderr: Eldr's warnings)."""
    if os.path.exists(exrPath):
        os.remove(exrPath)  # a variant that writes nothing fails, not reads the last one's
    r = run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "exr_record.py"),
             scenePath, "/Render", "/cam", "eldr:autoExposure=false"] + settings,
            capture_output=True, text=True, env=dict(os.environ, EXR_SIZE=str(SIZE)))
    sys.stdout.write(r.stdout)
    sys.stderr.write(r.stderr)
    r.check_returncode()
    return r.stdout + r.stderr


def rgb(p, var):
    return [p.get(f"{var}.{c}", 0) for c in "RGB"]


# Traced refraction (on in final quality) is noisy per sample: the colour
# AOVs are filtered with the film's weights, so it sums as well as the rest.
VARIANTS = {"default": ["eldr:rtRefraction=0"], "gi": ["eldr:gi=true", "eldr:rtRefraction=0"],
            "ssrDenoise": ["eldr:ssr=true", "eldr:ssrDenoise=true", "eldr:rtRefraction=0"],
            "rtRefraction": ["eldr:rtRefraction=2"]}
# Light path intensity: the beauty is the direct splits times the direct
# intensity plus the indirect ones times the indirect intensity, refracted
# and subsurface-transmitted light included. The splits themselves stay
# the unscaled light, so at the spheres' centres and at the sample points
# (the rims reflect their neighbours by SSR) they match the run without
# intensities (BASE): the light glass refracts and SSR reflects (both from
# a colour the intensities scaled) is scaled once, as indirect glossy.
INTENSITIES = ["eldr:directIntensity=1.6", "eldr:indirectIntensity=0.5"]
VARIANTS.update({"intensity": ["eldr:rtRefraction=0"] + INTENSITIES,
                 "intensityRt": ["eldr:rtRefraction=2"] + INTENSITIES,
                 "intensityDenoise": VARIANTS["ssrDenoise"] + INTENSITIES})
# Through fog (its light isn't in the splits, so only the splits compare):
# what refraction copies and SSR reflects is fogged, its splits aren't.
FOG = ["eldr:rtRefraction=0", "eldr:fog=true", "eldr:fogDensity=0.05"]
VARIANTS.update({"fog": FOG, "intensityFog": FOG + INTENSITIES})
BASE = {"intensity": "default", "intensityRt": "rtRefraction", "intensityDenoise": "ssrDenoise",
        "intensityFog": "fog"}
# The splits asking for the beauty's filter by name, Eldr's and Karma's way.
VARIANTS["beautyFilter"] = ["eldr:rtRefraction=0"]
UBOX = ("karma", '["ubox",{}]')
FILTERS = {"beautyFilter": {"directdiffuse": ("eldr", "beauty"), "indirectdiffuse": UBOX, "directglossy": ("eldr", "beauty"),
                            "indirectglossy": UBOX, "transmission": ("eldr", "beauty"), "emission": UBOX,
                            "volume": ("eldr", "beauty"), "sss": UBOX, "key": ("eldr", "beauty"), "env": UBOX}}
pixels = {}  # (variant, sphere, dx, dy): the EXR's channels there
# The beauty's splits; the subsurface holds direct and indirect light, so
# under intensities its scale is somewhere between the two.
SPLITS = ("directdiffuse", "indirectdiffuse", "directglossy", "indirectglossy", "transmission", "emission",
          "volume", "sss")
GROUPED = ("key", "env", "indirectdiffuse", "indirectglossy", "transmission", "emission", "volume")
for variant, settings in VARIANTS.items():
    write_scene(False, FILTERS.get(variant))
    scaled = INTENSITIES[0] in settings
    scale = {"directdiffuse": 1.6, "directglossy": 1.6, "indirectdiffuse": 0.5, "indirectglossy": 0.5,
             "transmission": 0.5, "emission": 1.0, "volume": 1.0} if scaled else dict.fromkeys(SPLITS, 1.0)
    render(settings)
    fogged = FOG[1] in settings
    for name, (x, y) in POSITIONS.items():
        centre = (int((x + 6) / 12 * SIZE), int((6 - y) / 12 * SIZE))
        worst, brightest, worstGroups = 0.0, 0.0, 0.0
        for dx, dy in POINTS:
            p = pixels[variant, name, dx, dy] = pixel(centre[0] + dx, centre[1] + dy)
            beauty = rgb(p, "C") if "C.R" in p else [p.get(c, 0) for c in "RGB"]
            brightness = max(max(beauty), 1e-3)
            brightest = max(brightest, brightness)
            for i, c in enumerate("RGB"):
                rest = beauty[i] - sum(scale[v] * p.get(f"{v}.{c}", 0) for v in SPLITS if v != "sss")
                sss = p.get(f"sss.{c}", 0)
                # Scaled: anywhere from all indirect (0.5) to all direct (1.6).
                lo, hi = (0.5 * sss, 1.6 * sss) if scaled else (sss, sss)
                residual = (rest - hi if rest > hi else rest - lo if rest < lo else 0.0) / brightness
                worst = residual if abs(residual) > abs(worst) else worst
                if not scaled and name != "subsurface":
                    g = (beauty[i] - sum(p.get(f"{v}.{c}", 0) for v in GROUPED)) / brightness
                    worstGroups = g if abs(g) > abs(worstGroups) else worstGroups
        print(f"{variant:10s} {name:12s} brightest {brightest:.3f} worst residual {worst:+.3f} groups {worstGroups:+.3f}")
        expect(brightest > 0.02, f"{variant} {name}: the sphere is lit")
        p = pixels[variant, name, 0, 0] = pixel(centre[0], centre[1])
        if name == "transmission" and not fogged:  # what it refracts (traced or not) is transmission
            share = scale["transmission"] * p.get("transmission.G", 0) / max(p.get("G", p.get("C.G", 0)), 1e-6)
            expect(share > 0.6, f"{variant} transmission: refracted light in transmission ({share:.2f})")
        expect(fogged or abs(worst) <= TOLERANCE,
               f"{variant} {name}: the splits and emission add up to the beauty ({worst:+.3f})")
        expect(fogged or abs(worstGroups) <= TOLERANCE,
               f"{variant} {name}: the light groups and indirect splits add up to the beauty ({worstGroups:+.3f})")
        if variant == "default":
            G = lambda v: p.get(f"{v}.G", 0)
            if name == "coat":
                expect(G("coat") > 0.01 and G("coat") <= G("directglossy") + G("indirectglossy") + 0.01,
                       f"coat: the coat ({G('coat'):.3f}) is in glossy")
            if name == "diffuse":
                expect(max(rgb(p, "coat")) < 1e-3 and max(rgb(p, "sss")) < 1e-3 and max(rgb(p, "transmission")) < 1e-3,
                       "diffuse: no coat, subsurface or transmission")
            if name == "subsurface":
                expect(G("sss") > 0.5 * (G("sss") + G("directdiffuse") + G("indirectdiffuse")),
                       f"subsurface: its diffuse light is the subsurface's ({G('sss'):.3f})")
        if variant in BASE:
            moved, where = 0.0, ""
            for dx, dy in [(0, 0)] + POINTS:
                p, q = pixels[variant, name, dx, dy], pixels[BASE[variant], name, dx, dy]
                brightness = max(max(q.get(c, 0) for c in "RGB"), 1e-3)
                for v in SPLITS:
                    for c in "RGB":
                        d = (p.get(f"{v}.{c}", 0) - q.get(f"{v}.{c}", 0)) / brightness
                        if abs(d) > abs(moved):
                            moved, where = d, f"{v}.{c} at {dx}, {dy}"
            print(f"{variant:10s} {name:12s} splits against {BASE[variant]}: {moved:+.3f} ({where})")
            expect(abs(moved) <= TOLERANCE, f"{variant} {name}: the intensities leave the splits unscaled ({moved:+.3f})")
    if variant == "default":
        # The cloud: its light, and the wall seen through it, still add up.
        c = pixel(int((CLOUD[0] + 6) / 12 * SIZE), int((6 - CLOUD[1]) / 12 * SIZE))
        beauty = [c.get(ch, 0) for ch in "RGB"]
        residual = max(abs(beauty[i] - sum(c.get(f"{v}.{ch}", 0) for v in SPLITS)) for i, ch in enumerate("RGB"))
        print(f"cloud: volume {c.get('volume.G', 0):.3f} beauty {beauty[1]:.3f} residual {residual:+.3f}")
        expect(c.get("volume.G", 0) > 0.02, "cloud: its light is in volume")
        expect(residual <= TOLERANCE * max(max(beauty), 0.1), f"cloud: the splits add up through it ({residual:+.3f})")
        s = pixel(*SHADOWED)
        print(f"wall in shadow: shadow {s.get('shadow.G', 0):.3f} key {s.get('key.G', 0):.3f}")
        expect(s.get("shadow.G", 0) > 0.05, "wall: the sun's shadow is in shadow")
        expect(abs(s.get("holdoutshadow.G", 0)) < 1e-3, "wall: no holdout shadow on a visible wall")

# Sample filters on three depth AOVs (Karma's depth, Hydra's cameraDepth
# and P_camera's z are all the same depth, negated for the last two), in two
# renders, at the diffuse sphere's silhouette against the wall.
DEPTHS = [("depth", "ray:hitPz", "float"), ("camdepth", "cameraDepth", "float"), ("peye", "ray:hitP_camera", "vector3f")]
FILTER_RUNS = [{"depth": ("eldr", "min"), "camdepth": ("karma", '["minmax",{"mode":"zmin"}]'), "peye": ("eldr", "farthest"),
                "directdiffuse": ("eldr", "nearest")},
               {"depth": ("karma", '["minmax",{"mode":"max"}]'), "camdepth": ("eldr", "nearest"), "peye": ("eldr", "box")}]
depths = []
for i, filters in enumerate(FILTER_RUNS):
    write_scene(False, filters, DEPTHS)
    log = render(["eldr:rtRefraction=0"])
    if i == 0:
        expect("no longer adds up" in log, "a light split with another filter warns")
    x0, y0 = POSITIONS["diffuse"]
    centre = (int((x0 + 6) / 12 * SIZE), int((6 - y0) / 12 * SIZE))
    depths.append({(dx, dy): pixel(centre[0] + dx, centre[1] + dy) for dy in (-15, 3, 10) for dx in range(17, 26)})
edges = 0
blended = False
for at, p in depths[0].items():
    q = depths[1][at]
    lo, hi = p.get("depth.Z", 0), q.get("depth.Z", 0)  # min, max
    closest, farthest = -p.get("camdepth.Z", 0), -p.get("peye.B", 0)
    nearest, box = -q.get("camdepth.Z", 0), -q.get("peye.B", 0)
    if hi - lo < 1.0:
        continue
    edges += 1
    print(f"edge {at}: min {lo:.3f} closest {closest:.3f} nearest {nearest:.3f} box {box:.3f} farthest {farthest:.3f} max {hi:.3f}")
    expect(abs(closest - lo) < 1e-3, f"edge {at}: closest ({closest:.3f}) is the min ({lo:.3f})")
    expect(abs(farthest - hi) < 1e-3, f"edge {at}: farthest ({farthest:.3f}) is the max ({hi:.3f})")
    expect(abs(nearest - hi) < 0.01 or lo - 1e-3 <= nearest <= lo + 0.6, f"edge {at}: nearest ({nearest:.3f}) is one sample")
    expect(lo - 1e-3 <= box <= hi + 1e-3, f"edge {at}: box ({box:.3f}) is within min and max")
    blended = blended or lo + 0.05 < box < hi - 0.05
expect(edges >= 2, f"the silhouette is found ({edges} edge pixels)")
expect(blended, "box blends the sphere and the wall somewhere")

# The wall as a holdout: black, its shadow in holdoutshadow only.
write_scene(True)
render(["eldr:rtRefraction=0"])
s = pixel(*SHADOWED)
print(f"holdout wall in shadow: holdoutshadow {s.get('holdoutshadow.G', 0):.3f} shadow {s.get('shadow.G', 0):.3f}")
expect(s.get("holdoutshadow.G", 0) > 0.05, "holdout wall: its shadow is in holdoutshadow")
expect(abs(s.get("shadow.G", 0)) < 1e-3 and abs(s.get("directdiffuse.G", 0)) < 1e-3,
       "holdout wall: none of its light in shadow or directdiffuse")

print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
