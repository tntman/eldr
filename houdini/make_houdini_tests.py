r"""Builds houdini-tests/<NN_name>.hip, one Houdini scene per item of the
Houdini checklist (2026-10-30), with the showcase scenes' standard render
settings nodes (eldr_default, eldr_final, karma), the item's own A/B nodes,
a USD Render ROP each and a sticky note saying what to check:

    hython houdini/make_houdini_tests.py [name ...]

Same environment as houdini/make_showcase_hip.py. Test scenes are sublayered
by $HIP-relative paths; crowd_9k only by its absolute path (never copied).
Pyro and SOP volumes are SOP nodes in the .hip (no caches).
"""
import os
import sys

import hou

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_showcase_hip as m  # noqa: E402

OUT = os.path.join(m.REPO, "houdini-tests")
CROWD = "E:/usd-testscenes/crowd_9k/crowd_9k_v001.usd"
SCENES = "$HIP/../tests/scenes/"
TESTS = []


def test(note):
    def wrap(fn):
        TESTS.append((fn.__name__.lstrip("_"), note, fn))
        return fn
    return wrap


def sublayer(net, path):
    n = net.createNode("sublayer", "scene")
    n.parm("filepath1").set(path)
    return n


def eldr(name, **attrs):
    return {"name": name, "renderer": m.ELDR, "attrs": {k.replace("__", ":"): (None, v) for k, v in attrs.items()}}


def karma_node(name, renderer, **attrs):
    """A Karma Render Settings node (Karma's UI) rendered by `renderer`."""
    return {"name": name, "renderer": renderer, "node": "karma",
            "attrs": {k.replace("__", ":"): (None, v) for k, v in attrs.items()}}


def chain(net, last, *nodes):
    for n in nodes:
        n.setInput(0, last)
        last = n
    return last


def sop_volume_geo(name):
    """An /obj geo node to put SOP volumes in."""
    return hou.node("/obj").createNode("geo", name)


# --- the items -------------------------------------------------------------

@test("Camera switch: look through /cam, then /cameras/cam2, then the free perspective camera, and back.\n"
      "Each switch restarts the film cleanly: no smear of the old view over the new one (lane 17).")
def _01_camera_switch(net):
    sub = sublayer(net, SCENES + "cornell.usda")
    cam = net.createNode("camera", "cam2")
    m.set_parm(cam, "primpath", "/cameras/cam2")
    m.set_parm(cam, "t", (1.5, 1.2, 4.5))
    m.set_parm(cam, "r", (-8, 18, 0))
    return chain(net, sub, cam), "/cam", (480, 480), []


@test("Moving camera in crowd_9k: tumble in the viewport. While moving, half resolution\n"
      "(Moving Render Scale 0.5 on Eldr's Render Settings tab), full sharpness as soon as it stops.\n"
      "Does the moving image look too soft? Compare eldr_moving1 (scale 1).\n"
      "Renders: ROPs use $ELDR_HUSK (deferred skinning). Karma renders the crowd without it.")
def _02_moving_camera_crowd(net):
    sub = sublayer(net, CROWD)
    return sub, None, (480, 270), [eldr("eldr_moving1", eldr__movingRenderScale=1.0)]


@test("Light Linker LOP: lightlink.usda's key light already skips the right ball (collection:lightLink);\n"
      "this Light Linker LOP also takes the middle ball off the key light, and the fill light's shadow\n"
      "off the left ball. Expect: middle and right ball lit by the fill only, no fill shadow from the left ball.")
def _03_light_linker(net):
    sub = sublayer(net, SCENES + "lightlink.usda")
    ll = net.createNode("lightlinker", "light_linker")
    m.set_parm(ll, "num_links", 2)
    for i, (light, geo, kind) in enumerate((("/key", "/balls/middle", "light"), ("/fill", "/balls/left", "shadow")), 1):
        m.set_parm(ll, "link_prim_%d" % i, light)
        m.set_parm(ll, "link_includes_%d" % i, "/")
        m.set_parm(ll, "link_excludes_%d" % i, geo)
        m.set_parm(ll, "link_type_%d" % i, kind)
    return chain(net, sub, ll), "/cam", (960, 540), [eldr("eldr_nolinking", eldr__lightLinking=0)]


