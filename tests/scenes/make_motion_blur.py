"""Writes tests/scenes/motion_blur.usda: every kind of motion blur in one
view, to compare Eldr against Karma at frame 24.

  usdenv python tests\\scenes\\make_motion_blur.py tests\\scenes\\motion_blur.usda

Rows, top to bottom (each object is named for what moves it):
  transform   translate, rotate (time-sampled xformOps)
  mesh        time-sampled points (shear), velocities only, velocities +
              accelerations, both (samples shear along x, velocities point
              up y: the blur's direction shows which one the renderer used),
              a Catmull-Clark mesh with time-sampled points
  points      Points and BasisCurves, time-sampled or with velocities only
  instancer   PointInstancer: time-sampled positions, velocities only,
              angularVelocities only, its own transform moving
  skinned     skel.usda's agent (joints)
Everything moves 0.5 units (or 15 degrees) a frame through frame 24; the
cameras' shutter is open half a frame (-0.25 to 0.25). /camMoving pans for
camera blur. Karma uses velocities only where Render Geometry Settings'
Velocity Blur (primvars:karma:object:vblur, instance_vblur) asks; nothing
here authors it."""
import math
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, Vt

FRAMES = range(0, 49)
MID = 24
SPEED = 0.5    # units a frame
FPS = 24.0
SPIN = 15.0    # degrees a frame
COLUMNS = [-7.5, -4.5, -1.5, 1.5, 4.5, 7.5]
ROWS = {"transform": 6.0, "mesh": 3.0, "points": 0.0, "instancer": -3.0, "skinned": -6.0}


def grid(n=8, size=2.0):
    """An n x n quad patch in XY, centred, `size` wide; (points, counts, indices)."""
    pts = [Gf.Vec3f(size * (i / n - 0.5), size * (j / n - 0.5), 0) for j in range(n + 1) for i in range(n + 1)]
    counts, idx = [], []
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            counts.append(4)
            idx += [a, a + 1, a + n + 2, a + n + 1]
    return pts, counts, idx


def shear(pts, t):
    """The top moves SPEED a frame along x, the bottom stays: a deformation."""
    return Vt.Vec3fArray([p + Gf.Vec3f(SPEED * (t - MID) * (p[1] + 1) / 2, 0, 0) for p in pts])


def shear_velocities(pts, axis=0):
    return Vt.Vec3fArray([Gf.Vec3f(*[SPEED * FPS * (p[1] + 1) / 2 if k == axis else 0 for k in range(3)])
                          for p in pts])


def place(stage, path, column, row):
    xf = UsdGeom.Xform.Define(stage, path)
    xf.AddTranslateOp().Set(Gf.Vec3d(COLUMNS[column], ROWS[row], 0))
    return xf


def mesh(stage, path, sampled=True, velocities=False, accelerations=False, both=False, subdiv=False):
    pts, counts, idx = grid()
    m = UsdGeom.Mesh.Define(stage, path)
    m.CreateFaceVertexCountsAttr(counts)
    m.CreateFaceVertexIndicesAttr(idx)
    m.CreateSubdivisionSchemeAttr("catmullClark" if subdiv else "none")
    m.CreateDoubleSidedAttr(True)
    if sampled:
        for t in FRAMES:
            m.GetPointsAttr().Set(shear(pts, t), t)
    else:
        m.CreatePointsAttr(shear(pts, MID))
    if velocities:
        m.CreateVelocitiesAttr(shear_velocities(pts, axis=1 if both else 0))
    if accelerations:
        # Curving the top's path downwards.
        m.CreateAccelerationsAttr(Vt.Vec3fArray([Gf.Vec3f(0, -SPEED * FPS * FPS * (p[1] + 1), 0) for p in pts]))
    return m


def cube(stage, path, size=1.0):
    c = UsdGeom.Cube.Define(stage, path)
    c.CreateSizeAttr(size)
    return c


