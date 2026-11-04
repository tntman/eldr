"""Render a USD scene with Blender's EEVEE, the raster reference beside Karma.
Imports the stage, looks through a USD camera and writes a linear EXR.
Run: blender -b --factory-startup -P tests/eevee_render.py -- scene.usda out.exr
     [--settings /Render/karma] [--camera /cameras/cam] [--size 960x540] [--samples 64]
     [--rt on|off] [--probes auto|none]
--settings takes the camera and resolution from a RenderSettings prim (--camera and --size
override them). --rt on: EEVEE's screen-space ray tracing (reflections, refraction, fast GI);
off: light probes and the world only (EEVEE's raster fallback). --probes auto (default) adds
a plane probe (planar reflection) on each flat glossy mesh, as an EEVEE artist would (EEVEE
uses them in its screen tracing only).
Blender's importer reads UsdPreviewSurface only and converts lights in its own units, so this
script sets from the stage: MaterialX standard_surface / open_pbr / ND_UsdPreviewSurface
materials (Principled BSDF), bindings on shape prims (Sphere, Cube, ...) and their smoothness,
light power in Karma's units, and the dome as the world. Scene units stay as authored (Karma
ignores metersPerUnit too)."""
import argparse
import math
import os
import subprocess
import sys

import bpy

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
parser = argparse.ArgumentParser()
parser.add_argument("usd")
parser.add_argument("out")
parser.add_argument("--settings")
parser.add_argument("--camera")
parser.add_argument("--size")
parser.add_argument("--samples", type=int, default=64)
parser.add_argument("--rt", default="on")
parser.add_argument("--probes", default="auto")
args = parser.parse_args(argv)

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
bpy.ops.wm.usd_import(filepath=os.path.abspath(args.usd), import_usd_preview=True, import_all_materials=True,
                      create_world_material=False, apply_unit_conversion_scale=False,
                      import_textures_mode="IMPORT_NONE")
HFS = os.environ.get("HFS", "C:/Program Files/Side Effects Software/Houdini 22.0.466")
HOIIO = os.path.join(HFS, "bin", "hoiiotool.exe")
# Where husk's resolver finds bare file names such as StinsonBeach.hdr (Hydra's hdx textures).
SEARCH = [os.path.join(HFS, "bin", "usd_plugins", "hdx", "resources", "textures")]


def resolve(asset, layer_dir=""):
    """An SdfAssetPath's file, also for bare names the stage's resolver doesn't find."""
    if asset.resolvedPath:
        return asset.resolvedPath
    for d in [layer_dir] + SEARCH:
        p = os.path.join(d, asset.path)
        if os.path.exists(p):
            return p
    return asset.path

from mathutils import Matrix, Vector  # noqa: E402
from pxr import Gf, Sdf, Usd, UsdGeom, UsdLux, UsdShade  # noqa: E402

stage = Usd.Stage.Open(os.path.abspath(args.usd))
objects = {}
for o in scene.objects:
    objects.setdefault(o.name.split(".")[0], o)
materials = {}
for m in bpy.data.materials:
    materials.setdefault(m.name.split(".")[0], m)


def colorspace(image, raw):
    """Raw (values as stored) or sRGB-encoded, under whichever name this Blender's OCIO uses."""
    names = ("Raw", "Non-Color") if raw else ("sRGB", "sRGB Encoded Rec.709 (sRGB)")
    for n in names:
        try:
            image.colorspace_settings.name = n
            return
        except TypeError:
            pass


def readable(path):
    """`path`, or an .exr beside the output made from a .tx/.rat Blender can't read."""
    if os.path.splitext(path)[1].lower() not in (".tx", ".rat") or not os.path.exists(path):
        return path
    exr = os.path.join(os.path.dirname(os.path.abspath(args.out)), os.path.basename(path) + ".exr")
    if not os.path.exists(exr) and os.path.exists(HOIIO):
        subprocess.run([HOIIO, path, "-o", exr], check=False)
    return exr if os.path.exists(exr) else path