@test("IES profile (Karma's shaping:ies:file) on a Houdini spot light: the asymmetric profile\n"
      "spot_asym.ies should throw its bright lobe the same way in Eldr and Karma (phi direction unverified).\n"
      "Rotate the light: the pattern must turn with it.")
def _04_ies(net):
    sub = sublayer(net, SCENES + "ies.usda")
    light = net.createNode("light::2.0", "ies_light")
    m.set_parm(light, "primpath", "/lights/ies_light")
    m.set_parm(light, "lighttype", "UsdLuxSphereLight")
    m.set_parm(light, "iesenable", 1)
    m.set_parm(light, "t", (0, 2.5, 0))
    m.set_parm(light, "r", (-90, 0, 0))
    m.set_attr(light, "inputs:intensity", 30)
    m.set_attr(light, "inputs:radius", 0.05)
    m.set_attr(light, "inputs:shaping:ies:file", "$HIP/../tests/scenes/textures/spot_asym.ies")
    return chain(net, sub, light), "/cam", (960, 540), [eldr("eldr_noies", eldr__lightIes=0)]


@test("Karma Render Settings > Color Limit: at 20 Eldr shows the emitters at 20 like Karma;\n"
      "at 1e6 (Karma's 0 renders black, not off) the visible emitters keep full HDR (40 and 60). Render karma_limit20 and karma_limit1e6\n"
      "with Eldr (render_*_eldr) and Karma (render_*_karma) and compare.")
def _05_colour_limit(net):
    sub = sublayer(net, SCENES + "emissive.usda")
    return sub, "/cam", (960, 540), [
        karma_node("karma_limit20_eldr", m.ELDR, karma__global__colorlimit=20),
        karma_node("karma_limit1e6_eldr", m.ELDR, karma__global__colorlimit=1e6),
        karma_node("karma_limit1e6_karma", m.KARMA, karma__global__colorlimit=1e6)]


@test("Karma quality parms (Diffuse / Reflect / Light / SSS / Volume Quality) affect Eldr:\n"
      "karma_quality_high (diffuse 4, reflect 2, light 2) should be visibly less noisy in GI and\n"
      "reflections at the same film samples than karma_quality_low (0.25 each), in Eldr as in Karma.")
def _06_karma_quality(net):
    sub = sublayer(net, SCENES + "emissive.usda")
    hi = dict(karma__object__diffusequality=4, karma__object__reflectquality=2,
              karma__global__light_sampling_quality=2, karma__object__sssquality=2, karma__object__volumequality=2)
    lo = {k: 0.25 for k in hi}
    return sub, "/cam", (960, 540), [
        karma_node("karma_quality_high_eldr", m.ELDR, **hi), karma_node("karma_quality_low_eldr", m.ELDR, **lo),
        karma_node("karma_quality_high_karma", m.KARMA, **hi)]


@test("Caustics: the ball's Karma 'Enable Caustics' (Render Geometry Settings) and the sun's\n"
      "'Contributes Caustics': a focused bright patch on the floor in front of the ball, in Eldr\n"
      "(eldr_final ticks eldr:caustics) and Karma. eldr_nocaustics: the plain tinted shadow.")
def _07_caustics(net):
    sub = sublayer(net, SCENES + "caustics.usda")
    geo = net.createNode("rendergeometrysettings", "ball_caustics")
    m.set_parm(geo, "primpattern", "/ball")
    m.set_attr(geo, "primvars:karma:object:causticsenable", 1)
    sun = net.createNode("light::2.0", "sun_caustics") if hou.nodeType(hou.lopNodeTypeCategory(), "light::2.0") else None
    last = chain(net, sub, geo)
    if sun:
        m.set_parm(sun, "primpath", "/sun")
        m.set_parm(sun, "createprims", "off", warn=False)
        m.set_attr(sun, "inputs:karma:light:contributescaustics", 1)
        last = chain(net, last, sun)
    return last, "/cam", (960, 540), [eldr("eldr_nocaustics", eldr__caustics=0)]


# Houdini 22's Material Palette (hou.galleries) has no Silk or SoapBubble; redvelvet stands in for sheen.
GALLERY = ["gold", "copper", "silver", "aluminum", "titanium", "redvelvet", "glass", "pianolacquer"]


