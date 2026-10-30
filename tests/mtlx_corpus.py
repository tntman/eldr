"""MaterialX coverage audit: every material of a corpus (MaterialX TestSuite and Examples,
Houdini's basic/quick materials) on a sphere, rendered by Eldr and by Karma (husk), compared.

TestSuite files mostly hold bare nodegraphs, which usdMtlx drops; each unused nodegraph output
is wrapped in a material first: colours (other types through `convert`) as standard_surface
emission (unlit, so pixels are the graph's value), BSDF/EDF through `surface`, surfaceshaders
as they are. Per material it reports the shading nodes Eldr can't compile (a static walk of
the USD network mirroring hydra/material.cpp, checked against Eldr's own warnings), and the
sphere's mean colour difference (CIEDE2000, white = 1) and pattern error against Karma.

Run (HDVK_BUILD = the build with hdVk.dll):
  usdenv.cmd python tests\\mtlx_corpus.py --out <dir> [--only stdlib_math,...] [--no-karma] [--static]
Writes <dir>/report.json, <dir>/report.md and <dir>/sheet_<group>.png (Eldr | Karma per cell).
`--selftest` checks the wrapping and Eldr's node-name rule without rendering (fast, for ctest)."""
import argparse
import glob
import json
import math
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET
from collections import Counter, defaultdict

from pxr import Gf, Sdf, Usd, UsdGeom, UsdRender, UsdShade, Vt

HERE = os.path.dirname(os.path.abspath(__file__))
USD = os.environ.get("USD_ROOT", r"C:\dev\usd-26.08-vdb")
HFS = r"C:\Program Files\Side Effects Software\Houdini 22.0.466"
MATERIALS = os.path.join(USD, "resources", "Materials")
TILE = 64  # px per sphere cell (sphere radius 1 in a 2.5 unit cell)
CELL = 2.5

# --- Eldr's compiler, as hydra/material.cpp has it (keep in sync) ------------------------
SURFACES = {"UsdPreviewSurface", "ND_UsdPreviewSurface_surfaceshader", "ND_standard_surface_surfaceshader",
            "ND_open_pbr_surface_surfaceshader", "ND_displacement_float", "ND_displacement_vector3", "ND_volume",
            "ND_gltf_pbr_surfaceshader", "ND_disney_principled", "ND_surface_unlit"}
# Karma's VEX nodes Eldr compiles by id (no nodedef).
KMA = ("kma_rampconst", "kma_color_hextiling", "kma_voronoinoise3d")
GRAPH = {"add", "subtract", "multiply", "divide", "min", "max", "power", "modulo", "absval", "floor", "ceil",
         "sin", "cos", "sqrt", "exp", "ln", "invert", "mix", "overlay", "clamp", "smoothstep", "dotproduct",
         "normalize", "luminance", "ifgreater", "ifgreatereq", "ifequal", "hsvadjust", "rgbtohsv", "hsvtorgb",
         "normalmap", "combine2", "combine3", "combine4", "constant", "convert", "dot", "extract", "separate2",
         "separate3", "separate4", "remap", "ramplr", "ramptb", "texcoord", "geompropvalue", "UsdPrimvarReader",
         "image", "tiledimage", "UsdUVTexture", "noise2d", "noise3d", "fractal2d", "fractal3d", "cellnoise2d",
         "cellnoise3d", "worleynoise2d", "worleynoise3d", "position", "normal", "place2d", "UsdTransform2d",
         "hinvlinear", "huniformramp", "heighttonormal", "fract", "round", "sign", "tan", "asin", "acos", "atan2",
         "magnitude", "crossproduct", "rotate2d", "rotate3d", "logical", "switch", "splitlr", "splittb", "frame",
         "time", "geomcolor", "tangent", "bitangent", "viewdirection", "artistic_ior", "transformpoint",
         "transformvector", "transformnormal", "transformmatrix", "and", "or", "not", "xor", "plus", "minus",
         "difference", "burn", "dodge", "screen", "disjointover", "in", "mask", "matte", "out", "over", "premult",
         "unpremult", "inside", "outside"}


def eldr_name(node_id):
    """material.cpp Kind(): ND_<name>_<type>, UsdShade <name>_<type>."""
    s = node_id[3:] if node_id.startswith("ND_") else node_id
    return s.split("_", 1)[0]


def eldr_supports(node_id, graphs=None):
    """Eldr compiles it natively, or expands its nodegraph (graphs: nodedef -> inner node
    categories, library_defs) into nodes it compiles."""
    if node_id in SURFACES or node_id.startswith(KMA) or eldr_name(node_id) in GRAPH:
        return True
    return graphs is not None and _category_ok(None, graphs, node_id, set())