# --- Materials -----------------------------------------------------------------------------
# Principled input <- (shader input, default) per surface shader. Values in a tuple are summed
# by a function in SPECIAL below instead.
STANDARD = {
    "Base Color": ("base_color", (0.8, 0.8, 0.8)), "Metallic": ("metalness", 0.0),
    "Roughness": ("specular_roughness", 0.2), "IOR": ("specular_IOR", 1.5),
    "Specular Tint": ("specular_color", (1, 1, 1)), "Transmission Weight": ("transmission", 0.0),
    "Coat Weight": ("coat", 0.0), "Coat Roughness": ("coat_roughness", 0.1), "Coat IOR": ("coat_IOR", 1.5),
    "Coat Tint": ("coat_color", (1, 1, 1)), "Emission Color": ("emission_color", (1, 1, 1)),
    "Emission Strength": ("emission", 0.0), "Sheen Weight": ("sheen", 0.0),
    "Sheen Roughness": ("sheen_roughness", 0.3), "Sheen Tint": ("sheen_color", (1, 1, 1)),
    "Subsurface Weight": ("subsurface", 0.0), "Subsurface Scale": ("subsurface_scale", 1.0),
    "Anisotropic": ("specular_anisotropy", 0.0), "Alpha": ("opacity", (1, 1, 1)),
}
OPENPBR = {
    "Base Color": ("base_color", (0.8, 0.8, 0.8)), "Metallic": ("base_metalness", 0.0),
    "Roughness": ("specular_roughness", 0.3), "IOR": ("specular_ior", 1.5),
    "Specular Tint": ("specular_color", (1, 1, 1)), "Transmission Weight": ("transmission_weight", 0.0),
    "Coat Weight": ("coat_weight", 0.0), "Coat Roughness": ("coat_roughness", 0.0),
    "Coat IOR": ("coat_ior", 1.6), "Coat Tint": ("coat_color", (1, 1, 1)),
    "Emission Color": ("emission_color", (1, 1, 1)), "Emission Strength": ("emission_luminance", 0.0),
    "Sheen Weight": ("fuzz_weight", 0.0), "Sheen Tint": ("fuzz_color", (1, 1, 1)),
    "Subsurface Weight": ("subsurface_weight", 0.0), "Alpha": ("geometry_opacity", 1.0),
}
PREVIEW = {
    "Base Color": ("diffuseColor", (0.18, 0.18, 0.18)), "Metallic": ("metallic", 0.0),
    "Roughness": ("roughness", 0.5), "IOR": ("ior", 1.5), "Coat Weight": ("clearcoat", 0.0),
    "Coat Roughness": ("clearcoatRoughness", 0.01), "Emission Color": ("emissiveColor", (0, 0, 0)),
    "Alpha": ("opacity", 1.0),
}


def texture_file(shader, depth=4):
    """The image file feeding a shader output, a few nodes up (image/tiledimage/UsdUVTexture)."""
    if depth == 0:
        return None
    f = shader.GetInput("file")
    if f and f.Get() is not None:
        v = f.Get()
        return resolve(v)
    for i in shader.GetInputs():
        if i.HasConnectedSource():
            src = i.GetConnectedSource()[0]
            found = texture_file(UsdShade.Shader(src.GetPrim()), depth - 1)
            if found:
                return found
    return None


def build_material(mat, shader, table):
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    bsdf = nt.nodes.new("ShaderNodeBsdfPrincipled")
    nt.links.new(bsdf.outputs["BSDF"], out.inputs["Surface"])
    values = {}
    for socket, (name, default) in table.items():
        inp = shader.GetInput(name)
        value = inp.Get() if inp and inp.Get() is not None else default
        values[socket] = value
        if inp and inp.HasConnectedSource():
            path = texture_file(UsdShade.Shader(inp.GetConnectedSource()[0].GetPrim()))
            if path and os.path.exists(readable(path)):
                tex = nt.nodes.new("ShaderNodeTexImage")
                tex.image = bpy.data.images.load(readable(path), check_existing=True)
                srgb = isinstance(default, tuple) and os.path.splitext(path)[1].lower() in (".png", ".jpg", ".jpeg", ".tga", ".bmp")
                colorspace(tex.image, not srgb)
                nt.links.new(tex.outputs["Color"], bsdf.inputs[socket])
                continue
        sock = bsdf.inputs[socket]
        if socket == "Alpha" and isinstance(value, (tuple, Gf.Vec3f)):
            value = sum(value) / 3
        if hasattr(sock.default_value, "__len__"):
            sock.default_value = (*value[:3], 1)[:len(sock.default_value)]
        else:
            sock.default_value = float(value if not hasattr(value, "__len__") else sum(value) / len(value))
    if table is STANDARD:
        base = shader.GetInput("base")
        b = base.Get() if base and base.Get() is not None else 1.0
        if not bsdf.inputs["Base Color"].is_linked:
            c = bsdf.inputs["Base Color"].default_value
            bsdf.inputs["Base Color"].default_value = (c[0] * b, c[1] * b, c[2] * b, 1)
        spec = shader.GetInput("specular")
        bsdf.inputs["Specular IOR Level"].default_value = 0.5 * (spec.Get() if spec and spec.Get() is not None else 1.0)
    if table is PREVIEW:
        bsdf.inputs["Emission Strength"].default_value = 1.0
    if bsdf.inputs["Alpha"].default_value < 1 or bsdf.inputs["Alpha"].is_linked:
        mat.surface_render_method = "BLENDED"


