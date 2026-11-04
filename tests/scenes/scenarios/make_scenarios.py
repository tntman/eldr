"""Writes the reflection/refraction scenario scenes (tests/scenes/scenarios/<name>.usda).
Each scene: camera /cam, a constant dome /sky, a key light /key (the furnace has only its dome),
MaterialX standard_surface materials with a UsdPreviewSurface fallback output (Blender's USD
importer reads only that), and RenderSettings /Render/eldr_default, /Render/eldr_rt (Eldr's traced
reflections, refraction and shadows) and /Render/karma. README.md says what to look for.
Run: python tests/scenes/scenarios/make_scenarios.py"""
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
TEX = "../textures/"  # tests/scenes/textures, relative to this folder
RES = (960, 540)


def f(v):
    return "%g" % v if v else "0"


def v3(v):
    return "(" + ", ".join(f(x) for x in v) + ")"


def arr(vs):
    return "[" + ", ".join(v3(v) if isinstance(v, tuple) else f(v) for v in vs) + "]"


def norm(v):
    n = math.sqrt(sum(x * x for x in v))
    return tuple(x / n for x in v)


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


class Scene:
    def __init__(self, name, doc, mpu=1):
        self.name, self.doc, self.mpu = name, doc, mpu
        self.prims, self.mats = [], []

    def camera(self, pos, target, focal=35, clip=(0.1, 10000)):
        fwd = norm(sub(target, pos))
        x = norm(cross(fwd, (0, 1, 0)))
        y = cross(x, fwd)
        z = tuple(-c for c in fwd)
        m = "(" + ", ".join("(%s, 0)" % ", ".join(f(c) for c in r) for r in (x, y, z)) + ", (%s, 1))" % ", ".join(f(c) for c in pos)
        self.prims.append(f'''def Camera "cam"
{{
    float focalLength = {f(focal)}
    float horizontalAperture = 36
    float verticalAperture = 20.25
    float2 clippingRange = ({f(clip[0])}, {f(clip[1])})
    matrix4d xformOp:transform = {m}
    uniform token[] xformOpOrder = ["xformOp:transform"]
}}''')

    def dome(self, color=(0.55, 0.62, 0.75), intensity=1.0):
        self.prims.append(f'''def DomeLight "sky"
{{
    color3f inputs:color = {v3(color)}
    float inputs:intensity = {f(intensity)}
}}''')

    def sun(self, rot=(-45, 35, 0), intensity=3.0, angle=0.53, name="key"):
        self.prims.append(f'''def DistantLight "{name}"
{{
    float inputs:angle = {f(angle)}
    float inputs:intensity = {f(intensity)}
    bool inputs:normalize = 1
    float3 xformOp:rotateXYZ = {v3(rot)}
    uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
}}''')

    def rect(self, name, pos, rot, size, intensity, color=(1, 1, 1)):
        self.prims.append(f'''def RectLight "{name}"
{{
    color3f inputs:color = {v3(color)}
    float inputs:width = {f(size[0])}
    float inputs:height = {f(size[1])}
    float inputs:intensity = {f(intensity)}
    bool inputs:normalize = 1
    double3 xformOp:translate = {v3(pos)}
    float3 xformOp:rotateXYZ = {v3(rot)}
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateXYZ"]
}}''')

    def material(self, name, tex=None, cutout=None, **ss):
        """standard_surface inputs as keywords (floats, 3-tuples for colours, bools); tex: a base colour
        image (tests/scenes/textures), cutout: an RGBA image whose alpha is the opacity."""
        ss = dict({"base": 1.0, "base_color": (0.8, 0.8, 0.8), "specular_roughness": 0.5}, **ss)
        p = f"/mtl/{name}"
        lines = []
        for k, val in ss.items():
            if isinstance(val, bool):
                lines.append(f"bool inputs:{k} = {int(val)}")
            elif isinstance(val, tuple):
                lines.append(f"color3f inputs:{k} = {v3(val)}")
            else:
                lines.append(f"float inputs:{k} = {f(val)}")
        nodes, pnodes = "", ""
        img = tex or cutout
        if img:
            lines.append(f"color3f inputs:base_color.connect = <{p}/rgb.outputs:out>")
            nodes += f'''
        def Shader "image"
        {{
            uniform token info:id = "ND_image_color4"
            asset inputs:file = @{TEX}{img}@ (colorSpace = "srgb_texture")
            color4f outputs:out
        }}
        def Shader "rgb"
        {{
            uniform token info:id = "ND_convert_color4_color3"
            color4f inputs:in.connect = <{p}/image.outputs:out>
            color3f outputs:out
        }}'''
            pnodes += f'''
        def Shader "tex"
        {{
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @{TEX}{img}@
            float2 inputs:st.connect = <{p}/st.outputs:result>
            token inputs:sourceColorSpace = "sRGB"
            token inputs:wrapS = "repeat"
            token inputs:wrapT = "repeat"
            float3 outputs:rgb
            float outputs:a
        }}
        def Shader "st"
        {{
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st"
            float2 outputs:result
        }}'''
        if cutout:
            lines.append(f"color3f inputs:opacity.connect = <{p}/alpha.outputs:out>")
            nodes += f'''
        def Shader "extractA"
        {{
            uniform token info:id = "ND_extract_color4"
            color4f inputs:in.connect = <{p}/image.outputs:out>
            int inputs:index = 3
            float outputs:out
        }}
        def Shader "alpha"
        {{
            uniform token info:id = "ND_convert_float_color3"
            float inputs:in.connect = <{p}/extractA.outputs:out>
            color3f outputs:out
        }}'''
        # The UsdPreviewSurface fallback: the closest mapping of the same inputs.
        g = ss.get
        base = tuple(g("base") * c for c in g("base_color"))
        pv = [f"float inputs:roughness = {f(g('specular_roughness'))}",
              f"float inputs:metallic = {f(g('metalness', 0.0))}",
              f"float inputs:ior = {f(g('specular_IOR', 1.5))}"]
        pv.append(f"color3f inputs:diffuseColor.connect = <{p}/tex.outputs:rgb>" if img
                  else f"color3f inputs:diffuseColor = {v3(g('transmission_color', base) if g('transmission') else base)}")
        if g("transmission"):
            pv.append(f"float inputs:opacity = {f(1 - g('transmission'))}")
        if cutout:
            pv += [f"float inputs:opacity.connect = <{p}/tex.outputs:a>", "float inputs:opacityThreshold = 0.5"]
        if g("coat"):
            pv += [f"float inputs:clearcoat = {f(g('coat'))}", f"float inputs:clearcoatRoughness = {f(g('coat_roughness', 0.1))}"]
        if g("emission"):
            pv.append(f"color3f inputs:emissiveColor = {v3(tuple(g('emission') * c for c in g('emission_color', (1, 1, 1))))}")
        body = "\n            ".join(lines)
        pbody = "\n            ".join(pv)
        self.mats.append(f'''    def Material "{name}"
    {{
        token outputs:mtlx:surface.connect = <{p}/surface.outputs:out>
        token outputs:surface.connect = <{p}/preview.outputs:surface>
        def Shader "surface"
        {{
            uniform token info:id = "ND_standard_surface_surfaceshader"
            {body}
            token outputs:out
        }}{nodes}
        def Shader "preview"
        {{
            uniform token info:id = "UsdPreviewSurface"
            {pbody}
            token outputs:surface
        }}{pnodes}
    }}''')
        return name

    def _xf(self, t=None, r=None, s=None):
        ops, order = [], []
        for op, typ, val in (("translate", "double3", t), ("rotateXYZ", "float3", r), ("scale", "float3", s)):
            if val is not None:
                ops.append(f"    {typ} xformOp:{op} = {v3(val)}")
                order.append(f'"xformOp:{op}"')
        if order:
            ops.append(f"    uniform token[] xformOpOrder = [{', '.join(order)}]")
        return "\n".join(ops)

    def _bind(self, kind, name, mat, body, t, r, s):
        self.prims.append(f'''def {kind} "{name}" (
    prepend apiSchemas = ["MaterialBindingAPI"]
)
{{
{body}
{self._xf(t, r, s)}
    rel material:binding = </mtl/{mat}>
}}''')

    def sphere(self, name, mat, center, radius):
        self._bind("Sphere", name, mat, f"    double radius = {f(radius)}", center, None, None)

    def cube(self, name, mat, center, size, rot=None):
        """size: (x, y, z) extents."""
        self._bind("Cube", name, mat, "    double size = 1", center, rot, size)

    def cylinder(self, name, mat, center, radius, height, axis="Y", rot=None):
        self._bind("Cylinder", name, mat, f'    double radius = {f(radius)}\n    double height = {f(height)}\n    token axis = "{axis}"',
                   center, rot, None)

    def mesh(self, name, mat, points, faces, normals=None, st=None, t=None, r=None, s=None):
        body = [f"    int[] faceVertexCounts = {arr([len(fc) for fc in faces])}",
                f"    int[] faceVertexIndices = {arr([i for fc in faces for i in fc])}",
                f"    point3f[] points = {arr(points)}",
                '    uniform token subdivisionScheme = "none"']
        if normals:
            body.append(f'    normal3f[] normals = {arr(normals)} (\n        interpolation = "vertex"\n    )')
        if st:
            body.append(f'    texCoord2f[] primvars:st = [{", ".join("(%s, %s)" % (f(a), f(b)) for a, b in st)}] (\n        interpolation = "vertex"\n    )')
        self._bind("Mesh", name, mat, "\n".join(body), t, r, s)

    def quad(self, name, mat, corners, tiles=1, normal=None):
        """corners in counter-clockwise order seen from the front; tiles: texture repeats."""
        st = [(0, 0), (tiles, 0), (tiles, tiles), (0, tiles)]
        self.mesh(name, mat, corners, [(0, 1, 2, 3)], [normal] * 4 if normal else None, st)

    def floor(self, mat, half, y=0.0, tiles=1, name="floor", z=(None, None)):
        z0, z1 = z[0] if z[0] is not None else -half, z[1] if z[1] is not None else half
        self.quad(name, mat, [(-half, y, z1), (half, y, z1), (half, y, z0), (-half, y, z0)], tiles, (0, 1, 0))

    def lathe(self, name, mat, profile, segments=64, t=None):
        """Revolves a profile [(radius, y)] about +Y; each profile segment is its own band (crisp
        edges), smooth around. The profile runs so that the solid lies to its right (outward normals)."""
        pts, nrm, faces = [], [], []
        for (r0, y0), (r1, y1) in zip(profile, profile[1:]):
            n = norm((y1 - y0, -(r1 - r0)))
            base = len(pts)
            for r, y in ((r0, y0), (r1, y1)):
                for j in range(segments):
                    a = 2 * math.pi * j / segments
                    pts.append((r * math.cos(a), y, r * math.sin(a)))
                    nrm.append((n[0] * math.cos(a), n[1], n[0] * math.sin(a)))
            for j in range(segments):
                k = (j + 1) % segments
                fc = (base + j, base + k, base + segments + k, base + segments + j)
                fc = _orient(fc, pts, nrm)
                faces.append(fc)
        self.mesh(name, mat, pts, faces, nrm, t=t)

    def write(self, eldr_rt_extra=None, karma_samples=128):
        rs = f"        rel camera = </cam>\n        uniform int2 resolution = {v3(RES)}"
        rt = "\n".join(f"        custom {('int' if isinstance(v, int) and not isinstance(v, bool) else 'bool')} eldr:{k} = {int(v)}"
                       for k, v in dict(RT_SETTINGS, **(eldr_rt_extra or {})).items())
        text = f'''#usda 1.0
(
    metersPerUnit = {f(self.mpu)}
    renderSettingsPrimPath = "/Render/eldr_default"
    upAxis = "Y"
)

{self.doc.strip()}
# Written by make_scenarios.py; edit that and run it again.

''' + "\n\n".join(self.prims) + '''

def Scope "mtl"
{
''' + "\n".join(self.mats) + f'''
}}

def Scope "Render"
{{
    def RenderSettings "eldr_default"
    {{
{rs}
    }}
    def RenderSettings "eldr_rt"
    {{
{rs}
{rt}
    }}
    def RenderSettings "karma"
    {{
{rs}
        custom string karma:global:imagefilter = "[[\\"denoise\\", {{\\"engine\\": \\"oidn\\", \\"use_albedo_input\\": true, \\"use_n_input\\": true, \\"forcecpu\\": false, \\"aovs\\": [\\"C\\"]}}]]"
        custom int karma:global:pathtracedsamples = {karma_samples}
    }}
}}
'''
        with open(os.path.join(HERE, self.name + ".usda"), "w", newline="\n") as fh:
            fh.write(text)