def _category_ok(category, graphs, node_id=None, seen=None):
    """A category (or one nodedef) compiles: natively, or through a nodegraph of compiling nodes."""
    if category in GRAPH:
        return True
    ids = [node_id] if node_id else graphs["by_category"].get(category, [])
    for i in ids:
        if i in seen:
            continue
        seen.add(i)
        inner = graphs["inner"].get(i)
        if inner is not None and all(_category_ok(c, graphs, None, seen) for c in inner):
            return True
    return False


# --- MaterialX libraries: nodedef -> node category, and which are nodegraph-implemented ----
def library_defs():
    """defs: nodedef -> category; graphs: nodedef -> its nodegraph's node categories (Eldr's
    _Expand), and category -> nodedefs."""
    defs, graphs = {}, {"inner": {}, "by_category": defaultdict(list)}
    roots = [os.path.join(USD, "libraries"), os.path.join(HFS, "houdini", "materialx", "libraries")]
    for root in roots:
        for path in glob.glob(os.path.join(root, "**", "*.mtlx"), recursive=True):
            try:
                doc = parse(path)
            except ET.ParseError:
                continue
            for e in doc:
                if e.tag == "nodedef":
                    if e.get("name") not in defs:
                        graphs["by_category"][unns(e.get("node", ""))].append(e.get("name"))
                    defs.setdefault(e.get("name"), unns(e.get("node", "")))
                elif e.tag == "nodegraph" and e.get("nodedef"):
                    graphs["inner"].setdefault(e.get("nodedef"), {unns(n.tag) for n in e
                                                                  if n.tag not in ("output", "input")})
    return defs, graphs


# MaterialX writes namespaced tags (npr_test:toon_shade) without xmlns declarations.
def parse(path):
    text = open(path, encoding="utf-8").read()
    text = re.sub(r"<(/?)(\w+):(\w+)", r"<\1\2__NS__\3", text)
    text = text.replace("<UDIM>", "&lt;UDIM&gt;").replace("<UVTILE>", "&lt;UVTILE&gt;")
    text = re.sub(r"\sxmlns:\w+=\"[^\"]*\"", "", text)
    return ET.fromstring(text)


def unns(tag):
    return tag.replace("__NS__", ":")


# --- corpus: .mtlx with every unused output wrapped in a material -------------------------
COLOUR_LIKE = {"float", "vector2", "vector3", "vector4", "color4", "integer", "boolean"}


