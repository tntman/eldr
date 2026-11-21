"""Write variants/complex_caustics.usda: a lathed wine glass and a rippled glass slab under a small
sphere light, both caustic casters. CC0, like the other variants.

  python tests/scenes/cornell/make_complex_caustics.py

Cornell data space: metres, Y up, the floor at y = 0, the back wall at z = 5.59; +x is screen left.
"""
import math
import os

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "variants", "complex_caustics.usda")

# Rippled slab: x, z extent, flat bottom and mean top height, grid step; waves (peak curvature A k^2 in 1/m,
# wavelength, direction in degrees, phase). A crest of curvature c focuses like a lens of f = 1 / (0.5 c).
SLAB = dict(x=(0.35, 2.75), z=(2.2, 4.6), bottom=1.55, top=1.7, step=0.025)
WAVES = [(0.8, 0.48, 10, 0.0), (0.8, 0.41, 70, 1.3), (0.8, 0.35, 130, 2.1), (0.8, 0.31, 165, 0.7)]
# Wine glass: base centre, bowl radius, rim/stem heights, wall thickness, segments around.
GLASS = dict(x=4.0, z=3.4, bowl=0.42, stem_r=0.035, foot_r=0.36, foot_h=0.03, bowl_y=0.70, rim_y=1.65,
             inner_y=0.78, wall=0.016, segments=72)
LIGHT = dict(pos=(2.6, 5.0, 5.1), radius=0.05, exposure=11.0)
CLAMP = 0.05  # the casters' caustics roughness clamp (Karma's default 0.2 blurs the web)


def fmt(v):
    return "(%s)" % ", ".join("%.5g" % c for c in v)


def norm(v):
    s = math.sqrt(sum(c * c for c in v))
    return tuple(c / s for c in v)


def height(x, z):
    """Slab top offset and its gradient (dh/dx, dh/dz)."""
    h = gx = gz = 0.0
    for c, wl, deg, ph in WAVES:
        k = 2 * math.pi / wl
        a = c / (k * k)
        kx, kz = k * math.cos(math.radians(deg)), k * math.sin(math.radians(deg))
        s = kx * x + kz * z + ph
        h += a * math.sin(s)
        gx += a * kx * math.cos(s)
        gz += a * kz * math.cos(s)
    return h, gx, gz