def main(out):
    stage = Usd.Stage.CreateNew(out)
    stage.SetStartTimeCode(FRAMES[0])
    stage.SetEndTimeCode(FRAMES[-1])
    stage.SetTimeCodesPerSecond(FPS)
    stage.SetFramesPerSecond(FPS)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 1)
    stage.SetMetadata("comment", __doc__.split("\n\n", 1)[1])

    for name, pan in (("cam", False), ("camMoving", True)):
        cam = UsdGeom.Camera.Define(stage, "/" + name)
        cam.CreateFocalLengthAttr(24)
        cam.CreateClippingRangeAttr(Gf.Vec2f(0.1, 1000))
        cam.CreateShutterOpenAttr(-0.25)
        cam.CreateShutterCloseAttr(0.25)
        op = cam.AddTranslateOp()
        if pan:
            for t in FRAMES:
                op.Set(Gf.Vec3d(SPEED * (t - MID), 0, 22), t)
        else:
            op.Set(Gf.Vec3d(0, 0, 22))

    sun = UsdLux.DistantLight.Define(stage, "/sun")
    sun.CreateIntensityAttr(8)
    sun.AddRotateXYZOp().Set(Gf.Vec3f(-20, 15, 0))
    UsdLux.DomeLight.Define(stage, "/sky").CreateIntensityAttr(1)
    wall = UsdGeom.Mesh.Define(stage, "/wall")
    wall.CreatePointsAttr([(-12, -9, -1.5), (12, -9, -1.5), (12, 9, -1.5), (-12, 9, -1.5)])
    wall.CreateFaceVertexCountsAttr([4])
    wall.CreateFaceVertexIndicesAttr([0, 1, 2, 3])
    wall.CreateSubdivisionSchemeAttr("none")

    # Transform blur.
    UsdGeom.Xform.Define(stage, "/transform")
    xf = UsdGeom.Xform.Define(stage, "/transform/translate")
    op = xf.AddTranslateOp()
    for t in FRAMES:
        op.Set(Gf.Vec3d(COLUMNS[0] + SPEED * (t - MID), ROWS["transform"], 0), t)
    cube(stage, "/transform/translate/cube")
    xf = UsdGeom.Xform.Define(stage, "/transform/rotate")
    xf.AddTranslateOp().Set(Gf.Vec3d(COLUMNS[1], ROWS["transform"], 0))
    op = xf.AddRotateZOp()
    for t in FRAMES:
        op.Set(SPIN * (t - MID), t)
    cube(stage, "/transform/rotate/cube", 1.5)

    # Deformation blur.
    for column, name, kw in ((0, "pointsSampled", {}),
                             (1, "velocitiesOnly", dict(sampled=False, velocities=True)),
                             (2, "velocitiesAccelerations", dict(sampled=False, velocities=True, accelerations=True)),
                             (3, "sampledAndVelocitiesUp", dict(velocities=True, both=True)),
                             (4, "subdivSampled", dict(subdiv=True))):
        place(stage, "/mesh_" + name, column, "mesh")
        mesh(stage, "/mesh_%s/mesh" % name, **kw)

    # Points and curves.
    pts, _, _ = grid(4)
    for column, name, sampled in ((0, "pointsSampled", True), (1, "pointsVelocities", False)):
        place(stage, "/" + name, column, "points")
        p = UsdGeom.Points.Define(stage, "/%s/points" % name)
        p.CreateWidthsAttr(Vt.FloatArray([0.25] * len(pts)))
        if sampled:
            for t in FRAMES:
                p.GetPointsAttr().Set(shear(pts, t), t)
        else:
            p.CreatePointsAttr(shear(pts, MID))
            p.CreateVelocitiesAttr(shear_velocities(pts))
    for column, name, sampled in ((2, "curvesSampled", True), (3, "curvesVelocities", False)):
        place(stage, "/" + name, column, "points")
        c = UsdGeom.BasisCurves.Define(stage, "/%s/curves" % name)
        c.CreateTypeAttr("linear")
        line = [Gf.Vec3f(x * 0.5 - 1, y * 0.25 - 1, 0) for x in range(5) for y in range(9)]
        c.CreateCurveVertexCountsAttr([9] * 5)
        c.CreateWidthsAttr(Vt.FloatArray([0.08]))
        c.SetWidthsInterpolation(UsdGeom.Tokens.constant)
        if sampled:
            for t in FRAMES:
                c.GetPointsAttr().Set(shear(line, t), t)
        else:
            c.CreatePointsAttr(shear(line, MID))
            c.CreateVelocitiesAttr(shear_velocities(line))

    # Instances.
    corners = [Gf.Vec3f(x, y, 0) for y in (-0.6, 0.6) for x in (-0.6, 0.6)]
    for column, name in enumerate(("positionsSampled", "velocities", "angularVelocities", "instancerTransform")):
        place(stage, "/inst_" + name, column, "instancer")
        pi = UsdGeom.PointInstancer.Define(stage, "/inst_%s/instancer" % name)
        cube(stage, "/inst_%s/instancer/proto" % name, 0.6)
        pi.CreatePrototypesRel().SetTargets([Sdf.Path("/inst_%s/instancer/proto" % name)])
        pi.CreateProtoIndicesAttr([0] * 4)
        pi.CreateOrientationsAttr(Vt.QuathArray([Gf.Quath(1, 0, 0, 0)] * 4))
        if name == "positionsSampled":
            for t in FRAMES:
                pi.CreatePositionsAttr().Set(Vt.Vec3fArray([p + Gf.Vec3f(SPEED * (t - MID), 0, 0) for p in corners]), t)
        else:
            pi.CreatePositionsAttr(Vt.Vec3fArray(corners))
        if name == "velocities":
            pi.CreateVelocitiesAttr(Vt.Vec3fArray([Gf.Vec3f(SPEED * FPS, 0, 0)] * 4))
        if name == "angularVelocities":
            pi.CreateAngularVelocitiesAttr(Vt.Vec3fArray([Gf.Vec3f(0, 0, SPIN * FPS)] * 4))  # degrees a second
        if name == "instancerTransform":
            op = pi.AddTranslateOp()
            for t in FRAMES:
                op.Set(Gf.Vec3d(SPEED * (t - MID), 0, 0), t)

    # Skinned: skel.usda's bending tube (joints).
    place(stage, "/skinned", 2, "skinned")
    agent = stage.DefinePrim("/skinned/agent")
    agent.GetReferences().AddReference("./skel.usda", "/agent")

    # Light orange movers on a dark wall: the blur reads as a gradient.
    for prim in stage.Traverse():
        if prim.IsA(UsdGeom.Gprim) and not prim.GetPath().HasPrefix("/skinned"):
            colour = (0.15, 0.15, 0.17) if prim.GetName() == "wall" else (0.9, 0.55, 0.25)
            UsdGeom.Gprim(prim).CreateDisplayColorAttr([Gf.Vec3f(*colour)])
    stage.GetRootLayer().Save()


if __name__ == "__main__":
    main(sys.argv[1])