def wrap(src, dst):
    """Writes src with materials for its unused outputs to dst; returns what it skipped."""
    root = parse(src)
    base = os.path.dirname(src)
    for e in root.iter():
        if e.tag == "xi__NS__include" and e.get("href") and not os.path.isabs(e.get("href")):
            e.set("href", os.path.join(base, e.get("href")).replace("\\", "/"))
        fp = e.get("fileprefix")
        if fp and not os.path.isabs(fp) and not fp.startswith("resources/"):
            e.set("fileprefix", os.path.join(base, fp).replace("\\", "/") + "/")
    used = {(e.get("nodegraph"), e.get("output") or "") for e in root.iter("input") if e.get("nodegraph")}
    used_nodes = {e.get("nodename") for e in root.iter("input") if e.get("nodename")}
    skipped = []
    n = [0]
    originals = list(root)

    def add(material_input, label):
        n[0] += 1
        name = "W%03d_%s" % (n[0], re.sub(r"\W", "_", label))[:60]
        typ = material_input.pop("type")
        if typ in ("BSDF", "EDF"):
            s = ET.SubElement(root, "surface", name="S" + name, type="surfaceshader")
            ET.SubElement(s, "input", dict(material_input, name=typ.lower(), type=typ))
            shader = {"nodename": "S" + name}
        elif typ == "color3":
            s = ET.SubElement(root, "standard_surface", name="S" + name, type="surfaceshader")
            for k, v in (("base", "0"), ("specular", "0"), ("emission", "1")):
                ET.SubElement(s, "input", name=k, type="float", value=v)
            ET.SubElement(s, "input", dict(material_input, name="emission_color", type="color3"))
            shader = {"nodename": "S" + name}
        else:  # surfaceshader
            shader = material_input
        m = ET.SubElement(root, "surfacematerial", name=name, type="material")
        ET.SubElement(m, "input", dict(shader, name="surfaceshader", type="surfaceshader"))
        return name

    labels = {}
    # Outputs of nodegraphs, and the document's own outputs (of its top-level nodes).
    graphs = [(ng, ng.get("name")) for ng in originals if ng.tag == "nodegraph" and not ng.get("nodedef")]
    for ng, ng_name in graphs + [(root, None)]:
        for out in [o for o in (ng if ng_name else originals) if o.tag == "output"]:
            key = (ng_name, out.get("name"))
            if ng_name and (key in used or (ng_name, "") in used):
                continue
            typ = out.get("type")
            label = "%s.%s" % key if ng_name else out.get("name")
            src_attr = {k: out.get(k) for k in ("nodename", "output", "interfacename") if out.get(k)}
            ref = {"nodegraph": ng_name, "output": out.get("name")} if ng_name else \
                {k: v for k, v in src_attr.items() if k != "interfacename"}
            if not ng_name:
                used_nodes.add(out.get("nodename"))
            if typ in COLOUR_LIKE:  # through convert to colour3
                cv = ET.SubElement(ng, "convert", name="__cv_" + out.get("name"), type="color3")
                ET.SubElement(cv, "input", dict(src_attr, name="in", type=typ))
                if ng_name:
                    ET.SubElement(ng, "output", name="__c3_" + out.get("name"), type="color3",
                                  nodename="__cv_" + out.get("name"))
                    ref = {"nodegraph": ng_name, "output": "__c3_" + out.get("name")}
                else:
                    ref = {"nodename": "__cv_" + out.get("name")}
                labels[add(dict(ref, type="color3"), label)] = label
            elif typ in ("color3", "BSDF", "EDF", "surfaceshader"):
                labels[add(dict(ref, type=typ), label)] = label
            else:
                skipped.append("%s (%s)" % (label, typ))
    # Top-level shaders nobody uses.
    for e in originals:
        if e.get("type") in ("surfaceshader", "BSDF", "EDF") and e.tag not in ("output", "nodedef", "nodegraph") \
                and e.get("name") not in used_nodes and e.tag != "surfacematerial":
            labels[add({"type": e.get("type"), "nodename": e.get("name")}, e.get("name"))] = e.get("name")
        elif e.get("type") in ("displacementshader", "volumeshader") and e.get("name") not in used_nodes:
            skipped.append("%s (%s)" % (e.get("name"), e.get("type")))
    text = ET.tostring(root, encoding="unicode").replace("__NS__", ":").replace(
        "<materialx ", '<materialx xmlns:xi="http://www.w3.org/2001/XInclude" ', 1)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8") as f:
        f.write('<?xml version="1.0"?>\n' + text)
    return skipped, labels


def network(material):
    """Shader ids reachable from the material's surface (and displacement) outputs."""
    ids, seen = [], set()

    def walk(prim):
        if prim.GetPath() in seen:
            return
        seen.add(prim.GetPath())
        if prim.IsA(UsdShade.Shader):
            sid = UsdShade.Shader(prim).GetShaderId()
            # No id: a node the document defines itself (Hydra names it "<hash><ND_..><mtlx>").
            ids.append(str(sid) if sid else "local_nodedef")
            ports = UsdShade.Shader(prim).GetInputs()
        else:  # nodegraph or material: follow its outputs and inputs
            ports = UsdShade.ConnectableAPI(prim).GetOutputs() + UsdShade.ConnectableAPI(prim).GetInputs()
        for port in ports:
            for src in port.GetConnectedSources()[0]:
                walk(src.source.GetPrim())

    surf = next((s for s in (material.ComputeSurfaceSource(c)[0] for c in ("mtlx", "glslfx", "")) if s), None)
    if surf:
        walk(surf.GetPrim())
    disp = next((s for s in (material.ComputeDisplacementSource(c)[0] for c in ("mtlx", "")) if s), None)
    if disp:
        walk(disp.GetPrim())
    return bool(surf), ids


