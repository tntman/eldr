"""Writes displacement.usda next to this file (stdlib only): python make_displacement.py"""
import os

N = 16  # quads per side


def grid(size):
    pts, idx = [], []
    for j in range(N + 1):
        for i in range(N + 1):
            pts.append((size * (i / N - 0.5), 0.0, size * (0.5 - j / N)))
    for j in range(N):
        for i in range(N):
            a = j * (N + 1) + i
            idx += [a, a + 1, a + N + 2, a + N + 1]
    st = [(i / N, j / N) for j in range(N + 1) for i in range(N + 1)]
    return pts, idx, st


def fmt(vs):
    return ", ".join("(" + ", ".join(f"{c:g}" for c in v) + ")" for v in vs)


pts, idx, st = grid(3.0)
plane = f"""    int[] faceVertexCounts = [{", ".join(["4"] * (N * N))}]
    int[] faceVertexIndices = [{", ".join(map(str, idx))}]
    point3f[] points = [{fmt(pts)}]
    texCoord2f[] primvars:st = [{fmt(st)}] (interpolation = "vertex")
    uniform token subdivisionScheme = "catmullClark"
"""

text = f'''#usda 1.0
(
    upAxis = "Y"
    metersPerUnit = 1
)

# Displacement, best at Complexity High or Very High (the planes are 16 x 16
# quads). Left: UsdPreviewSurface displacement from a height map (bumps.png,
# round bumps on five cells of a 3 x 3 checker) times 0.6. Middle: the same
# map through a MaterialX displacement shader (ND_displacement_float, scale
# 0.6). Right: a cube displaced outwards by a constant 0.25.
# Written by make_displacement.py.

def Camera "cam"
{{
    float focalLength = 24
    float horizontalAperture = 36
    float verticalAperture = 18
    double3 xformOp:translate = (0, 4.5, 8.5)
    float xformOp:rotateX = -28
    uniform token[] xformOpOrder = ["xformOp:translate", "xformOp:rotateX"]
}}

def DistantLight "sun"
{{
    float inputs:intensity = 3
    bool inputs:normalize = 1
    float3 xformOp:rotateXYZ = (-35, -50, 0)
    uniform token[] xformOpOrder = ["xformOp:rotateXYZ"]
}}

def DomeLight "sky"
{{
    float inputs:intensity = 0.3
}}

def Scope "mtl"
{{
    def Material "preview"
    {{
        token outputs:surface.connect = </mtl/preview/pbr.outputs:surface>
        token outputs:displacement.connect = </mtl/preview/pbr.outputs:displacement>
        def Shader "pbr"
        {{
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.75, 0.5, 0.3)
            float inputs:roughness = 0.5
            float inputs:displacement.connect = </mtl/preview/height.outputs:r>
            token outputs:surface
            token outputs:displacement
        }}
        def Shader "height"
        {{
            uniform token info:id = "UsdUVTexture"
            asset inputs:file = @./textures/bumps.png@
            token inputs:sourceColorSpace = "raw"
            float4 inputs:scale = (0.6, 0.6, 0.6, 1)
            float2 inputs:st.connect = </mtl/preview/st.outputs:result>
            float outputs:r
        }}
        def Shader "st"
        {{
            uniform token info:id = "UsdPrimvarReader_float2"
            string inputs:varname = "st"
            float2 outputs:result
        }}
    }}

    def Material "mtlx"
    {{
        token outputs:mtlx:surface.connect = </mtl/mtlx/surface.outputs:out>
        token outputs:mtlx:displacement.connect = </mtl/mtlx/displacement.outputs:out>
        def Shader "surface"
        {{
            uniform token info:id = "ND_standard_surface_surfaceshader"
            color3f inputs:base_color = (0.3, 0.5, 0.75)
            float inputs:specular_roughness = 0.5
            token outputs:out
        }}
        def Shader "displacement"
        {{
            uniform token info:id = "ND_displacement_float"
            float inputs:displacement.connect = </mtl/mtlx/height.outputs:out>
            float inputs:scale = 0.6
            token outputs:out
        }}
        def Shader "height"
        {{
            uniform token info:id = "ND_image_float"
            asset inputs:file = @./textures/bumps.png@
            float outputs:out
        }}
    }}

    def Material "inflate"
    {{
        token outputs:surface.connect = </mtl/inflate/pbr.outputs:surface>
        token outputs:displacement.connect = </mtl/inflate/pbr.outputs:displacement>
        def Shader "pbr"
        {{
            uniform token info:id = "UsdPreviewSurface"
            color3f inputs:diffuseColor = (0.4, 0.7, 0.4)
            float inputs:displacement = 0.25
            token outputs:surface
            token outputs:displacement
        }}
    }}
}}

def Mesh "ground"
{{
    int[] faceVertexCounts = [4]
    int[] faceVertexIndices = [0, 1, 2, 3]
    point3f[] points = [(-8, -0.01, -6), (8, -0.01, -6), (8, -0.01, 6), (-8, -0.01, 6)]
    uniform token subdivisionScheme = "none"
    color3f[] primvars:displayColor = [(0.5, 0.5, 0.5)]
}}

def Mesh "previewPlane" (prepend apiSchemas = ["MaterialBindingAPI"])
{{
{plane}    rel material:binding = </mtl/preview>
    double3 xformOp:translate = (-3.4, 0, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}}

def Mesh "mtlxPlane" (prepend apiSchemas = ["MaterialBindingAPI"])
{{
{plane}    rel material:binding = </mtl/mtlx>
}}

def Mesh "inflated" (prepend apiSchemas = ["MaterialBindingAPI"])
{{
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
    point3f[] points = [(-0.8, -0.8, 0.8), (0.8, -0.8, 0.8), (-0.8, 0.8, 0.8), (0.8, 0.8, 0.8), (-0.8, 0.8, -0.8), (0.8, 0.8, -0.8), (-0.8, -0.8, -0.8), (0.8, -0.8, -0.8)]
    rel material:binding = </mtl/inflate>
    double3 xformOp:translate = (3.4, 1.05, 0)
    uniform token[] xformOpOrder = ["xformOp:translate"]
}}
'''
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "displacement.usda"), "w").write(text)