for prim in stage.Traverse():
    if not prim.IsA(UsdShade.Material):
        continue
    shader = UsdShade.Material(prim).ComputeSurfaceSource("mtlx")[0]
    sid = shader.GetIdAttr().Get() if shader else None
    table = {"ND_standard_surface_surfaceshader": STANDARD, "ND_open_pbr_surface_surfaceshader": OPENPBR,
             "ND_UsdPreviewSurface_surfaceshader": PREVIEW}.get(str(sid))
    if table is None:
        continue  # UsdPreviewSurface: the importer's own material
    mat = materials.get(prim.GetName()) or bpy.data.materials.new(prim.GetName())
    mat.use_nodes = True
    materials[prim.GetName()] = mat
    build_material(mat, shader, table)

for prim in stage.Traverse():
    # Bindings on shape prims (Sphere, Cube, ...), which the importer drops; smooth them.
    obj = objects.get(prim.GetName())
    if obj is None or obj.type != "MESH" or not prim.IsA(UsdGeom.Gprim) or prim.IsA(UsdGeom.Mesh):
        continue
    material, _ = UsdShade.MaterialBindingAPI(prim).ComputeBoundMaterial()
    mat = materials.get(material.GetPrim().GetName()) if material else None
    if mat is not None:
        obj.data.materials.clear()
        obj.data.materials.append(mat)
    obj.modifiers.new("smooth", "SUBSURF").levels = obj.modifiers["smooth"].render_levels = 3
    for poly in obj.data.polygons:
        poly.use_smooth = True


# --- Lights ---------------------------------------------------------------------------------
def light_radiance(light):
    """Karma's emitted radiance: intensity * 2^exposure * colour."""
    s = (light.GetIntensityAttr().Get() or 0.0) * 2.0 ** (light.GetExposureAttr().Get() or 0.0)
    return s, light.GetColorAttr().Get() or Gf.Vec3f(1)


for prim in stage.Traverse():
    if not prim.HasAPI(UsdLux.LightAPI) and not prim.IsA(UsdLux.BoundableLightBase) \
            and not prim.IsA(UsdLux.NonboundableLightBase):
        continue
    obj = objects.get(prim.GetName())
    if obj is None or obj.type != "LIGHT":
        continue
    # Blender's power is Karma's: sun irradiance L * (1 or the disc's solid angle), else
    # L * pi * (1 or the area). Within 0.5% of husk Karma on a grey plane for every type.
    light = UsdLux.LightAPI(prim)
    L, color = light_radiance(light)
    normalize = bool(light.GetNormalizeAttr().Get())
    d = obj.data
    d.color = (color[0], color[1], color[2])
    d.diffuse_factor = light.GetDiffuseAttr().Get() if light.GetDiffuseAttr().Get() is not None else 1.0
    d.specular_factor = light.GetSpecularAttr().Get() if light.GetSpecularAttr().Get() is not None else 1.0
    if hasattr(d, "use_soft_falloff"):
        d.use_soft_falloff = False
    if hasattr(d, "normalize"):  # Blender 5: off scales the power by the light's area
        d.normalize = True
        d.exposure = 0.0
    if prim.IsA(UsdLux.DistantLight):
        angle = UsdLux.DistantLight(prim).GetAngleAttr().Get()
        angle = 0.53 if angle is None else angle
        d.angle = math.radians(angle)
        omega = 2 * math.pi * (1 - math.cos(math.radians(angle) / 2))
        d.energy = L * (1.0 if normalize else omega)  # irradiance
    elif prim.IsA(UsdLux.SphereLight):
        r = UsdLux.SphereLight(prim).GetRadiusAttr().Get() or 0.5
        d.shadow_soft_size = r
        d.energy = L * math.pi * (1.0 if normalize else 4 * math.pi * r * r)
    elif prim.IsA(UsdLux.RectLight) or prim.IsA(UsdLux.DiskLight):
        if prim.IsA(UsdLux.RectLight):
            w, h = UsdLux.RectLight(prim).GetWidthAttr().Get() or 1, UsdLux.RectLight(prim).GetHeightAttr().Get() or 1
            d.shape, d.size, d.size_y, area = "RECTANGLE", w, h, w * h
        else:
            r = UsdLux.DiskLight(prim).GetRadiusAttr().Get() or 0.5
            d.shape, d.size, area = "DISK", 2 * r, math.pi * r * r
        d.energy = L * math.pi * (1.0 if normalize else area)
    print("eevee_render: light %s %s %g W" % (prim.GetPath(), d.type, d.energy))