# tests/scenes/final_settings.usda plus traced shadows and a converged film.
RT_SETTINGS = {"filmRestart": True, "filmSamples": 64, "rtReflections": True, "rtRefraction": True,
               "rtHitShadows": True, "rtShadows": True, "finalGather": True, "caustics": True,
               "rtShadowSamples": 1, "rtShadowLights": 16}


def _orient(fc, pts, nrm):
    a, b, c = (pts[i] for i in fc[:3])
    return fc if dot(cross(sub(b, a), sub(c, a)), nrm[fc[0]]) >= 0 else tuple(reversed(fc))


def common(s, matte_tex=True):
    s.dome()
    s.sun()
    if matte_tex:
        s.material("checker", tex="quadrants.png", specular_roughness=0.7)
    s.material("matte", base_color=(0.5, 0.5, 0.5), specular_roughness=0.8)


def scenes():
    out = []

    s = Scene("mirror_floor_roughness", """
# Three floor tiles of the same dark dielectric (IOR 1.5) at specular
# roughness 0, 0.2 and 0.5, each under a red sphere and a blue box. Look at:
# the reflection's shape and position (no smear or stretch), its sharpness
# growing blur with roughness, Fresnel (stronger reflection towards the
# horizon), and the sphere's highlight and dark rim inside the reflection.""")
    common(s)
    s.camera((0, 1.6, 7.5), (0, 0.5, 0), 35)
    for i, rough in enumerate((0.0, 0.2, 0.5)):
        x = (i - 1) * 3.2
        m = s.material(f"floor{i}", base_color=(0.05, 0.05, 0.06), specular_roughness=rough)
        s.quad(f"tile{i}", m, [(x - 1.5, 0, 3), (x + 1.5, 0, 3), (x + 1.5, 0, -3), (x - 1.5, 0, -3)], 1, (0, 1, 0))
        s.sphere(f"ball{i}", s.material(f"red{i}", base_color=(0.7, 0.08, 0.05), specular_roughness=0.25), (x - 0.4, 0.6, -0.5), 0.6)
        s.cube(f"box{i}", s.material(f"blue{i}", base_color=(0.1, 0.2, 0.7), specular_roughness=0.4), (x + 0.6, 0.35, 0.6), (0.7, 0.7, 0.7), (0, 30, 0))
    s.floor("checker", 20, -0.01, 20, "ground")
    out.append(s)

    s = Scene("metal_interreflection", """
# Gold (roughness 0), silver (0.15) and copper (0.4) spheres touching, and a
# chrome cube behind them, on a checker floor. Look at: each metal showing
# the others (inter-reflection needs a second bounce in the reflected
# metal), the dark gaps where they touch, metal colour of the edges.""")
    common(s)
    s.camera((0.3, 1.4, 5), (0, 0.6, 0), 40)
    s.floor("checker", 10, 0, 10)
    s.sphere("gold", s.material("gold", base_color=(1.0, 0.77, 0.34), metalness=1.0, specular_roughness=0.0), (-1.0, 0.6, 0), 0.6)
    s.sphere("silver", s.material("silver", base_color=(0.95, 0.94, 0.92), metalness=1.0, specular_roughness=0.15), (0.2, 0.6, 0), 0.6)
    s.sphere("copper", s.material("copper", base_color=(0.95, 0.64, 0.54), metalness=1.0, specular_roughness=0.4), (1.4, 0.6, 0), 0.6)
    s.cube("chrome", s.material("chrome", base_color=(0.9, 0.9, 0.9), metalness=1.0, specular_roughness=0.02), (0.2, 0.75, -1.5), (3.5, 1.5, 0.4))
    out.append(s)

    s = Scene("glass_solid_and_thin", """
# Solid glass (IOR 1.5): a sphere and a 0.3 m thick slab; and a thin-walled
# pane (standard_surface thin_walled), in front of a checker wall. Look at:
# the sphere inverts and shrinks the wall, the slab shifts it sideways at its
# angle, the thin pane doesn't bend it at all; none is black or opaque;
# reflections of the sky and key on each.""")
    common(s)
    s.camera((0, 1.2, 6), (0, 0.9, 0), 35)
    s.floor("checker", 10, 0, 10)
    s.quad("wall", "checker", [(-5, 0, -2.5), (5, 0, -2.5), (5, 5, -2.5), (-5, 5, -2.5)], 6, (0, 0, 1))
    glass = s.material("glass", transmission=1.0, specular_roughness=0.0, specular_IOR=1.5)
    thin = s.material("thinGlass", transmission=1.0, specular_roughness=0.0, thin_walled=True)
    s.sphere("ball", glass, (-1.8, 0.8, 0), 0.8)
    s.cube("slab", glass, (0, 0.9, 0), (1.2, 1.8, 0.3), (0, 35, 0))
    s.cube("pane", thin, (1.8, 0.9, 0), (1.2, 1.8, 0.01), (0, 35, 0))
    out.append(s)

    s = Scene("glass_behind_glass", """
# Glass seen through glass: a clear sphere in front of a clear slab, a clear
# pane in front of a green glass sphere, two overlapping thin panes; checker
# wall behind. Look at: the far glass still refracts and reflects when seen
# through the near one (not black, not missing, not opaque), tint only where
# the green glass is.""")
    common(s)
    s.camera((0, 1.2, 6), (0, 0.9, 0), 35)
    s.floor("checker", 10, 0, 10)
    s.quad("wall", "checker", [(-5, 0, -3), (5, 0, -3), (5, 5, -3), (-5, 5, -3)], 6, (0, 0, 1))
    glass = s.material("glass", transmission=1.0, specular_roughness=0.0)
    green = s.material("green", transmission=1.0, specular_roughness=0.0, transmission_color=(0.3, 0.85, 0.4))
    thin = s.material("thinGlass", transmission=1.0, specular_roughness=0.0, thin_walled=True)
    s.sphere("frontBall", glass, (-1.9, 0.6, 1.0), 0.6)
    s.cube("backSlab", glass, (-1.9, 1.0, -1.0), (1.4, 2.0, 0.3))
    s.cube("frontPane", glass, (0.1, 0.9, 1.0), (1.0, 1.8, 0.1))
    s.sphere("greenBall", green, (0.1, 0.7, -0.6), 0.7)
    s.cube("pane1", thin, (1.9, 0.9, 0.8), (1.2, 1.8, 0.01), (0, 20, 0))
    s.cube("pane2", thin, (2.2, 0.9, -0.4), (1.2, 1.8, 0.01), (0, -20, 0))
    out.append(s)

    s = Scene("water_over_tiles", """
# A pool: a closed block of water (IOR 1.33, faint blue-green, rippled top
# from real geometry) over a tiled bottom and tiled walls. Look at: the tiles
# seen through the water bend with the ripples; the sky and the key reflect
# on the surface, strongest at grazing angles at the back (Fresnel); the
# water isn't black, milky or opaque.""")
    common(s)
    s.camera((0, 2.2, 5.5), (0, -0.3, 0), 35)
    s.material("tiles", tex="udim.1001.png", specular_roughness=0.4)
    W, D, top, bot = 3.0, 2.0, -0.05, -0.8
    s.quad("bottom", "tiles", [(-W, bot - 0.01, D), (W, bot - 0.01, D), (W, bot - 0.01, -D), (-W, bot - 0.01, -D)], 8, (0, 1, 0))
    s.quad("backWall", "tiles", [(-W, bot, -D - 0.01), (W, bot, -D - 0.01), (W, 0.2, -D - 0.01), (-W, 0.2, -D - 0.01)], 4, (0, 0, 1))
    s.quad("leftWall", "tiles", [(-W - 0.01, bot, D), (-W - 0.01, bot, -D), (-W - 0.01, 0.2, -D), (-W - 0.01, 0.2, D)], 4, (1, 0, 0))
    s.quad("rightWall", "tiles", [(W + 0.01, bot, -D), (W + 0.01, bot, D), (W + 0.01, 0.2, D), (W + 0.01, 0.2, -D)], 4, (-1, 0, 0))
    for nm, corners in (("deckFront", [(-6, 0.2, 6), (6, 0.2, 6), (6, 0.2, D), (-6, 0.2, D)]),
                        ("deckBack", [(-6, 0.2, -D), (6, 0.2, -D), (6, 0.2, -6), (-6, 0.2, -6)])):
        s.quad(nm, "matte", corners, 1, (0, 1, 0))
    water = s.material("water", transmission=1.0, specular_roughness=0.0, specular_IOR=1.33,
                       transmission_color=(0.8, 0.95, 0.93), transmission_depth=2.0)
    n = 48
    def h(x, z):
        return top + 0.02 * math.sin(3.1 * x + 1.3 * z) + 0.015 * math.sin(2.3 * z - 4.1 * x)
    def hn(x, z):
        dx = 0.02 * 3.1 * math.cos(3.1 * x + 1.3 * z) - 0.015 * 4.1 * math.cos(2.3 * z - 4.1 * x)
        dz = 0.02 * 1.3 * math.cos(3.1 * x + 1.3 * z) + 0.015 * 2.3 * math.cos(2.3 * z - 4.1 * x)
        return norm((-dx, 1, -dz))
    pts, nrm, faces = [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            x, z = -W + 2 * W * i / n, -D + 2 * D * j / n
            pts.append((x, h(x, z), z))
            nrm.append(hn(x, z))
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            faces.append(_orient((a, a + 1, a + n + 2, a + n + 1), pts, nrm))
    # Sides and bottom close the block: their own vertices, flat normals.
    edges = [([(i, 0) for i in range(n + 1)], (0, 0, -1)), ([(i, n) for i in range(n + 1)], (0, 0, 1)),
             ([(0, j) for j in range(n + 1)], (-1, 0, 0)), ([(n, j) for j in range(n + 1)], (1, 0, 0))]
    for ring, nn in edges:
        for (i0, j0), (i1, j1) in zip(ring, ring[1:]):
            t0, t1 = pts[j0 * (n + 1) + i0], pts[j1 * (n + 1) + i1]
            b = len(pts)
            pts += [t0, t1, (t1[0], bot, t1[2]), (t0[0], bot, t0[2])]
            nrm += [nn] * 4
            faces.append(_orient((b, b + 1, b + 2, b + 3), pts, nrm))
    b = len(pts)
    pts += [(-W, bot, -D), (W, bot, -D), (W, bot, D), (-W, bot, D)]
    nrm += [(0, -1, 0)] * 4
    faces.append(_orient((b, b + 1, b + 2, b + 3), pts, nrm))
    s.mesh("water", water, pts, faces, nrm)
    out.append(s)

    s = Scene("liquid_in_glass", """
# A tumbler (solid glass walls 6 mm, 15 mm base) two-thirds full of an amber
# liquid (IOR 1.34, absorbing), and an empty one beside it, on a checker
# table in front of a checker wall. Look at: the walls' bright and dark
# edges, the liquid's colour deepening with path length, the table and wall
# bent through both, the liquid surface's reflection, nothing black.""")
    common(s)
    s.camera((0, 0.16, 0.55), (0, 0.06, 0), 50, (0.01, 100))
    s.floor("checker", 2, 0, 16)
    s.quad("wall", "checker", [(-2, 0, -0.25), (2, 0, -0.25), (2, 1, -0.25), (-2, 1, -0.25)], 16, (0, 0, 1))
    glass = s.material("glass", transmission=1.0, specular_roughness=0.0)
    liquid = s.material("liquid", transmission=1.0, specular_roughness=0.0, specular_IOR=1.34,
                        transmission_color=(0.85, 0.45, 0.08), transmission_depth=0.04)
    R, H, t, b = 0.035, 0.11, 0.006, 0.015
    tumbler = [(0, 0), (R, 0), (R, H), (R - t, H), (R - t, b), (0, b)]
    s.lathe("tumbler", glass, tumbler, 64, (-0.045, 0, 0))
    e, L = 0.0005, 0.075
    s.lathe("liquid", liquid, [(0, b + e), (R - t - e, b + e), (R - t - e, L), (0, L)], 64, (-0.045, 0, 0))
    s.lathe("empty", glass, tumbler, 64, (0.045, 0, -0.02))
    out.append(s)

    s = Scene("coloured_glass", """
# Absorbing glass: three blue glass blocks 0.1, 0.3 and 0.9 m thick
# (transmission_depth 0.3, Beer-Lambert), a red glass sphere and a thin
# amber pane, over a white floor with a white wall behind. Look at: thicker
# blocks darker and more saturated; the sphere darker in its middle than
# its rim; coloured light under each (caustics/tinted shadows in Eldr RT and
# Karma).""")
    s.dome()
    s.sun((-55, 20, 0))
    s.material("white", base_color=(0.8, 0.8, 0.8), specular_roughness=0.7)
    s.camera((0, 1.5, 5.5), (0, 0.5, 0), 35)
    s.floor("white", 10, 0, 1)
    s.quad("wall", "white", [(-5, 0, -2.5), (5, 0, -2.5), (5, 5, -2.5), (-5, 5, -2.5)], 1, (0, 0, 1))
    blue = s.material("blue", transmission=1.0, specular_roughness=0.0, transmission_color=(0.25, 0.45, 0.9), transmission_depth=0.3)
    for i, th in enumerate((0.1, 0.3, 0.9)):
        s.cube(f"block{i}", blue, (-2.2 + i * 1.1, 0.45, 0.8 - th / 2), (0.8, 0.9, th))
    s.sphere("redBall", s.material("red", transmission=1.0, specular_roughness=0.0, transmission_color=(0.9, 0.15, 0.1), transmission_depth=0.5),
             (1.5, 0.6, 0.3), 0.6)
    s.cube("amberPane", s.material("amber", transmission=1.0, specular_roughness=0.0, thin_walled=True, transmission_color=(1.0, 0.6, 0.1)),
           (2.9, 0.7, -0.5), (0.8, 1.4, 0.01), (0, -25, 0))
    out.append(s)

    s = Scene("refraction_busy_background", """
# A glass sphere, a glass cylinder lying down and a glass cube in front of a
# densely tiled background: tiled wall and floor and coloured boxes and balls
# behind the glass, some only visible through it. Look at: the background
# bent correctly (inverted in the sphere, compressed in the cylinder);
# objects behind the glass appear in it at the right place, no smearing,
# stretching or ghosting of the foreground into the glass.""")
    common(s)
    s.camera((0, 1.3, 6), (0, 0.8, 0), 35)
    s.floor("checker", 12, 0, 24)
    s.quad("wall", "checker", [(-6, 0, -3), (6, 0, -3), (6, 6, -3), (-6, 6, -3)], 12, (0, 0, 1))
    glass = s.material("glass", transmission=1.0, specular_roughness=0.0)
    s.sphere("ball", glass, (-1.8, 0.8, 0.5), 0.8)
    s.cylinder("rod", glass, (0.2, 0.4, 0.8), 0.4, 2.2, "X")
    s.cube("cube", glass, (2.0, 0.6, 0.6), (1.2, 1.2, 1.2), (0, 25, 0))
    for i, col in enumerate(((0.8, 0.1, 0.1), (0.1, 0.7, 0.2), (0.9, 0.7, 0.1), (0.2, 0.3, 0.9), (0.8, 0.3, 0.8))):
        m = s.material(f"col{i}", base_color=col, specular_roughness=0.4)
        x = -2.6 + i * 1.3
        if i % 2:
            s.sphere(f"behindBall{i}", m, (x, 0.35, -1.2), 0.35)
        else:
            s.cube(f"behindBox{i}", m, (x, 0.5, -1.4), (0.5, 1.0, 0.5), (0, 20 * i, 0))
    out.append(s)

    s = Scene("contact_reflections", """
# Glossy objects touching a glossy wall and floor: a red glossy box pressed
# against a white glossy wall (roughness 0.1), a chrome sphere touching the
# wall and floor, a glossy black floor (roughness 0.05). Look at: where
# each object meets the wall or floor its reflection meets it without a gap
# or offset (contact), the corner where wall meets floor reflects itself,
# no light leaking under the objects.""")
    common(s)
    s.camera((2.2, 1.3, 4.0), (0, 0.5, -0.5), 35)
    s.floor(s.material("blackGloss", base_color=(0.03, 0.03, 0.03), specular_roughness=0.05), 6, 0, 1, z=(-1.5, 6))
    s.quad("wall", s.material("whiteGloss", base_color=(0.75, 0.75, 0.72), specular_roughness=0.1),
           [(-6, 0, -1.5), (6, 0, -1.5), (6, 4, -1.5), (-6, 4, -1.5)], 1, (0, 0, 1))
    s.cube("box", s.material("redGloss", base_color=(0.7, 0.05, 0.05), specular_roughness=0.15), (-0.8, 0.4, -1.1), (0.8, 0.8, 0.8))
    s.sphere("chrome", s.material("chrome", base_color=(0.95, 0.95, 0.95), metalness=1.0, specular_roughness=0.0), (0.6, 0.5, -1.0), 0.5)
    s.cylinder("post", s.material("blueGloss", base_color=(0.1, 0.2, 0.7), specular_roughness=0.2), (1.8, 0.75, 0.2), 0.2, 1.5)
    out.append(s)

    s = Scene("car_paint", """
# Car-paint-like coat: a dark red metallic base (metalness 0.6, roughness
# 0.35) under a clear coat (coat 1, coat_roughness 0.02) on an ellipsoid and
# a sphere; beside them the same base without coat. A large softbox (rect
# light) above and the sky reflect in the coat. Look at: a sharp softbox
# and sky reflection on top of the blurred base highlight (two lobes); the
# coat darkening the base slightly; edges brighter (coat Fresnel).""")
    common(s)
    s.rect("softbox", (0, 3.5, 1.5), (-70, 0, 0), (3.0, 1.2), 6.0)
    s.camera((0, 1.2, 5.5), (0, 0.6, 0), 35)
    s.floor("matte", 10, 0, 1)
    paint = s.material("paint", base_color=(0.45, 0.02, 0.03), metalness=0.6, specular_roughness=0.35, coat=1.0, coat_roughness=0.02)
    bare = s.material("bare", base_color=(0.45, 0.02, 0.03), metalness=0.6, specular_roughness=0.35)
    s._bind("Sphere", "body", paint, "    double radius = 1", (-0.8, 0.6, 0), None, (1.6, 0.6, 0.9))
    s.sphere("ball", paint, (1.5, 0.6, 0.4), 0.6)
    s.sphere("bareBall", bare, (2.4, 0.4, -0.8), 0.4)
    out.append(s)

    s = Scene("facing_mirrors", """
# Two facing mirrors (metal, roughness 0) 3 m apart with a yellow sphere and
# a blue box between them, seen at an angle: the corridor of repeated
# reflections. Karma's default reflect limit is 4 bounces; count the
# repeated objects in each renderer (Eldr's traced reflections bounce once;
# what fills the deeper copies: sky, black, or the last image?).""")
    common(s)
    s.camera((0.8, 1.3, 2.8), (-1.5, 1.0, -0.6), 35)
    s.floor("checker", 10, 0, 10)
    mirror = s.material("mirror", base_color=(0.95, 0.95, 0.95), metalness=1.0, specular_roughness=0.0)
    s.quad("mirrorL", mirror, [(-1.5, 0.2, 3), (-1.5, 0.2, -3), (-1.5, 2.6, -3), (-1.5, 2.6, 3)], 1, (1, 0, 0))
    s.quad("mirrorR", mirror, [(1.5, 0.2, -3), (1.5, 0.2, 3), (1.5, 2.6, 3), (1.5, 2.6, -3)], 1, (-1, 0, 0))
    s.sphere("ball", s.material("yellow", base_color=(0.9, 0.7, 0.05), specular_roughness=0.3), (-0.3, 0.5, -0.5), 0.5)
    s.cube("box", s.material("blue", base_color=(0.1, 0.2, 0.8), specular_roughness=0.4), (0.4, 0.4, 0.6), (0.6, 0.8, 0.6), (0, 30, 0))
    out.append(s)

    s = Scene("offscreen_reflections", """
# A mirror-like floor (roughness 0.02) with objects cut by the frame: a tall
# column leaving the top of the image, a sphere half out of the left edge, a
# box out of the right edge, and a red wall behind the camera. Look at:
# reflections of the parts outside the frame (screen-space reflections can't
# see them: they fade or go missing; traced ones and Karma show them), the
# red wall in the floor near the bottom of the image.""")
    common(s)
    s.camera((0, 1.2, 5), (0, 0.4, 0), 35)
    s.floor(s.material("mirrorFloor", base_color=(0.04, 0.04, 0.05), specular_roughness=0.02), 12, 0, 1)
    s.quad("redWall", s.material("red", base_color=(0.7, 0.08, 0.05), specular_roughness=0.7),
           [(-6, 0, 8), (-6, 3, 8), (6, 3, 8), (6, 0, 8)], 1, (0, 0, -1))
    s.cylinder("column", s.material("stone", base_color=(0.7, 0.66, 0.6), specular_roughness=0.6), (0.3, 3, -1), 0.35, 6)
    s.sphere("leftBall", s.material("green", base_color=(0.1, 0.6, 0.2), specular_roughness=0.3), (-2.9, 0.7, 0.5), 0.7)
    s.cube("rightBox", s.material("blue", base_color=(0.1, 0.2, 0.8), specular_roughness=0.4), (3.0, 0.6, 0.3), (1.2, 1.2, 1.2), (0, 20, 0))
    out.append(s)

    s = Scene("thin_cards", """
# Cut-out cards (an RGBA ring image, opacity from its alpha) standing on a
# mirror-like floor, and two more behind a clear glass pane and a glass
# sphere. Look at: the holes stay holes in the floor's reflection and seen
# through the glass (not solid quads, not missing cards), card shadows
# ring-shaped.""")
    common(s)
    s.camera((0, 1.3, 5.5), (0, 0.8, 0), 35)
    s.floor(s.material("mirrorFloor", base_color=(0.04, 0.04, 0.05), specular_roughness=0.02), 10, 0, 1)
    ring = s.material("ring", cutout="cutout.png", specular_roughness=0.6)
    for i, x in enumerate((-2.4, -1.2)):
        s.quad(f"card{i}", ring, [(x - 0.5, 0, 0.3 * i), (x + 0.5, 0, 0.3 * i), (x + 0.5, 1, 0.3 * i), (x - 0.5, 1, 0.3 * i)], 1, (0, 0, 1))
    for i, x in enumerate((0.8, 2.2)):
        s.quad(f"cardBehind{i}", ring, [(x - 0.5, 0.2, -1.2), (x + 0.5, 0.2, -1.2), (x + 0.5, 1.2, -1.2), (x - 0.5, 1.2, -1.2)], 1, (0, 0, 1))
    glass = s.material("glass", transmission=1.0, specular_roughness=0.0)
    s.cube("pane", glass, (0.8, 0.8, 0), (1.3, 1.6, 0.05))
    s.sphere("ball", glass, (2.2, 0.6, 0.2), 0.6)
    out.append(s)

    s = Scene("emissive_reflected", """
# Emitters seen in reflections: an orange emissive panel and a cyan emissive
# ball (emission 4, under the colour limit of 20) next to a chrome sphere,
# over a glossy floor (roughness 0.1); dim sky and key. Look at: the
# emitters' reflections in the floor and the chrome (bright, right colour),
# the light they cast on the floor (Eldr: eldr:emissiveLights / final
# gather vs Karma).""")
    s.dome(intensity=0.15)
    s.sun(intensity=0.5)
    s.camera((0, 1.3, 5.5), (0, 0.6, 0), 35)
    s.floor(s.material("gloss", base_color=(0.2, 0.2, 0.2), specular_roughness=0.1), 10, 0, 1)
    s.quad("panel", s.material("orangeGlow", base_color=(0, 0, 0), emission=4.0, emission_color=(1.0, 0.5, 0.1)),
           [(-2.6, 0.3, -1), (-1.4, 0.3, -0.6), (-1.4, 1.8, -0.6), (-2.6, 1.8, -1)], 1)
    s.sphere("glowBall", s.material("cyanGlow", base_color=(0, 0, 0), emission=4.0, emission_color=(0.1, 0.8, 1.0)), (1.8, 0.35, 0.5), 0.35)
    s.sphere("chrome", s.material("chrome", base_color=(0.95, 0.95, 0.95), metalness=1.0, specular_roughness=0.0), (0, 0.7, -0.3), 0.7)
    out.append(s)

    s = Scene("subsurface_translucent", """
# Subsurface and translucency, back-lit: a skin-coloured ball (subsurface 1,
# radius scale 0.1 m), a jade ball (subsurface green, larger radius), a
# thin translucent leaf (thin_walled + subsurface) with the key behind it,
# beside an opaque diffuse ball of the same colour for reference. Look at:
# light bleeding into the shadow side, the leaf glowing from behind, the
# subsurface balls softer than the opaque one, colour of the scattered light.""")
    s.dome(intensity=0.6)
    s.sun((-25, 160, 0), 3.0)
    s.camera((0, 1.0, 5), (0, 0.6, 0), 35)
    s.floor(s.material("matte", base_color=(0.5, 0.5, 0.5), specular_roughness=0.8), 10, 0, 1)
    s.sphere("skin", s.material("skin", base_color=(0.85, 0.6, 0.5), subsurface=1.0, subsurface_color=(0.9, 0.6, 0.5),
                                subsurface_radius=(1.0, 0.35, 0.2), subsurface_scale=0.1, specular_roughness=0.4), (-1.9, 0.6, 0), 0.6)
    s.sphere("opaque", s.material("opaque", base_color=(0.85, 0.6, 0.5), specular_roughness=0.4), (-0.6, 0.6, 0), 0.6)
    s.sphere("jade", s.material("jade", base_color=(0.35, 0.75, 0.45), subsurface=1.0, subsurface_color=(0.35, 0.75, 0.45),
                                subsurface_radius=(0.5, 1.0, 0.6), subsurface_scale=0.3, specular_roughness=0.15), (0.7, 0.6, 0), 0.6)
    s.quad("leaf", s.material("leaf", base_color=(0.3, 0.6, 0.1), subsurface=1.0, subsurface_color=(0.4, 0.8, 0.1), thin_walled=True,
                              specular_roughness=0.4), [(1.6, 0.1, 0.2), (2.6, 0.1, -0.2), (2.6, 1.4, -0.2), (1.6, 1.4, 0.2)], 1)
    out.append(s)

    s = Scene("furnace_grey", """
# Energy check: a uniform white dome of intensity 1 and nothing else (no
# floor, no key light). Spheres, left to right: diffuse white (albedo 1:
# reads 1, disappears), diffuse grey 0.5 (reads 0.5), rough white metal
# (roughness 0.6, reads about 1 with multiple scattering), clear glass
# (reads 1, nearly vanishes), white diffuse with a clear coat (reads about
# 1). The background reads 1. Any sphere brighter than the background
# makes energy; a dark rim on the white ones loses it.""")
    s.dome((1, 1, 1), 1.0)
    s.camera((0, 0, 9), (0, 0, 0), 50)
    for i, (nm, kw) in enumerate((
            ("white", dict(base_color=(1, 1, 1), specular=0.0, specular_roughness=1.0)),
            ("grey", dict(base_color=(0.5, 0.5, 0.5), specular=0.0, specular_roughness=1.0)),
            ("roughMetal", dict(base_color=(1, 1, 1), metalness=1.0, specular_roughness=0.6)),
            ("glass", dict(transmission=1.0, specular_roughness=0.0)),
            ("coated", dict(base_color=(1, 1, 1), specular=0.0, specular_roughness=1.0, coat=1.0, coat_roughness=0.05)))):
        s.sphere(nm, s.material(nm, **kw), (-3.2 + i * 1.6, 0, 0), 0.65)
    out.append(s)

    s = Scene("sun_on_chrome", """
# Small bright highlights: chrome balls (roughness 0, 0.05, 0.15) under a
# sharp sun (angle 0.53 degrees, intensity 5) and a small sphere light, over
# a glossy floor (roughness 0.05). Look at: the sun's highlight a small hot
# dot on the smooth ball, growing and dimming with roughness; the same
# highlights seen again in the floor's reflection of the balls; no fireflies
# or blown halos.""")
    s.dome(intensity=0.6)
    s.sun((-35, 30, 0), 5.0, 0.53)
    s.prims.append('''def SphereLight "spot"
{
    float inputs:radius = 0.05
    float inputs:intensity = 40
    bool inputs:normalize = 1
    color3f inputs:color = (1, 0.85, 0.6)
    double3 xformOp:translate = (1.5, 2.5, 1.5)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}''')
    s.camera((0, 1.1, 5), (0, 0.5, 0), 40)
    s.floor(s.material("gloss", base_color=(0.06, 0.06, 0.07), specular_roughness=0.05), 10, 0, 1)
    for i, rough in enumerate((0.0, 0.05, 0.15)):
        s.sphere(f"chrome{i}", s.material(f"chrome{i}", base_color=(0.95, 0.95, 0.95), metalness=1.0, specular_roughness=rough),
                 (-1.4 + 1.4 * i, 0.5, 0), 0.5)
    out.append(s)

    s = Scene("large_scale", """
# Large scale: a 120 m glossy plaza (roughness 0.1) with 6-15 m tall blocks
# and 2 m chrome and glass balls 10-60 m from a camera at eye height.
# Look at: far reflections (screen-space reach, precision), shadows of the
# far blocks (cascades), glass and chrome balls far away (not black, not
# noisy), no banding or acne on the large floor.""")
    s.dome()
    s.sun((-35, 40, 0), 3.0)
    s.camera((0, 1.7, 12), (0, 3, -30), 30, (0.1, 1000))
    s.material("matte", base_color=(0.5, 0.5, 0.5), specular_roughness=0.8)
    s.floor(s.material("plaza", base_color=(0.15, 0.15, 0.16), specular_roughness=0.1), 60, 0, 1)
    for i, (x, z, w, hgt) in enumerate(((-18, -30, 8, 15), (-6, -50, 10, 12), (10, -40, 7, 9), (22, -25, 6, 6))):
        s.cube(f"block{i}", "matte", (x, hgt / 2, z), (w, hgt, w))
    s.sphere("chrome", s.material("chrome", base_color=(0.95, 0.95, 0.95), metalness=1.0, specular_roughness=0.0), (-4, 2, -8), 2)
    s.sphere("glass", s.material("glass", transmission=1.0, specular_roughness=0.0), (5, 2, -15), 2)
    s.sphere("red", s.material("red", base_color=(0.7, 0.08, 0.05), specular_roughness=0.3), (1, 2, -45), 2)
    out.append(s)

    s = Scene("tiny_scale", """
# Centimetre scale: beads 3-8 mm across (chrome, glass, red plastic) on a
# glossy 20 cm tile, camera 12 cm away (near clip 1 mm). Look at: the same
# image you'd expect at metre scale: no shadow acne or detached shadows, no
# self-intersection speckle on the glass, reflections of the beads in the
# tile, the glass not black.""")
    s.dome()
    s.sun((-45, 35, 0), 3.0)
    s.camera((0, 0.03, 0.12), (0, 0.004, 0), 50, (0.001, 10))
    s.material("checker", tex="quadrants.png", specular_roughness=0.7)
    s.floor("checker", 1, -0.0005, 40, "ground")
    s.quad("tile", s.material("gloss", base_color=(0.06, 0.06, 0.07), specular_roughness=0.05),
           [(-0.1, 0, 0.1), (0.1, 0, 0.1), (0.1, 0, -0.1), (-0.1, 0, -0.1)], 1, (0, 1, 0))
    s.sphere("chrome", s.material("chrome", base_color=(0.95, 0.95, 0.95), metalness=1.0, specular_roughness=0.0), (-0.012, 0.004, 0), 0.004)
    s.sphere("glass", s.material("glass", transmission=1.0, specular_roughness=0.0), (0, 0.003, 0.004), 0.003)
    s.sphere("red", s.material("red", base_color=(0.7, 0.08, 0.05), specular_roughness=0.3), (0.012, 0.0035, -0.003), 0.0035)
    s.cube("die", s.material("white", base_color=(0.8, 0.8, 0.8), specular_roughness=0.4), (0.005, 0.0025, -0.012), (0.005, 0.005, 0.005), (0, 30, 0))
    out.append(s)
    return out


if __name__ == "__main__":
    for s in scenes():
        s.write()
        print("wrote", s.name + ".usda")