def corpus(out, only):
    """[(group, source layer, reference root, [(label, material path under the root)]), ...], notes."""
    items, notes = [], defaultdict(list)
    for kind in ("TestSuite", "Examples"):
        for src in sorted(glob.glob(os.path.join(MATERIALS, kind, "**", "*.mtlx"), recursive=True)):
            rel = os.path.relpath(src, os.path.join(MATERIALS, kind)).replace("\\", "/")
            parts = rel.split("/")
            if parts[0] in ("libraries",) and "definition" in rel or os.path.basename(src) == "_options.mtlx":
                continue
            group = ("%s_%s" % tuple(parts[:2]) if kind == "TestSuite" and len(parts) > 2 else
                     parts[0] if kind == "TestSuite" else "ex_" + parts[0])
            if only and group not in only:
                continue
            dst = os.path.join(out, "src", kind, rel)
            try:
                skipped, labels = wrap(src, dst)
            except ET.ParseError as e:
                notes[rel].append("unparsable: %s" % e)
                continue
            notes[rel] += ["not renderable: " + s for s in skipped]
            items.append((group, dst, "/MaterialX", rel, labels))
    for name in ("basic_materials", "quick_materials"):
        if only and "houdini_" + name not in only:
            continue
        src = os.path.join(HFS, "houdini", "usd", "materials", name, name + ".usd")
        items.append(("houdini_" + name, src, None, name, {}))
    return items, notes


def materials(layer, ref_root):
    stage = Usd.Stage.Open(layer)
    found = []
    for prim in stage.Traverse():
        if prim.IsA(UsdShade.Material):
            path = prim.GetPath()
            root = Sdf.Path(ref_root) if ref_root else path.GetPrefixes()[0]
            has_surface, ids = network(UsdShade.Material(prim))
            found.append((str(root), str(path.MakeRelativePath(root)), has_surface, ids))
    return found


# --- scene ----------------------------------------------------------------------------------
def sphere_mesh(stage, path, nu=32, nv=16):
    mesh = UsdGeom.Mesh.Define(stage, path)
    pts, st, counts, idx = [], [], [], []
    for j in range(nv + 1):
        v = j / nv
        th = math.pi * v
        for i in range(nu + 1):
            u = i / nu
            ph = 2 * math.pi * u
            pts.append(Gf.Vec3f(-math.sin(th) * math.sin(ph), -math.cos(th), -math.sin(th) * math.cos(ph)))
            st.append(Gf.Vec2f(u, v))
    for j in range(nv):
        for i in range(nu):
            a = j * (nu + 1) + i
            counts.append(4)
            idx += [a, a + 1, a + nu + 2, a + nu + 1]
    mesh.CreatePointsAttr(pts)
    mesh.CreateNormalsAttr(pts)
    mesh.SetNormalsInterpolation(UsdGeom.Tokens.vertex)
    mesh.CreateFaceVertexCountsAttr(counts)
    mesh.CreateFaceVertexIndicesAttr(idx)
    mesh.CreateSubdivisionSchemeAttr("none")
    mesh.CreateExtentAttr([(-1, -1, -1), (1, 1, 1)])
    UsdGeom.PrimvarsAPI(mesh).CreatePrimvar("st", Sdf.ValueTypeNames.TexCoord2fArray,
                                            UsdGeom.Tokens.vertex).Set(st)
    return mesh