@test("Houdini materials (Material Library filled from the Material Palette: gold, copper, silver,\n"
      "aluminum, titanium, redvelvet, glass, pianolacquer) against Karma: one ball each, left to right.\n"
      "Metals should match Karma's colour and roughness; redvelvet's sheen and the glass are the hard ones.\n"
      "Silk and SoapBubble are not in H22's palette: add them by hand from the Material Catalog.")
def _08_houdini_materials(net):
    sub = sublayer(net, SCENES + "cornell.usda")
    lib = net.createNode("materiallibrary", "materials")
    lib.setInput(0, sub)
    made = []
    for name in GALLERY:
        entries = hou.galleries.galleryEntries(name)
        entries = [e for e in entries if e.canCreateChildNode(lib)] if entries else []
        if entries:
            made.extend(entries[0].createChildNode(lib) for _ in [0])
    lib.layoutChildren()
    last = lib
    for i, mat in enumerate(made):
        s = net.createNode("sphere", "ball_%s" % mat.name())
        m.set_parm(s, "primpath", "/balls/%s" % mat.name())
        m.set_parm(s, "t", (-1.5 + 3.0 * i / max(len(made) - 1, 1), 0.35, 0.8))
        m.set_parm(s, "radius", 0.25, warn=False)
        a = net.createNode("assignmaterial", "assign_%s" % mat.name())
        m.set_parm(a, "primpattern1", "/balls/%s" % mat.name())
        m.set_parm(a, "matspecpath1", "/materials/%s" % mat.name())
        last = chain(net, last, s, a)
    print("  materials:", [x.name() for x in made])
    return last, "/cam", (960, 540), []


@test("Denoise: Karma's Denoiser (oidn) and eldr:denoise. Houdini's own OIDN 2.3.3 must load without\n"
      "errors (watch the console / husk log). karma_oidn_eldr: Karma's denoise image filter read by Eldr;\n"
      "eldr_denoise_off: the raw film.")
def _09_denoise(net):
    sub = sublayer(net, SCENES + "emissive.usda")
    return sub, "/cam", (960, 540), [
        karma_node("karma_oidn_eldr", m.ELDR, karma__global__imagefilter=m.OIDN),
        eldr("eldr_denoise_off", eldr__filmSamples=16, eldr__denoise=0),
        eldr("eldr_denoise_on", eldr__filmSamples=16, eldr__denoise=1)]


def _pyro(geo):
    """A tiny pyro sim (SOP nodes only, no cache): a sphere emits, rises with a curl."""
    src = geo.createNode("sphere", "source")
    m.set_parm(src, "type", "poly")
    m.set_parm(src, "rad", (0.4, 0.4, 0.4))
    ps = geo.createNode("pyrosource", "pyro_source")
    ps.setInput(0, src)
    m.set_parm(ps, "mode", 2)  # scatter points through the volume
    m.set_parm(ps, "particlesep", 0.05)
    attrs = geo.createNode("attribcreate::2.0", "density_temperature")
    attrs.setInput(0, ps)
    m.set_parm(attrs, "numattr", 2)
    for i, name in ((1, "density"), (2, "temperature")):
        m.set_parm(attrs, "name%d" % i, name)
        m.set_parm(attrs, "value%dv" % i, (1.0, 0, 0, 0))
    ras = geo.createNode("volumerasterizeattributes", "rasterize")
    ras.setInput(0, attrs)
    m.set_parm(ras, "attributes", "density temperature")
    m.set_parm(ras, "voxelsize", 0.08)
    sim = geo.createNode("pyrosolver", "pyro")
    sim.setInput(0, ras)
    m.set_parm(sim, "divsize", 0.08)
    return sim


@test("Pyro with vel + Karma 'Velocity Blur' (Render Geometry Settings, karma:object:vblur =\n"
      "velocity blur) and motion blur on: the smoke blurs along vel in Eldr as in Karma (lane 42).\n"
      "Frame 24. The sim is SOP nodes (pyrosolver), cooked on first view: no cache in the .hip.")