def slab():
    """[(name, points, normals or None, counts, indices)] of the slab's top, bottom and sides."""
    (x0, x1), (z0, z1), yb, yt = SLAB["x"], SLAB["z"], SLAB["bottom"], SLAB["top"]
    nx, nz = int(round((x1 - x0) / SLAB["step"])), int(round((z1 - z0) / SLAB["step"]))
    xs = [x0 + (x1 - x0) * i / nx for i in range(nx + 1)]
    zs = [z0 + (z1 - z0) * j / nz for j in range(nz + 1)]
    top = lambda x, z: (x, yt + height(x, z)[0], z)
    pts, nrm = [], []
    for z in zs:
        for x in xs:
            h, gx, gz = height(x, z)
            pts.append((x, yt + h, z))
            nrm.append(norm((-gx, 1.0, -gz)))
    w = nx + 1
    # Counter-clockwise seen from above (+y): (i, j), (i, j+1), (i+1, j+1), (i+1, j) in x right, z down.
    idx = []
    for j in range(nz):
        for i in range(nx):
            a = j * w + i
            idx += [a, a + w, a + w + 1, a + 1]
    out = [("slab_top", pts, nrm, [4] * (nx * nz), idx)]
    out.append(("slab_bottom", [(x0, yb, z0), (x1, yb, z0), (x1, yb, z1), (x0, yb, z1)], None, [4], [0, 1, 2, 3]))
    # Sides: each edge of the top grid down to the bottom, outward.
    side_pts, side_idx = [], []
    edges = [[(x, z0) for x in xs], [(x1, z) for z in zs], [(x, z1) for x in reversed(xs)], [(x0, z) for z in reversed(zs)]]
    for e in edges:  # walking counter-clockwise seen from above, so the outside is on the right
        for (ax, az), (bx, bz) in zip(e, e[1:]):
            n = len(side_pts)
            side_pts += [(ax, yb, az), (bx, yb, bz), top(bx, bz), top(ax, az)]
            side_idx += [n, n + 3, n + 2, n + 1]
    out.append(("slab_sides", side_pts, None, [4] * (len(side_pts) // 4), side_idx))
    return out


def bowl_shape(s):
    """Bowl radius over the bowl's height (0 bottom, 1 rim), as a fraction of the widest radius."""
    if s < 0.6:
        return math.sqrt(max(0.0, 1.0 - (1.0 - s / 0.6) ** 2))
    return 1.0 - 0.12 * ((s - 0.6) / 0.4) ** 2


def profile():
    """The glass's closed profile, from the foot's centre underneath to the bowl's inner centre, as
    pieces of (r, y) points; corners start a new piece so they stay sharp."""
    g = GLASS
    R, t, rs, rf, fh = g["bowl"], g["wall"], g["stem_r"], g["foot_r"], g["foot_h"]
    yb, yr, yi = g["bowl_y"], g["rim_y"], g["inner_y"]
    n = 48
    s = [(i / n) ** 1.5 for i in range(n + 1)]  # finer near the bottom, where the bowl curves most
    outer = [(R * bowl_shape(u), yb + (yr - yb) * u) for u in s]
    outer = [p for p in outer if p[0] > rs]
    s_join = 0.6 * (1 - math.sqrt(1 - (rs / R) ** 2))  # where the bowl's outside reaches the stem's radius
    y_join = yb + (yr - yb) * s_join
    inner = [((R - t) * bowl_shape(u), yi + (yr - yi) * u) for u in reversed(s)]
    return [[(0.0, 0.0), (rf, 0.0)], [(rf, 0.0), (rf, fh)], [(rf, fh), (rs, fh)], [(rs, fh), (rs, y_join)],
            [(rs, y_join)] + outer, [outer[-1], inner[0]], inner]


def lathe():
    g = GLASS
    seg = g["segments"]
    pts, nrm, counts, idx = [], [], [], []
    for piece in profile():
        # Profile normals: the tangent turned clockwise (the solid is on the left walking the profile).
        tang = []
        for i, (r, y) in enumerate(piece):
            a, b = piece[max(i - 1, 0)], piece[min(i + 1, len(piece) - 1)]
            tang.append(norm((b[0] - a[0], b[1] - a[1])))
        rings = []
        for (r, y), (tr, ty) in zip(piece, tang):
            nr, ny = ty, -tr
            if r < 1e-9:
                rings.append([len(pts)])
                pts.append((g["x"], y, g["z"]))
                nrm.append(norm((0.0, ny, 0.0)) if abs(ny) > 1e-9 else (0.0, 1.0, 0.0))
                continue
            ring = []
            for k in range(seg):
                a = 2 * math.pi * k / seg
                ring.append(len(pts))
                pts.append((g["x"] + r * math.cos(a), y, g["z"] + r * math.sin(a)))
                nrm.append(norm((nr * math.cos(a), ny, nr * math.sin(a))))
            rings.append(ring)
        for ra, rb in zip(rings, rings[1:]):
            for k in range(seg):
                k1 = (k + 1) % seg
                # Counter-clockwise seen from outside.
                if len(ra) == 1:
                    counts.append(3)
                    idx += [ra[0], rb[k], rb[k1]]
                elif len(rb) == 1:
                    counts.append(3)
                    idx += [ra[k], rb[0], ra[k1]]
                else:
                    counts.append(4)
                    idx += [ra[k], rb[k], rb[k1], ra[k1]]
    return [("wine_glass", pts, nrm, counts, idx)]


def mesh_usda(name, pts, nrm, counts, idx, material):
    s = ['    def Mesh "%s" (prepend apiSchemas = ["MaterialBindingAPI"])' % name, "    {",
         "        int[] faceVertexCounts = [%s]" % ", ".join(map(str, counts)),
         "        int[] faceVertexIndices = [%s]" % ", ".join(map(str, idx)),
         "        point3f[] points = [%s]" % ", ".join(map(fmt, pts))]
    if nrm:
        s.append('        normal3f[] normals = [%s] (interpolation = "vertex")' % ", ".join(map(fmt, nrm)))
    s += ["        uniform token subdivisionScheme = \"none\"",
          "        rel material:binding = <%s>" % material,
          '        bool primvars:karma:object:causticsenable = 1 (interpolation = "constant")',
          '        float primvars:karma:object:causticsroughnessclamp = %g (interpolation = "constant")' % CLAMP, "    }"]
    return "\n".join(s)


def check(meshes):
    """Every closed piece encloses positive volume (faces point outward)."""
    for name, pts, _, counts, idx in meshes:
        vol, i = 0.0, 0
        for c in counts:
            f = [pts[j] for j in idx[i:i + c]]
            for a, b in zip(f[1:], f[2:]):
                o = f[0]
                vol += (o[0] * (a[1] * b[2] - a[2] * b[1]) - o[1] * (a[0] * b[2] - a[2] * b[0])
                        + o[2] * (a[0] * b[1] - a[1] * b[0])) / 6
            i += c
        yield name, vol


def main():
    s, w = slab(), lathe()
    slab_vol = sum(v for _, v in check(s))
    glass_vol = sum(v for _, v in check(w))
    (x0, x1), (z0, z1) = SLAB["x"], SLAB["z"]
    expect = (x1 - x0) * (z1 - z0) * (SLAB["top"] - SLAB["bottom"])
    assert abs(slab_vol - expect) < 0.02 * expect, (slab_vol, expect)
    assert 0 < glass_vol < 0.2, glass_vol
    lx, ly, lz = LIGHT["pos"]
    head = '''#usda 1.0
(
    doc = "Complex caustics: a wine glass and a rippled glass slab under a small sphere light, seen from above. The bowl focuses a bright cusp into its shadow and the ripples a web on the floor."
    defaultPrim = "cornell_box"
    metersPerUnit = 1
    upAxis = "Z"
    startTimeCode = 1
    endTimeCode = 1
    timeCodesPerSecond = 24
    renderSettingsPrimPath = "/Render/Settings"
    subLayers = [@../cornell.usda@]
)

# Written by tests/scenes/cornell/make_complex_caustics.py; edit that and run it again.

over "cornell_box"
{
    over "camera"
    {
        # Higher and looking down, so the floor fills the frame.
        double3 xformOp:translate = (2.78, 4.6, -1.8)
        float3 xformOp:rotateXYZ = (-40, 180, 0)
    }
    over "ceiling_light"
    {
        # A dim fill that throws no caustics; the sphere light makes them.
        float inputs:exposure = 1.5
        bool inputs:karma:light:contributescaustics = 0
    }
    def SphereLight "caustic_light"
    {
        float inputs:radius = %g
        float inputs:intensity = 1
        float inputs:exposure = %g
        color3f inputs:color = (1, 0.93, 0.82)
        double3 xformOp:translate = (%g, %g, %g)
        uniform token[] xformOpOrder = ["xformOp:translate"]
    }
''' % (LIGHT["radius"], LIGHT["exposure"], lx, ly, lz)
    boxes = ["tall_box_front", "tall_box_right", "tall_box_back", "tall_box_left", "tall_box_top",
             "short_box_back", "short_box_right", "short_box_front", "short_box_left", "short_box_top"]
    body = "".join('    over "%s" (active = false)\n    {\n    }\n' % b for b in boxes)
    body += '''    def Material "glass"
    {
        token outputs:mtlx:surface.connect = </cornell_box/glass/S.outputs:out>
        def Shader "S"
        {
            uniform token info:id = "ND_standard_surface_surfaceshader"
            float inputs:base = 0
            float inputs:specular_roughness = 0
            float inputs:specular_IOR = 1.5
            float inputs:transmission = 1
            color3f inputs:transmission_color = (1, 1, 1)
            token outputs:out
        }
    }
'''
    body += "\n".join(mesh_usda(*m, "/cornell_box/glass") for m in s + w) + "\n}\n"
    tail = '''
over "Render"
{
    over "Settings"
    {
        # Karma: the casters' Enable Caustics and a 0.05 roughness clamp (above), more samples for the small light seen through glass,
        # a colour limit that keeps the caustic paths, and enough refraction for the glass's four walls.
        int karma:global:pathtracedsamples = 2048
        float karma:global:colorlimit = 1000
        int karma:global:refractlimit = 8
    }
}
'''
    with open(OUT, "w", newline="\n") as f:
        f.write(head + body + tail)
    print(OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