def scene(folder, group, cells):
    """cells: [(source layer, ref root, material rel path)]; a cols x cols grid seen orthographically."""
    cols = max(1, math.ceil(math.sqrt(len(cells))))
    path = os.path.join(folder, group + ".usda")
    stage = Usd.Stage.CreateNew(path)
    UsdGeom.SetStageUpAxis(stage, UsdGeom.Tokens.y)
    cam = UsdGeom.Camera.Define(stage, "/World/cam")
    cam.CreateProjectionAttr("orthographic")
    cam.CreateHorizontalApertureAttr(cols * CELL * 10)
    cam.CreateVerticalApertureAttr(cols * CELL * 10)
    cam.CreateClippingRangeAttr(Gf.Vec2f(1, 100))
    cam.AddTranslateOp().Set(Gf.Vec3d(0, 0, 20))
    dome = stage.DefinePrim("/World/dome", "DomeLight")
    dome.CreateAttribute("inputs:intensity", Sdf.ValueTypeNames.Float).Set(0.5)
    key = stage.DefinePrim("/World/key", "DistantLight")
    key.CreateAttribute("inputs:intensity", Sdf.ValueTypeNames.Float).Set(2.0)
    key.CreateAttribute("inputs:angle", Sdf.ValueTypeNames.Float).Set(1.0)
    UsdGeom.Xformable(key).AddRotateXYZOp().Set(Gf.Vec3f(-35, -30, 0))
    proto = sphere_mesh(stage, "/Proto/sphere")
    stage.GetPrimAtPath("/Proto").SetSpecifier(Sdf.SpecifierClass)
    refs = {}
    for i, (layer, root, rel) in enumerate(cells):
        if (layer, root) not in refs:
            p = stage.DefinePrim("/World/src%d" % len(refs))
            p.GetReferences().AddReference(layer.replace("\\", "/"), root)
            refs[(layer, root)] = p.GetPath()
        s = stage.DefinePrim("/World/s%04d" % i, "Mesh")
        s.GetReferences().AddInternalReference(proto.GetPath())
        x = (i % cols - (cols - 1) / 2) * CELL
        y = ((cols - 1) / 2 - i // cols) * CELL
        UsdGeom.Xformable(s).AddTranslateOp().Set(Gf.Vec3d(x, y, 0))
        mat = UsdShade.Material(stage.GetPrimAtPath(refs[(layer, root)].AppendPath(rel)))
        UsdShade.MaterialBindingAPI.Apply(s).Bind(mat)
    stage.GetRootLayer().Save()
    return path.replace("\\", "/"), cols


def eldr_layer(folder, group, scene_path, cols):
    path = os.path.join(folder, group + "_eldr.usda")
    stage = Usd.Stage.CreateNew(path)
    stage.GetRootLayer().subLayerPaths.append(scene_path)
    s = UsdRender.Settings.Define(stage, "/Render/settings")
    p = UsdRender.Product.Define(stage, "/Render/product")
    p.CreateProductNameAttr(os.path.join(folder, group + "_eldr.exr").replace("\\", "/"))
    p.CreateResolutionAttr(Gf.Vec2i(cols * TILE, cols * TILE))
    p.CreateCameraRel().SetTargets(["/World/cam"])
    s.CreateProductsRel().SetTargets([p.GetPath()])
    s.CreateCameraRel().SetTargets(["/World/cam"])
    v = UsdRender.Var.Define(stage, "/Render/Vars/C")
    v.CreateSourceTypeAttr("lpe")
    v.CreateSourceNameAttr("C.*[LO]")
    v.CreateDataTypeAttr("color4f")
    p.CreateOrderedVarsRel().AddTarget(v.GetPath())
    stage.GetRootLayer().Save()
    return path.replace("\\", "/")


def search_path():
    dirs = [USD, os.path.join(USD, "lib", "usd", "hdx", "resources", "textures")]
    dirs += sorted({os.path.dirname(p) for p in glob.glob(os.path.join(MATERIALS, "**", "*.mtlx"), recursive=True)})
    return os.pathsep.join(dirs)


def run_eldr(folder, group, scene_path, cols):
    # HFS: Eldr reads Houdini's MaterialX library (hmtlx*, kma_* nodegraphs) from it.
    env = dict(os.environ, PXR_AR_DEFAULT_SEARCH_PATH=search_path(), HFS=HFS)
    r = subprocess.run([sys.executable, os.path.join(HERE, "exr_record.py"), eldr_layer(folder, group, scene_path, cols),
                        "/Render/settings", "/World/cam", "eldr:tonemapper=none", "eldr:autoExposure=false",
                        "eldr:exposureCompensation=0", "eldr:bloomIntensity=0", "eldr:productLinear=true"],
                       capture_output=True, text=True, env=env, timeout=1800)
    warnings = sorted(set(re.findall(r"shading node '([^']+)' isn't supported", r.stderr + r.stdout)))
    return os.path.join(folder, group + "_eldr.exr"), warnings, r.returncode, r.stderr[-2000:]


def husk_env(folder):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("PXR_", "PYTHON"))}
    env["HFS"] = HFS
    env["PATH"] = os.path.join(HFS, "bin") + os.pathsep + os.environ.get("SystemRoot", r"C:\Windows") + r"\System32"
    env["PXR_AR_DEFAULT_SEARCH_PATH"] = search_path()
    env["HOUDINI_USER_PREF_DIR"] = os.path.join(folder, "prefs", "houdini__HVER__")
    return env


def run_karma(folder, group, scene_path, cols):
    exr = os.path.join(folder, group + "_karma.exr")
    r = subprocess.run([os.path.join(HFS, "bin", "husk.exe"), "-c", "/World/cam", "--res", str(cols * TILE),
                        str(cols * TILE), "-p", "32", "-o", exr, scene_path], capture_output=True, text=True,
                       env=husk_env(folder), timeout=3600)
    return exr, r.returncode, (r.stdout + r.stderr)[-4000:]


