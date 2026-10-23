"""Writes UsdSkel test scenes: a bending tube with three joints.

  usdenv python tests\\scenes\\make_skel.py tests\\scenes\\skel.usda
  usdenv python tests\\scenes\\make_skel.py crowd.usdc 40

With a count N, writes an N x N crowd: every agent is an instance of one
agent definition with its own skel:animationSource (eight animations with
different phases), the usual layout of exported crowds."""
import math
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdSkel, Vt

FRAMES = 48
JOINTS = ["root", "root/mid", "root/mid/tip"]
JOINT_Y = [0.0, 0.7, 1.4]
RINGS, SIDES, HEIGHT, RADIUS = 13, 10, 2.1, 0.2


def define_agent(stage, path):
    root = UsdSkel.Root.Define(stage, path)
    skel = UsdSkel.Skeleton.Define(stage, path.AppendChild("skel"))
    skel.CreateJointsAttr(JOINTS)
    skel.CreateBindTransformsAttr(Vt.Matrix4dArray(
        [Gf.Matrix4d().SetTranslate(Gf.Vec3d(0, y, 0)) for y in JOINT_Y]))
    skel.CreateRestTransformsAttr(Vt.Matrix4dArray(
        [Gf.Matrix4d().SetTranslate(Gf.Vec3d(0, y, 0)) for y in (0.0, 0.7, 0.7)]))

    mesh = UsdGeom.Mesh.Define(stage, path.AppendChild("tube"))
    points, indices, weights = [], [], []
    for r in range(RINGS):
        y = HEIGHT * r / (RINGS - 1)
        # Blend between the two joints around this height.
        j = min(int(y / 0.7), 1)
        t = min(max((y - JOINT_Y[j]) / 0.7, 0.0), 1.0)
        for s in range(SIDES):
            a = 2 * math.pi * s / SIDES
            points.append(Gf.Vec3f(RADIUS * math.cos(a), y, RADIUS * math.sin(a)))
            indices += [j, j + 1]
            weights += [1 - t, t]
    counts, verts = [], []
    for r in range(RINGS - 1):
        for s in range(SIDES):
            a, b = r * SIDES + s, r * SIDES + (s + 1) % SIDES
            counts.append(4)
            verts += [a, a + SIDES, b + SIDES, b]
    mesh.CreatePointsAttr(points)
    mesh.CreateFaceVertexCountsAttr(counts)
    mesh.CreateFaceVertexIndicesAttr(verts)
    mesh.CreateExtentAttr([(-1, 0, -1), (1, HEIGHT, 1)])
    mesh.CreateDisplayColorAttr([(0.8, 0.45, 0.25)])
    binding = UsdSkel.BindingAPI.Apply(mesh.GetPrim())
    binding.CreateSkeletonRel().SetTargets([skel.GetPath()])
    binding.CreateJointIndicesPrimvar(False, 2).Set(indices)
    binding.CreateJointWeightsPrimvar(False, 2).Set(weights)
    binding.CreateGeomBindTransformAttr(Gf.Matrix4d(1))
    return root, skel


def define_animation(stage, path, phase):
    anim = UsdSkel.Animation.Define(stage, path)
    anim.CreateJointsAttr(JOINTS)
    anim.CreateTranslationsAttr(Vt.Vec3fArray([(0, 0, 0), (0, 0.7, 0), (0, 0.7, 0)]))
    anim.CreateScalesAttr(Vt.Vec3hArray([(1, 1, 1)] * 3))
    rotations = anim.CreateRotationsAttr()
    for f in range(FRAMES):
        w = 2 * math.pi * f / FRAMES + phase
        angles = [10 * math.sin(w), 45 * math.sin(w), 45 * math.sin(w - 1.0)]
        rotations.Set(Vt.QuatfArray([Gf.Quatf(Gf.Rotation(Gf.Vec3d(0, 0, 1), a).GetQuat()) for a in angles]),
                      Usd.TimeCode(f))
    return anim


def main():
    out = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    stage = Usd.Stage.CreateNew(out)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 1)
    stage.SetStartTimeCode(0)
    stage.SetEndTimeCode(FRAMES - 1)
    stage.SetTimeCodesPerSecond(24)

    # Elevated, aimed at the middle of the crowd (or the single agent).
    height, back, center = 1.5 + count * 0.25, 4 + count * 0.5, -(count - 1) * 0.6 if count else 0.0
    cam = UsdGeom.Camera.Define(stage, "/cam")
    cam.CreateFocalLengthAttr(24)
    cam.AddTranslateOp().Set(Gf.Vec3d(0, height, back))
    cam.AddRotateXOp().Set(-math.degrees(math.atan2(height - 1.0, back - center)))
    cam.GetClippingRangeAttr().Set(Gf.Vec2f(0.1, 1000))
    sun = UsdLux.DistantLight.Define(stage, "/sun")
    sun.CreateIntensityAttr(3.0)
    sun.CreateNormalizeAttr(True)
    sun.AddRotateXYZOp().Set(Gf.Vec3f(-50, 35, 0))
    UsdLux.DomeLight.Define(stage, "/sky").CreateIntensityAttr(0.3)
    floor = UsdGeom.Mesh.Define(stage, "/floor")
    size = 3 + count * 0.6
    floor.CreatePointsAttr([(-size, 0, -size), (-size, 0, size), (size, 0, size), (size, 0, -size)])
    floor.CreateFaceVertexCountsAttr([4])
    floor.CreateFaceVertexIndicesAttr([0, 1, 2, 3])

    if count == 0:
        _, skel = define_agent(stage, Sdf.Path("/agent"))
        anim = define_animation(stage, Sdf.Path("/agent/anim"), 0.0)
        UsdSkel.BindingAPI.Apply(skel.GetPrim()).CreateAnimationSourceRel().SetTargets([anim.GetPath()])
    else:
        define_agent(stage, Sdf.Path("/AgentDef"))
        stage.GetPrimAtPath("/AgentDef").SetSpecifier(Sdf.SpecifierClass)
        anims = [define_animation(stage, Sdf.Path(f"/Anims/anim{k}"), k * 0.8).GetPath() for k in range(8)]
        for i in range(count * count):
            x, z = (i % count - (count - 1) / 2) * 1.2, -(i // count) * 1.2
            agent = stage.DefinePrim(f"/Crowd/agent{i}", "SkelRoot")
            agent.GetReferences().AddInternalReference("/AgentDef")
            agent.SetInstanceable(True)
            UsdGeom.Xformable(agent).AddTranslateOp().Set(Gf.Vec3d(x, 0, z))
            binding = UsdSkel.BindingAPI.Apply(agent)
            binding.CreateAnimationSourceRel().SetTargets([anims[i % len(anims)]])
    stage.GetRootLayer().Save()
    print("agents:", max(count * count, 1))


main()