world = bpy.data.worlds.new("dome")
scene.world = world
world.use_nodes = True
nodes, links = world.node_tree.nodes, world.node_tree.links
nodes.clear()
bg = nodes.new("ShaderNodeBackground")
bg.inputs["Color"].default_value = (0, 0, 0, 1)
links.new(bg.outputs["Background"], nodes.new("ShaderNodeOutputWorld").inputs["Surface"])
for prim in stage.Traverse():
    # The first dome as the world.
    if prim.GetTypeName() not in ("DomeLight", "DomeLight_1"):
        continue
    L, color = light_radiance(UsdLux.LightAPI(prim))
    bg.inputs["Strength"].default_value = L
    bg.inputs["Color"].default_value = (color[0], color[1], color[2], 1)
    attr = prim.GetAttribute("inputs:texture:file")
    path = resolve(attr.Get()) if attr and attr.Get() else ""
    if path:
        env = nodes.new("ShaderNodeTexEnvironment")
        env.image = bpy.data.images.load(readable(path))
        colorspace(env.image, True)  # the texture's values as Karma reads them
        tint = nodes.new("ShaderNodeMixRGB")
        tint.blend_type = "MULTIPLY"
        tint.inputs["Fac"].default_value = 1.0
        tint.inputs["Color2"].default_value = (color[0], color[1], color[2], 1)
        links.new(env.outputs["Color"], tint.inputs["Color1"])
        links.new(tint.outputs["Color"], bg.inputs["Color"])
        # Blender world direction -> USD world (the importer turns Y-up to Z-up) -> the dome's
        # local frame -> Blender's equirect lookup. UsdLux latlong (as Eldr and Karma): u 0.25 at
        # local +X, 0.5 at +Z, +Y up; Blender's: u 0.25 at +Y, 0.5 at +X, +Z up.
        mapping = nodes.new("ShaderNodeMapping")
        coord = nodes.new("ShaderNodeTexCoord")
        xf = UsdGeom.Xformable(prim).ComputeLocalToWorldTransform(Usd.TimeCode.Default())
        dome = Matrix([[xf[c][r] for c in range(3)] for r in range(3)]).to_quaternion().to_matrix()
        yup = UsdGeom.GetStageUpAxis(stage) == UsdGeom.Tokens.y
        to_usd = Matrix(((1, 0, 0), (0, 0, 1), (0, -1, 0))) if yup else Matrix.Identity(3)
        to_lookup = Matrix(((0, 0, 1), (1, 0, 0), (0, 1, 0)))
        mapping.inputs["Rotation"].default_value = (to_lookup @ dome.transposed() @ to_usd).to_euler("XYZ")
        links.new(coord.outputs["Generated"], mapping.inputs["Vector"])
        links.new(mapping.outputs["Vector"], env.inputs["Vector"])
    o = objects.get(prim.GetName())
    if o is not None and o.type == "LIGHT":
        bpy.data.objects.remove(o)
    # Render Light Geometry authored off hides the dome from the camera (Eldr and Karma).
    for ns in ("eldr", "karma"):
        a = prim.GetAttribute("inputs:%s:light:renderlightgeo" % ns)
        if a and a.HasAuthoredValue():
            scene.render.film_transparent = not a.Get()
            break
    break