def run_storm(folder, group, scene_path, cols):
    """Storm renders MaterialX through MaterialX's own GLSL generator: the referee where Eldr and Karma differ."""
    exr = os.path.join(folder, group + "_storm.exr")
    env = dict(os.environ, PXR_AR_DEFAULT_SEARCH_PATH=search_path())
    try:  # Storm generates a shader per material: about 10 s each
        subprocess.run(["usdrecord", "--renderer", "Storm", "--camera", "/World/cam", "--colorCorrectionMode",
                        "disabled", "--disableCameraLight", "--imageWidth", str(cols * TILE), scene_path, exr],
                       capture_output=True, text=True, env=env, timeout=7200, shell=True)
    except subprocess.TimeoutExpired:
        print("Storm timed out on", group)
    return exr


def read_exr(folder, exr):
    """EXR -> numpy (h, w, 3) through Houdini's hoiiotool and PFM."""
    import numpy as np
    if not os.path.exists(exr):
        return None
    pfm = exr[:-4] + ".pfm"
    subprocess.run([os.path.join(HFS, "bin", "hoiiotool.exe"), "-i", exr, "--ch", "R,G,B", "-o", pfm],
                   check=True, capture_output=True, env=husk_env(folder), timeout=300)
    with open(pfm, "rb") as f:
        assert f.readline().strip() == b"PF"
        w, h = map(int, f.readline().split())
        scale = float(f.readline())
        data = np.frombuffer(f.read(), dtype="<f4" if scale < 0 else ">f4").reshape(h, w, 3)
    # Eldr's buffers hold no negative radiance (Karma's do: signed noise, world positions).
    return np.maximum(np.nan_to_num(data[::-1].astype(np.float64), nan=0.0, posinf=0.0, neginf=0.0), 0.0)


# --- comparison ---------------------------------------------------------------------------
def lab(rgb):
    r, g, b = rgb
    xyz = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b, 0.2126729 * r + 0.7151522 * g + 0.0721750 * b,
           0.0193339 * r + 0.1191920 * g + 0.9503041 * b)
    f = lambda t: t ** (1 / 3) if t > 216 / 24389 else (24389 / 27 * t + 16) / 116
    fx, fy, fz = (f(max(c, 0) / n) for c, n in zip(xyz, (0.95047, 1.0, 1.08883)))
    return 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)


sys.path.insert(0, HERE)
from macbeth_compare import de2000  # noqa: E402


