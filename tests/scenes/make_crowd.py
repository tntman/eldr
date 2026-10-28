"""Writes a crowd stress test: N instanced humanoid agents (13 joints, ~3000
quads) walking in place with ten varied, phase-offset walk cycles.

  usdenv python tests\\scenes\\make_crowd.py crowd.usdc 20000

Every agent is an instanceable SkelRoot referencing one agent definition,
with its own skel:animationSource (the usual layout of exported crowds).

  usdenv python tests\\scenes\\make_crowd.py crowd50.usda 50 houdini

"houdini" writes Houdini's agent export instead: an Xform per agent holding its
own SkelAnimation (and skel:animationSource) above an instanceable SkelRoot
"geometry", whose skeleton has a __locomotion__ joint the mesh's skel:joints
leave out (so the mesh maps its joints)."""
import math
import sys

from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade, UsdSkel, Vt

FRAMES = 48
# name, parent index, rest position (world, metres)
JOINTS = [
    ("hips", -1, (0, 1.0, 0)),
    ("spine", 0, (0, 1.15, 0)),
    ("chest", 1, (0, 1.35, 0)),
    ("neck", 2, (0, 1.55, 0)),
    ("head", 3, (0, 1.65, 0)),
    ("lShoulder", 2, (0.2, 1.48, 0)),
    ("lElbow", 5, (0.22, 1.2, 0)),
    ("rShoulder", 2, (-0.2, 1.48, 0)),
    ("rElbow", 7, (-0.22, 1.2, 0)),
    ("lHip", 0, (0.1, 0.95, 0)),
    ("lKnee", 9, (0.1, 0.52, 0)),
    ("rHip", 0, (-0.1, 0.95, 0)),
    ("rKnee", 11, (-0.1, 0.52, 0)),
]
# Tubes: start, end, radius, sides, rings, joint, parent joint blended at the start
SEGMENTS = [
    ((0, 0.92, 0), (0, 1.55, 0), 0.15, 24, 30, None, None),  # torso: hips/spine/chest by height
    ((0, 1.55, 0), (0, 1.85, 0), 0.10, 24, 16, 4, 3),
    ((0.2, 1.48, 0), (0.22, 1.2, 0), 0.05, 16, 14, 5, 2),
    ((0.22, 1.2, 0), (0.24, 0.95, 0), 0.04, 16, 14, 6, 5),
    ((-0.2, 1.48, 0), (-0.22, 1.2, 0), 0.05, 16, 14, 7, 2),
    ((-0.22, 1.2, 0), (-0.24, 0.95, 0), 0.04, 16, 14, 8, 7),
    ((0.1, 0.95, 0), (0.1, 0.52, 0), 0.07, 16, 16, 9, 0),
    ((0.1, 0.52, 0), (0.1, 0.08, 0), 0.05, 16, 16, 10, 9),
    ((-0.1, 0.95, 0), (-0.1, 0.52, 0), 0.07, 16, 16, 11, 0),
    ((-0.1, 0.52, 0), (-0.1, 0.08, 0), 0.05, 16, 16, 12, 11),
]


def joint_paths():
    paths = []
    for name, parent, _ in JOINTS:
        paths.append(name if parent < 0 else paths[parent] + "/" + name)
    return paths


def local_translation(i):
    _, parent, p = JOINTS[i]
    q = (0, 0, 0) if parent < 0 else JOINTS[parent][2]
    return Gf.Vec3f(p[0] - q[0], p[1] - q[1], p[2] - q[2])