def _10_pyro_velocity_blur(net):
    geo = sop_volume_geo("pyro")
    sim = _pyro(geo)
    out = geo.createNode("null", "OUT")
    out.setInput(0, sim)
    out.setDisplayFlag(True)
    out.setRenderFlag(True)
    geo.layoutChildren()
    imp = net.createNode("sopimport", "pyro_import")
    m.set_parm(imp, "soppath", out.path())
    m.set_parm(imp, "primpath", "/pyro", warn=False)
    cam = net.createNode("camera", "cam")
    m.set_parm(cam, "primpath", "/cam")
    m.set_parm(cam, "t", (0, 1.2, 6))
    light = net.createNode("distantlight::2.0", "sun")
    m.set_parm(light, "rx", -40)
    m.set_parm(light, "ry", 30)
    gs = net.createNode("rendergeometrysettings", "velocity_blur")
    m.set_parm(gs, "primpattern", "/pyro/**")
    m.set_attr(gs, "primvars:karma:object:vblur", "Velocity Blur")
    hou.setFrame(24)
    return chain(net, imp, cam, light, gs), "/cam", (640, 480), [
        eldr("eldr_novblur", eldr__volumeVelocityBlur=0)]


@test("A nameless SOP volume (Volume SOP with an empty name: Houdini calls it field_0) shows as density\n"
      "in Eldr, as in Karma.")
def _11_nameless_sop_volume(net):
    geo = sop_volume_geo("nameless")
    v = geo.createNode("volume", "volume_noname")
    m.set_parm(v, "name", "")
    m.set_parm(v, "initialval1", 2.0)
    m.set_parm(v, "divsize", 0.05)
    v.setDisplayFlag(True)
    v.setRenderFlag(True)
    imp = net.createNode("sopimport", "volume_import")
    m.set_parm(imp, "soppath", v.path())
    cam = net.createNode("camera", "cam")
    m.set_parm(cam, "primpath", "/cam")
    m.set_parm(cam, "t", (0, 0, 5))
    light = net.createNode("distantlight::2.0", "sun")
    m.set_parm(light, "rx", -40)
    m.set_parm(light, "ry", 30)
    return chain(net, imp, cam, light), "/cam", (480, 360), []


@test("Coloured smoke casts coloured shadows (lane 21): a red-orange smoke ball (density and Cd fields\n"
      "from SOPs) over a white floor under the sun. The shadow on the floor is tinted, in Eldr as in Karma.\n"
      "eldr_rtshadows: the same with traced shadows.")
def _12_coloured_smoke_shadows(net):
    geo = sop_volume_geo("smoke")
    s = geo.createNode("sphere", "ball")
    m.set_parm(s, "type", "poly")
    m.set_parm(s, "t", (0, 1.0, 0))
    pts = geo.createNode("pyrosource", "points")  # points filling the ball
    pts.setInput(0, s)
    m.set_parm(pts, "mode", 2)
    m.set_parm(pts, "particlesep", 0.04)
    attrs = geo.createNode("attribcreate::2.0", "density_cd")
    attrs.setInput(0, pts)
    m.set_parm(attrs, "numattr", 2)
    m.set_parm(attrs, "name1", "density")
    m.set_parm(attrs, "value1v", (3.0, 0, 0, 0))
    m.set_parm(attrs, "name2", "Cd")
    m.set_parm(attrs, "size2", 3)
    m.set_parm(attrs, "value2v", (1.0, 0.25, 0.05, 0))
    ras = geo.createNode("volumerasterizeattributes", "rasterize")
    ras.setInput(0, attrs)
    m.set_parm(ras, "attributes", "density Cd")
    m.set_parm(ras, "voxelsize", 0.04)
    ras.setDisplayFlag(True)
    ras.setRenderFlag(True)
    geo.layoutChildren()
    imp = net.createNode("sopimport", "smoke_import")
    m.set_parm(imp, "soppath", ras.path())
    floor = net.createNode("cube", "floor")
    m.set_parm(floor, "primpath", "/floor")
    m.set_parm(floor, "s", (8, 0.02, 8))
    cam = net.createNode("camera", "cam")
    m.set_parm(cam, "primpath", "/cam")
    m.set_parm(cam, "t", (0, 2.5, 6))
    m.set_parm(cam, "r", (-18, 0, 0))
    light = net.createNode("distantlight::2.0", "sun")
    m.set_parm(light, "rx", -55)
    m.set_parm(light, "ry", 25)
    return chain(net, imp, floor, cam, light), "/cam", (640, 360), [
        eldr("eldr_rtshadows", eldr__rtShadows=1)]