def cell_stats(img, i, cols):
    """Mean colour over the sphere's inner disk (radius 0.8), and the disk's pixels."""
    import numpy as np
    y0, x0 = (i // cols) * TILE, (i % cols) * TILE
    t = img[y0:y0 + TILE, x0:x0 + TILE]
    yy, xx = np.mgrid[0:TILE, 0:TILE]
    r = TILE / CELL * 0.8
    disk = (yy + 0.5 - TILE / 2) ** 2 + (xx + 0.5 - TILE / 2) ** 2 < r * r
    px = t[disk]
    return px.mean(axis=0), px


def lum(c):
    return 0.2126729 * c[..., 0] + 0.7151522 * c[..., 1] + 0.0721750 * c[..., 2]


def compare(e_img, k_img, i, cols):
    import numpy as np
    em, ep = cell_stats(e_img, i, cols)
    km, kp = cell_stats(k_img, i, cols)
    de = de2000(lab(em), lab(km))
    # Pattern: mean |luminance difference| over the disk, relative to Karma's mean (0.02 floor).
    pattern = float(np.abs(lum(ep) - lum(kp)).mean() / max(float(lum(km)), 0.02))
    ratio = float(lum(em)) / max(float(lum(km)), 1e-4)
    return dict(eldr=[round(float(c), 4) for c in em], karma=[round(float(c), 4) for c in km], dE=round(de, 2),
                pattern=round(pattern, 3), lum_ratio=round(ratio, 3))


def sheet(path, e_img, k_img, labels, cols):
    """Eldr | Karma per cell, sRGB-encoded, labelled."""
    import numpy as np
    from PySide6.QtCore import Qt
    from PySide6.QtGui import QColor, QFont, QGuiApplication, QImage, QPainter
    global _app
    _app = QGuiApplication.instance() or QGuiApplication([])  # fonts need one

    def srgb(img):
        if img is None:
            return np.zeros((cols * TILE, cols * TILE, 3), np.uint8)
        c = np.clip(img, 0, 1)
        c = np.where(c <= 0.0031308, 12.92 * c, 1.055 * c ** (1 / 2.4) - 0.055)
        return (c * 255 + 0.5).astype(np.uint8)

    e, k = srgb(e_img), srgb(k_img)
    rows = math.ceil(len(labels) / cols)
    ch = TILE + 12
    out = QImage(cols * 2 * TILE, rows * ch, QImage.Format_RGB888)
    out.fill(QColor(32, 32, 32))
    p = QPainter(out)
    p.setFont(QFont("Arial", 6))
    p.setPen(QColor(220, 220, 220))
    for i, label in enumerate(labels):
        y0, x0 = (i // cols) * TILE, (i % cols) * TILE
        for j, src in enumerate((e, k)):
            tile = np.ascontiguousarray(src[y0:y0 + TILE, x0:x0 + TILE])
            q = QImage(tile.data, TILE, TILE, TILE * 3, QImage.Format_RGB888)
            p.drawImage((i % cols) * 2 * TILE + j * TILE, (i // cols) * ch, q)
        p.drawText((i % cols) * 2 * TILE + 1, (i // cols) * ch + TILE, 2 * TILE - 2, 12,
                   Qt.AlignLeft | Qt.AlignVCenter, label[-26:])
    p.end()
    out.save(path)


# --- main ---------------------------------------------------------------------------------
def selftest():
    assert eldr_name("ND_multiply_color3FA") == "multiply" and eldr_supports("ND_multiply_color3FA")
    assert eldr_supports("UsdPrimvarReader_float2") and not eldr_supports("ND_hextiledimage_color3")
    assert eldr_supports("ND_separate3_color3") and eldr_supports("ND_gltf_pbr_surfaceshader")
    assert eldr_supports("ND_burn_color3") and eldr_supports("kma_rampconst_float")
    assert not eldr_supports("kma_ramp_float") and not eldr_supports("ND_surface")
    import tempfile
    d = tempfile.mkdtemp()
    src = os.path.join(d, "a.mtlx")
    with open(src, "w") as f:
        f.write('<?xml version="1.0"?><materialx version="1.39"><nodegraph name="g">'
                '<add name="a" type="float"><input name="in1" type="float" value="0.25"/></add>'
                '<output name="out" type="float" nodename="a"/><output name="m" type="matrix33" nodename="a"/>'
                '</nodegraph><x:custom name="c" type="color3"/></materialx>')
    skipped, labels = wrap(src, os.path.join(d, "b.mtlx"))
    assert skipped == ["g.m (matrix33)"] and list(labels.values()) == ["g.out"], (skipped, labels)
    mats = materials(os.path.join(d, "b.mtlx"), "/MaterialX")
    assert len(mats) == 1 and mats[0][2] and "ND_convert_float_color3" in mats[0][3], mats
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    ap.add_argument("--only", default="", help="groups to run (comma separated)")
    ap.add_argument("--skip", default="", help="materials to leave out, e.g. Light_Bulb (it lights its neighbours)")
    ap.add_argument("--no-karma", action="store_true")
    ap.add_argument("--static", action="store_true", help="inventory only, no rendering")
    ap.add_argument("--reuse", action="store_true", help="compare the EXRs of an earlier run in --out again")
    ap.add_argument("--storm", action="store_true", help="also render with Storm (MaterialX's GLSL) as a referee")
    ap.add_argument("--refs", help="an earlier run's --out: render Eldr only, compare with its Karma (and Storm) EXRs")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return 0
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    old = {}
    if a.reuse and os.path.exists(os.path.join(out, "report.json")):
        with open(os.path.join(out, "report.json")) as f:
            old = json.load(f)["renders"]
    only = set(filter(None, a.only.split(",")))
    skip = set(filter(None, a.skip.split(",")))
    defs, ng_impl = library_defs()
    items, notes = corpus(out, only)
    groups = defaultdict(list)  # group -> [(layer, root, rel, record)]
    records = []
    for group, layer, root, name, labels in items:
        try:
            found = materials(layer, root)
        except Exception as e:  # noqa: BLE001 - a corpus file USD can't open is a finding
            notes[name].append("USD can't open: %s" % str(e).splitlines()[0])
            continue
        if not found:
            notes[name].append("no materials")
        for ref_root, rel, has_surface, ids in found:
            label = labels.get(rel.split("/")[-1], rel.split("/")[-1])
            rec = dict(group=group, file=name, material=label, surface=has_surface,
                       nodes=sorted(set(ids)), unsupported=sorted({i for i in ids if not eldr_supports(i, ng_impl)}))
            records.append(rec)
            if has_surface and label not in skip:
                groups[group].append((layer, ref_root, rel, rec))
    renders = {}
    if not a.static:
        for group, cells in sorted(groups.items()):
            if a.reuse and os.path.exists(os.path.join(out, group + "_karma.exr")):  # compare again only
                cols = max(1, math.ceil(math.sqrt(len(cells))))
                prev = old.get(group, {})
                exr, warned, rc, err = (os.path.join(out, group + "_eldr.exr"), prev.get("eldr_warned", []),
                                        prev.get("eldr_rc"), prev.get("eldr_err", ""))
                kexr, krc, klog = os.path.join(out, group + "_karma.exr"), prev.get("karma_rc"), prev.get("karma_log", "")
            else:
                scene_path, cols = scene(out, group, [c[:3] for c in cells])
                exr, warned, rc, err = run_eldr(out, group, scene_path, cols)
                if a.refs:  # same corpus and --skip, so the same cells
                    kexr, krc, klog = os.path.join(a.refs, group + "_karma.exr"), 0, ""
                else:
                    kexr, krc, klog = run_karma(out, group, scene_path, cols) if not a.no_karma else (None, None, "")
            predicted = sorted({u for c in cells for u in c[3]["unsupported"]})
            e_img, k_img = read_exr(out, exr), read_exr(out, kexr) if kexr else None
            renders[group] = dict(eldr_rc=rc, eldr_warned=warned, predicted_unsupported=predicted,
                                  karma_rc=krc, eldr_err=err if rc else "", karma_log=klog if krc else "",
                                  karma_warnings=sorted(set(re.findall(r"(?im)^.*(?:warning|error).*$", klog)))[:40])
            s_img = None
            if a.storm:
                sexr = os.path.join(out, group + "_storm.exr")
                if a.refs:
                    sexr = os.path.join(a.refs, group + "_storm.exr")
                elif not (a.reuse and os.path.exists(sexr)):
                    sexr = run_storm(out, group, scene_path if not a.reuse else os.path.join(out, group + ".usda"), cols)
                s_img = read_exr(out, sexr)
            for i, c in enumerate(cells):
                if e_img is not None and k_img is not None:
                    c[3].update(compare(e_img, k_img, i, cols))
                if s_img is not None:
                    s = compare(e_img, s_img, i, cols)
                    c[3].update(storm=s["karma"], dE_storm=s["dE"],
                                dE_karma_storm=compare(k_img, s_img, i, cols)["dE"] if k_img is not None else None)
            sheet(os.path.join(out, "sheet_%s.png" % group), e_img, k_img, [c[3]["material"] for c in cells], cols)
            print("%-28s %3d materials  eldr rc %s karma rc %s  warned %s" % (group, len(cells), rc, krc, warned),
                  flush=True)
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(dict(records=records, notes=notes, renders=renders, defs=defs, ng_impl=sorted(ng_impl["inner"])), f,
                  indent=1)
    print(summary(records, notes, defs, ng_impl))
    return 0


def summary(records, notes, defs, ng_impl):
    """Node census: per node category, uses, materials using it, materials it alone blocks."""
    cat = lambda i: defs.get(i, re.sub(r"_(float|color[34]|vector[234]|integer|boolean)\w*$", "",
                                       i[3:] if i.startswith("ND_") else i))
    uses, mats, alone = Counter(), Counter(), Counter()
    ids_of = defaultdict(set)
    for r in records:
        for i in r["nodes"]:
            ids_of[cat(i)].add(i)
        for c in {cat(i) for i in r["nodes"]}:
            mats[c] += 1
        blockers = {cat(i) for i in r["unsupported"]}
        for c in blockers:
            uses[c] += 1
        if len(blockers) == 1:
            alone[blockers.pop()] += 1
    lines = ["| node | materials using | Eldr | materials blocked | blocked by it alone | nodegraph-implemented |",
             "|---|---|---|---|---|---|"]
    for c, n in sorted(mats.items(), key=lambda kv: (-uses[kv[0]], -kv[1])):
        ok = all(eldr_supports(i, ng_impl) for i in ids_of[c])
        part = not ok and any(eldr_supports(i, ng_impl) for i in ids_of[c])
        ng = any(i in ng_impl["inner"] for i in ids_of[c])
        lines.append("| %s | %d | %s | %d | %d | %s |" % (c, n, "yes" if ok else "partly" if part else "no", uses[c],
                                                       alone[c], "yes" if ng else ""))
    full = sum(1 for r in records if r["surface"] and not r["unsupported"])
    lines.append("\n%d materials with a surface, %d compile whole in Eldr; %d without a surface"
                 % (sum(r["surface"] for r in records), full, sum(not r["surface"] for r in records)))
    return "\n".join(lines)


if __name__ == "__main__":
    sys.exit(main())