# --- Probes ---------------------------------------------------------------------------------
if args.probes == "auto":
    # A plane probe (planar reflection) on each flat glossy mesh. No sphere probe: placed by
    # rule it lands inside objects (the scene's centre is often inside one) and reflects their
    # inside; the world probe already gives the distant reflections.
    for o in [o for o in scene.objects if o.type == "MESH"]:
        mat = o.active_material
        rough = 0.5
        if mat and mat.use_nodes:
            bsdf = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
            rough = bsdf.inputs["Roughness"].default_value if bsdf and not bsdf.inputs["Roughness"].is_linked else rough
        if rough >= 0.3 or not o.data.polygons:
            continue
        # Flat: every vertex within 0.1% of the size from the first polygon's plane.
        n = (o.matrix_world.to_3x3() @ o.data.polygons[0].normal).normalized()
        pts = [o.matrix_world @ v.co for v in o.data.vertices]
        c = sum(pts, Vector()) / len(pts)
        size = max((p - c).length for p in pts)
        if size == 0 or max(abs((p - c).dot(n)) for p in pts) > 1e-3 * size:
            continue
        # The probe's square is its local XY (-1..1), centred on the mesh's extent in the
        # plane; it reflects along local +Z.
        u = (Vector((1, 0, 0)) if abs(n.x) < 0.9 else Vector((0, 1, 0)))
        u = (u - n * u.dot(n)).normalized()
        v = n.cross(u)
        du, dv = [(p - c).dot(u) for p in pts], [(p - c).dot(v) for p in pts]
        hu, hv = (max(du) - min(du)) / 2, (max(dv) - min(dv)) / 2
        # Just above the surface: a probe exactly in the plane breaks EEVEE's screen tracing.
        c = c + u * (max(du) + min(du)) / 2 + v * (max(dv) + min(dv)) / 2 + n * 1e-3 * size
        bpy.ops.object.lightprobe_add(type="PLANE")
        bpy.context.object.data.influence_distance = 0.01 * size
        bpy.context.object.matrix_world = Matrix((
            (u.x * hu, v.x * hv, n.x, c.x), (u.y * hu, v.y * hv, n.y, c.y),
            (u.z * hu, v.z * hv, n.z, c.z), (0, 0, 0, 1)))

# --- Camera, resolution, render -----------------------------------------------------------------
W, H, camera = 960, 540, args.camera
if args.settings:
    rs = stage.GetPrimAtPath(args.settings)
    if rs:
        res = rs.GetAttribute("resolution").Get()
        W, H = (res[0], res[1]) if res else (W, H)
        targets = rs.GetRelationship("camera").GetTargets() if rs.GetRelationship("camera") else []
        camera = camera or (str(targets[0]) if targets else None)
if args.size:
    W, H = (int(v) for v in args.size.split("x"))
cameras = [o for o in scene.objects if o.type == "CAMERA"]
if camera:
    name = camera.rstrip("/").split("/")[-1]
    cameras = [o for o in cameras if o.name == name or o.data.name == name] or cameras
if not cameras:
    sys.exit("no camera in %s" % args.usd)
scene.camera = cameras[0]
# The lens from the USD camera (the importer leaves unauthored apertures at Blender's 36 mm),
# fitted as RenderSettings' expandAperture (the default): the aperture grows to the image aspect.
usdcam = next((p for p in stage.Traverse() if p.IsA(UsdGeom.Camera) and
               (str(p.GetPath()) == camera or p.GetName() == scene.camera.name)), None)
if usdcam:
    gc = UsdGeom.Camera(usdcam).GetCamera(Usd.TimeCode.Default())
    cd = scene.camera.data
    k = 1.0 if gc.focalLength >= 1 else 100.0  # Blender's lens is 1 mm at least
    cd.lens = gc.focalLength * k
    cd.sensor_width, cd.sensor_height = gc.horizontalAperture * k, gc.verticalAperture * k
    wide = W / H >= gc.horizontalAperture / gc.verticalAperture
    cd.sensor_fit = "VERTICAL" if wide else "HORIZONTAL"
    fit = gc.verticalAperture if wide else gc.horizontalAperture
    cd.shift_x = gc.horizontalApertureOffset / fit
    cd.shift_y = gc.verticalApertureOffset / fit

scene.render.engine = "BLENDER_EEVEE"
scene.render.resolution_x, scene.render.resolution_y = W, H
scene.render.resolution_percentage = 100
scene.view_settings.view_transform = "Standard"  # linear EXR: no view transform applied anyway
eevee = scene.eevee
eevee.taa_render_samples = args.samples
eevee.use_shadows = True
eevee.light_threshold = 1e-5  # the default 0.01 cuts dim lights and the tails of others
# rt off: reflections from the world and sphere probes only. EEVEE 5.2 uses plane probes only
# in its screen tracing (rt on); in a background render they add nothing with ray tracing off
# or with Light Probe tracing.
eevee.use_raytracing = args.rt == "on"
if eevee.use_raytracing:
    eevee.ray_tracing_method = "SCREEN"
    eevee.ray_tracing_options.resolution_scale = "1"
    eevee.use_fast_gi = True
scene.render.image_settings.file_format = "OPEN_EXR"
scene.render.image_settings.color_depth = "32"
scene.render.filepath = os.path.abspath(args.out)
bpy.ops.render.render(write_still=True)
print("eevee_render: wrote %s (rt %s, camera %s, %dx%d)" % (args.out, args.rt, scene.camera.name, W, H))