def build_mesh():
    points, counts, indices, joint_idx, joint_w = [], [], [], [], []
    for start, end, radius, sides, rings, joint, parent in SEGMENTS:
        a, b = Gf.Vec3d(*start), Gf.Vec3d(*end)
        axis = (b - a).GetNormalized()
        side = Gf.Cross(axis, Gf.Vec3d(0, 0, 1)).GetNormalized()
        up = Gf.Cross(side, axis)
        base = len(points)
        for r in range(rings):
            t = r / (rings - 1)
            center = a + (b - a) * t
            for s in range(sides):
                ang = 2 * math.pi * s / sides
                points.append(Gf.Vec3f(center + (side * math.cos(ang) + up * math.sin(ang)) * radius))
                if joint is None:  # torso: hips -> spine -> chest along its height
                    y = center[1]
                    if y < 1.15:
                        w = (y - 0.92) / 0.23
                        joint_idx += [0, 1]
                        joint_w += [1 - w, w]
                    else:
                        w = min((y - 1.15) / 0.2, 1.0)
                        joint_idx += [1, 2]
                        joint_w += [1 - w, w]
                else:  # rigid, blended with the parent over the first fifth
                    w = min(t / 0.2, 1.0)
                    joint_idx += [parent, joint]
                    joint_w += [1 - w, w]
        for r in range(rings - 1):
            for s in range(sides):
                i0 = base + r * sides + s
                i1 = base + r * sides + (s + 1) % sides
                counts.append(4)
                indices += [i0, i1, i1 + sides, i0 + sides]
    return points, counts, indices, joint_idx, joint_w


def define_agent(stage, path, locomotion=False):
    UsdSkel.Root.Define(stage, path)
    skel = UsdSkel.Skeleton.Define(stage, path.AppendChild("skel"))
    joints = joint_paths()
    binds = [Gf.Matrix4d().SetTranslate(Gf.Vec3d(*p)) for _, _, p in JOINTS]
    rests = [Gf.Matrix4d().SetTranslate(Gf.Vec3d(local_translation(i))) for i in range(len(JOINTS))]
    if locomotion:  # a second root, second in order (as Houdini writes it)
        joints.insert(1, "__locomotion__")
        binds.insert(1, Gf.Matrix4d(1))
        rests.insert(1, Gf.Matrix4d(1))
    skel.CreateJointsAttr(joints)
    skel.CreateBindTransformsAttr(Vt.Matrix4dArray(binds))
    skel.CreateRestTransformsAttr(Vt.Matrix4dArray(rests))
    points, counts, indices, joint_idx, joint_w = build_mesh()
    mesh = UsdGeom.Mesh.Define(stage, path.AppendChild("body"))
    mesh.CreatePointsAttr(points)
    mesh.CreateFaceVertexCountsAttr(counts)
    mesh.CreateFaceVertexIndicesAttr(indices)
    mesh.CreateExtentAttr([(-0.6, 0, -0.6), (0.6, 1.9, 0.6)])
    mesh.CreateSubdivisionSchemeAttr(UsdGeom.Tokens.catmullClark)
    binding = UsdSkel.BindingAPI.Apply(mesh.GetPrim())
    binding.CreateSkeletonRel().SetTargets([skel.GetPath()])
    binding.CreateJointIndicesPrimvar(False, 2).Set(joint_idx)
    binding.CreateJointWeightsPrimvar(False, 2).Set(joint_w)
    binding.CreateGeomBindTransformAttr(Gf.Matrix4d(1))
    if locomotion:
        binding.CreateJointsAttr(joint_paths())
    return len(counts)


def rot(axis, degrees):
    return Gf.Quatf(Gf.Rotation(Gf.Vec3d(*axis), degrees).GetQuat())


def define_walk(stage, path, k):
    """Walk cycle k of 10: varied stride, arm swing, knee bend, twist and phase."""
    stride = 22 + 3 * (k % 4)
    arms = 15 + 4 * ((k * 3) % 5)
    knee = 35 + 5 * (k % 3)
    twist = 3 + (k % 5)
    phase = 2 * math.pi * k / 10
    anim = UsdSkel.Animation.Define(stage, path)
    anim.CreateJointsAttr(joint_paths())
    anim.CreateScalesAttr(Vt.Vec3hArray([(1, 1, 1)] * len(JOINTS)))
    translations, rotations = anim.CreateTranslationsAttr(), anim.CreateRotationsAttr()
    for f in range(FRAMES):
        w = 2 * math.pi * f / FRAMES + phase
        t = [local_translation(i) for i in range(len(JOINTS))]
        t[0] = Gf.Vec3f(0, 1.0 + 0.03 * abs(math.sin(w)), 0)
        r = [Gf.Quatf(1)] * len(JOINTS)
        r[0] = rot((0, 1, 0), twist * math.sin(w))
        r[2] = rot((0, 1, 0), -2 * twist * math.sin(w))
        r[4] = rot((1, 0, 0), 4 * math.sin(2 * w))
        r[5] = rot((1, 0, 0), -arms * math.sin(w))
        r[6] = rot((1, 0, 0), -10 - 10 * max(0.0, -math.sin(w)))
        r[7] = rot((1, 0, 0), arms * math.sin(w))
        r[8] = rot((1, 0, 0), -10 - 10 * max(0.0, math.sin(w)))
        r[9] = rot((1, 0, 0), stride * math.sin(w))
        r[10] = rot((1, 0, 0), knee * max(0.0, math.sin(w + math.pi / 2)))
        r[11] = rot((1, 0, 0), -stride * math.sin(w))
        r[12] = rot((1, 0, 0), knee * max(0.0, math.sin(w - math.pi / 2)))
        translations.Set(Vt.Vec3fArray(t), Usd.TimeCode(f))
        rotations.Set(Vt.QuatfArray(r), Usd.TimeCode(f))