@test("husk progress and --timelimit with batching: render render_eldr_batch and watch the progress\n"
      "(Render Scheduler / MPlay) climb in steps of Offline Batch Seconds (2 s); render_eldr_timelimit\n"
      "stops after 5 s (husk --timelimit 5) with what it has.")
def _13_husk_progress_timelimit(net):
    sub = sublayer(net, SCENES + "emissive.usda")
    return sub, "/cam", (960, 540), [
        eldr("eldr_batch", eldr__filmSamples=512, eldr__offlineBatchSeconds=2.0),
        dict(eldr("eldr_timelimit", eldr__filmSamples=4096), timelimit=5)]


@test("AOV menu and Sample Blending: the viewport's Render Outputs menu should list depth, N, primid,\n"
      "instanceid, the light group C<L.'key'>, transmission/sss/coat/volume/shadow, caustics, beautyRaw\n"
      "and fill each. The depth_min var's Karma 'Sample Blending' (min) makes edges take the near depth;\n"
      "eldr:filter has no UI (Edit Properties LOP).")
def _14_aov_menu_sample_blending(net):
    sub = sublayer(net, SCENES + "cornell.usda")
    vars_ = [("C", "C.*[LO]", "color4f", None), ("depth", "depth", "float", None),
             ("depth_min", "depth", "float", '["minmax",{"mode":"min"}]'), ("N", "N", "normal3f", None),
             ("primid", "primid", "int", None), ("instanceid", "instanceid", "int", None),
             ("lg_key", "C.*<L.'key'>", "color3f", None), ("transmission", "C<TD>.*L", "color3f", None),
             ("sss", "C<TS>.*L", "color3f", None), ("coat", "C<RS'coat'>.*L", "color3f", None),
             ("volume", "CV.*L", "color3f", None), ("shadow", "shadow;C<RD>L", "color3f", None),
             ("caustics", "C<RD>.+<TS>L", "color3f", None), ("beautyRaw", "eldr:beautyRaw", "color3f", None)]
    last = sub
    paths = []
    for name, src, dt, filt in vars_:
        v = net.createNode("rendervar", name)
        m.set_parm(v, "primpath", "/Render/Products/Vars/" + name)
        m.set_parm(v, "sourceName", src)
        m.set_attr(v, "driver:parameters:aov:husk:name", name)
        # The LOP authors Format "float" (its UI follows Data Type, a script's set doesn't): husk then
        # writes one channel per var. Unauthored, husk takes the channels from dataType.
        v.parm(hou.text.encodeParm("driver:parameters:aov:husk:format_control")).set("none")
        m.set_parm(v, "sourceType", "raw" if src in ("depth", "N", "primid", "instanceid", "eldr:beautyRaw") else "lpe")
        m.set_parm(v, "dataType", dt)
        if filt:
            m.set_attr(v, "driver:parameters:aov:karma:filter", filt)
        last = chain(net, last, v)
        paths.append("/Render/Products/Vars/" + name)
    prod = net.createNode("renderproduct", "aovs")
    m.set_parm(prod, "primpath", "/Render/Products/aovs")
    m.set_parm(prod, "productName", "$HIP/render/aovs.$F4.exr", warn=False) or m.set_parm(prod, "productname", "$HIP/render/aovs.$F4.exr")
    m.set_parm(prod, "orderedVars", " ".join(paths), warn=False) or m.set_parm(prod, "orderedvars", " ".join(paths))
    return chain(net, last, prod), "/cam", (480, 480), [], ["/Render/Products/aovs"]


def build(name, note, fn):
    hou.hipFile.clear(suppress_save_prompt=True)
    net = hou.node("/stage")
    r = fn(net)
    last, camera, res, extra = r[:4]
    products = r[4] if len(r) > 4 else []
    if camera is None:  # the scene's own camera: the first one on the stage
        cams = [p for p in last.stage().Traverse() if p.GetTypeName() == "Camera"]
        camera = str(cams[0].GetPath()) if cams else "/cam"
    std = m.standard_prims({"attrs": {}, "products": products})
    for p in extra:
        p.setdefault("products", products)
    m.add_render_nodes(net, last, camera, res, std + extra, note)
    hou.hipFile.save(os.path.join(OUT, name + ".hip"))
    print("built", name, [p["name"] for p in std + extra])


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    want = sys.argv[1:]
    for name, note, fn in TESTS:
        if not want or name in want:
            build(name, note, fn)
