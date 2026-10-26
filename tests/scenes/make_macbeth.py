"""Writes macbeth.usda: a 6 x 4 ColorChecker facing +Z, lit by one of four lighting variants.
Reflectances: ColorChecker Classic, BabelColor 2005 average of 30 charts (xyY under D50, as in
colour-science COLORCHECKER_2005; typed in from memory, verify before relying on the last digit),
Bradford-adapted D50 -> D65 and converted to linear Rec.709/sRGB (Lindbloom matrix below).
Run: python tests/scenes/make_macbeth.py"""
import os

# name, x, y, Y (xyY, D50)
CHART = [
    ("dark skin", .4316, .3777, .1008), ("light skin", .4197, .3744, .3495), ("blue sky", .2760, .3016, .1836),
    ("foliage", .3703, .4499, .1325), ("blue flower", .2999, .2856, .2304), ("bluish green", .2848, .3911, .4178),
    ("orange", .5295, .4055, .3118), ("purplish blue", .2305, .2106, .1175), ("moderate red", .5012, .3273, .1929),
    ("purple", .3319, .2482, .0643), ("yellow green", .3984, .5008, .4393), ("orange yellow", .4957, .4427, .4357),
    ("blue", .2018, .1692, .0575), ("green", .3253, .5032, .2318), ("red", .5686, .3303, .1257),
    ("yellow", .4697, .4734, .5981), ("magenta", .4159, .2688, .2009), ("cyan", .2131, .3023, .1930),
    ("white 9.5", .3469, .3608, .9065), ("neutral 8", .3440, .3584, .5894), ("neutral 6.5", .3432, .3581, .3620),
    ("neutral 5", .3446, .3579, .1915), ("neutral 3.5", .3401, .3548, .0883), ("black 2", .3406, .3537, .0315)]
M = [(3.1338561, -1.6168667, -0.4906146), (-0.9787684, 1.9161415, 0.0334540), (0.0719453, -0.2289914, 1.4052427)]


def rgb(x, y, Y):
    X, Z = x * Y / y, (1 - x - y) * Y / y
    # Cyan is outside Rec.709 (red -0.04): clamped to 0, so that patch is the nearest in-gamut colour.
    return tuple(max(0.0, sum(m * v for m, v in zip(row, (X, Y, Z)))) for row in M)


out = ['#usda 1.0\n(\n    defaultPrim = "Macbeth"\n    upAxis = "Y"\n    metersPerUnit = 1\n)\n']
out.append('''# ColorChecker Classic, flat diffuse patches (roughness 1, specular 0). Values: BabelColor 2005
# average, D50 xyY -> Bradford to D65 -> linear Rec.709. See tests/scenes/make_macbeth.py.
# The light variant set picks the lighting; "hdri" is the default. StinsonBeach.hdr ships with
# USD and resolves through the asset search path (usdenv.cmd sets it).
def Xform "Macbeth" (
    variants = {string light = "hdri"}
    prepend variantSets = "light"
)
{
    def Camera "cam"
    {
        float focalLength = 50
        float horizontalAperture = 36
        float verticalAperture = 24
        float2 clippingRange = (1, 100)
        double3 xformOp:translate = (0, 0, 10)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
    variantSet "light" = {
        "hdri" {
            def DomeLight "dome"
            {
                asset inputs:texture:file = @StinsonBeach.hdr@
                float inputs:intensity = 1
            }
        }
        "white" {
            def DomeLight "dome"
            {
                color3f inputs:color = (1, 1, 1)
                float inputs:intensity = 1
            }
        }
        "distant" {
            def DistantLight "sun"
            {
                float inputs:intensity = 3
                float inputs:angle = 0
            }
        }
        "rect" {
            def RectLight "panel"
            {
                float inputs:width = 4
                float inputs:height = 4
                float inputs:intensity = 3
                double3 xformOp:translate = (0, 5, 5)
                float xformOp:rotateX = -45
                uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateX"]
            }
        }
    }
    def Scope "chart"
    {''')
mats = []
for i, (name, x, y, Y) in enumerate(CHART):
    r, g, b = rgb(x, y, Y)
    cx, cy = (i % 6 - 2.5) * 1.1, (1.5 - i // 6) * 1.1
    out.append(f'''        def Mesh "p{i + 1:02d}" (
            prepend apiSchemas = ["MaterialBindingAPI"]
        )
        {{
            # {i + 1} {name}: xyY D50 {x} {y} {Y}
            int[] faceVertexCounts = [4]
            int[] faceVertexIndices = [0, 1, 2, 3]
            point3f[] points = [({cx - .5:.3f}, {cy - .5:.3f}, 0), ({cx + .5:.3f}, {cy - .5:.3f}, 0), ({cx + .5:.3f}, {cy + .5:.3f}, 0), ({cx - .5:.3f}, {cy + .5:.3f}, 0)]
            normal3f[] normals = [(0, 0, 1), (0, 0, 1), (0, 0, 1), (0, 0, 1)] (interpolation = "vertex")
            uniform token subdivisionScheme = "none"
            rel material:binding = </Macbeth/mtl/m{i + 1:02d}>
        }}''')
    mats.append(f'''        def Material "m{i + 1:02d}"
        {{
            token outputs:surface.connect = </Macbeth/mtl/m{i + 1:02d}/pbr.outputs:surface>
            def Shader "pbr"
            {{
                uniform token info:id = "UsdPreviewSurface"
                color3f inputs:diffuseColor = ({r:.5f}, {g:.5f}, {b:.5f})
                int inputs:useSpecularWorkflow = 1
                color3f inputs:specularColor = (0, 0, 0)
                float inputs:roughness = 1
                token outputs:surface
            }}
        }}''')
out.append('    }\n    def Scope "mtl"\n    {\n' + "\n".join(mats) + '\n    }\n}\n')
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "macbeth.usda"), "w").write("\n".join(out))