def main():
    out = sys.argv[1]
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
    houdini = len(sys.argv) > 3 and sys.argv[3] == "houdini"
    columns = int(math.ceil(math.sqrt(count * 2)))  # twice as wide as deep
    rows = int(math.ceil(count / columns))
    spacing = 1.0
    stage = Usd.Stage.CreateNew(out)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    UsdGeom.SetStageMetersPerUnit(stage, 1)
    stage.SetStartTimeCode(0)
    stage.SetEndTimeCode(FRAMES - 1)
    stage.SetTimeCodesPerSecond(24)

    width, depth = columns * spacing, rows * spacing
    cam = UsdGeom.Camera.Define(stage, "/cam")
    cam.CreateFocalLengthAttr(24)
    cam.CreateClippingRangeAttr(Gf.Vec2f(0.5, 5000))
    cam.AddTranslateOp().Set(Gf.Vec3d(0, depth * 0.35 + 4, depth * 0.2 + 8))
    cam.AddRotateXOp().Set(-30.0)
    sun = UsdLux.DistantLight.Define(stage, "/sun")
    sun.CreateIntensityAttr(3.0)
    sun.CreateNormalizeAttr(True)
    sun.AddRotateXYZOp().Set(Gf.Vec3f(-50, 35, 0))
    UsdLux.DomeLight.Define(stage, "/sky").CreateIntensityAttr(0.4)
    floor = UsdGeom.Mesh.Define(stage, "/floor")
    floor.CreatePointsAttr([(-width, 0, -depth * 2), (-width, 0, depth), (width, 0, depth), (width, 0, -depth * 2)])
    floor.CreateFaceVertexCountsAttr([4])
    floor.CreateFaceVertexIndicesAttr([0, 1, 2, 3])

    quads = define_agent(stage, Sdf.Path("/AgentDef"), locomotion=houdini)
    stage.GetPrimAtPath("/AgentDef").SetSpecifier(Sdf.SpecifierClass)
    anims = []
    for k in range(0 if houdini else 10):
        p = Sdf.Path(f"/Anims/walk{k}")
        define_walk(stage, p, k)
        anims.append(p)
    crowd = stage.DefinePrim("/Crowd", "Xform")
    for i in range(count):
        x = (i % columns - (columns - 1) / 2) * spacing
        z = -(i // columns) * spacing
        agent = stage.DefinePrim(f"/Crowd/agent{i}", "Xform" if houdini else "SkelRoot")
        geometry = stage.DefinePrim(agent.GetPath().AppendChild("geometry"), "SkelRoot") if houdini else agent
        geometry.GetReferences().AddInternalReference("/AgentDef")
        geometry.SetInstanceable(True)
        xf = UsdGeom.Xformable(agent)
        xf.AddTranslateOp().Set(Gf.Vec3d(x, 0, z))
        xf.AddRotateYOp().Set(float((i * 37) % 60 - 30))
        anim = anims[(i * 7) % 10] if not houdini else agent.GetPath().AppendChild("animation")
        if houdini:
            define_walk(stage, anim, (i * 7) % 10)
        UsdSkel.BindingAPI.Apply(agent).CreateAnimationSourceRel().SetTargets([anim])
    stage.GetRootLayer().Save()
    print(f"agents: {count}, quads per agent: {quads}, joints: {len(JOINTS)}")


main()
