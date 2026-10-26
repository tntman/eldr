r"""Renders step4 with husk and Eldr from a Karma-style Render Settings (Karma
Cryptomatte for primitives and materials, Prim ID), then checks the EXR with
OpenImageIO: Cryptomatte rank layers 00..02 and manifests that parse, ids
that are not 0 on objects, and soft edges (a decoder: name -> manifest hash ->
float -> the coverage of every rank holding it). Rendered twice: at the
product's resolution, and with `husk --res` at another, where Eldr renders
at the product's framing and resamples into husk's buffers. Houdini only,
not part of ctest.
Run: hython tests\houdini_cryptomatte.py <build-h22 dir>
husk runs with a throw-away HOUDINI_USER_PREF_DIR and
HOUDINI_PACKAGE_DIR=<build-h22>\houdini, so no other Eldr is loaded."""
import json
import os
import struct
import subprocess
import sys
import tempfile

import hou
import numpy as np
import OpenImageIO as oiio

build = os.path.abspath(sys.argv[1])
scene = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "step4.usda")
work = tempfile.mkdtemp(prefix="eldr_crypto_")

# A Karma Render Settings LOP with the Cryptomatte and Prim ID vars.
stage = hou.node("/stage")
sub = stage.createNode("sublayer")
sub.parm("filepath1").set(scene)
std = stage.createNode("karmastandardrendervars")
std.setInput(0, sub)
std.parm("primid").set(1)
crypto = stage.createNode("karmacryptomatte")
crypto.setInput(0, std)
crypto.parm("doprimcrypto").set(1)
crypto.parm("domtlcrypto").set(1)
settings = stage.createNode("karmarendersettings")
settings.setInput(0, crypto)
settings.parm("camera").set("/cam")
settings.parm("resolutionx").set(320)  # the height follows (16:9): 320 x 180
usd = os.path.join(work, "settings.usda")
settings.stage().Export(usd)

env = dict(os.environ)
env["HOUDINI_USER_PREF_DIR"] = os.path.join(tempfile.mkdtemp(prefix="eldr_pref_"), "houdini__HVER__")
os.makedirs(env["HOUDINI_USER_PREF_DIR"])
env["HOUDINI_PACKAGE_DIR"] = os.path.join(build, "houdini")
env["PXR_AR_DEFAULT_SEARCH_PATH"] = os.path.join(hou.text.expandString("$HFS"), "bin", "usd_plugins", "hdx",
                                                 "resources", "textures")
husk = os.path.join(hou.text.expandString("$HFS"), "bin", "husk")


def read(exr):
    image = oiio.ImageInput.open(exr)
    parts = {}
    part = 0
    while image.seek_subimage(part, 0):
        spec = image.spec()
        parts[spec.get_string_attribute("name")] = (spec, np.array(image.read_image("float")).reshape(
            spec.height, spec.width, spec.nchannels))
        part += 1
    header = oiio.ImageInput.open(exr).spec()
    return parts, {a.name: str(a.value) for a in header.extra_attribs if a.name.startswith("cryptomatte/")}


def check(label, res):
    exr = os.path.join(work, label + ".exr")
    args = [husk, "-R", "HdVkRendererPlugin", "-f", "1", "-n", "1", "-o", exr, usd]
    if res:
        args[1:1] = ["--res", str(res[0]), str(res[1])]
    subprocess.run(args, env=env, check=False)
    assert os.path.exists(exr), label + ": husk wrote no image"
    parts, meta = read(exr)
    for name in ("CryptoPrimitives", "CryptoMaterials"):
        layers = []
        for rank in ("00", "01", "02"):
            spec, layer = parts[name + rank]  # (id, coverage, id, coverage)
            assert spec.nchannels == 4, spec.channelnames
            layers.append(layer)
        assert res is None or layers[0].shape[:2] == (res[1], res[0]), (label, layers[0].shape)
        key = next(k.split("/")[1] for k, v in meta.items() if k.endswith("/name") and v == name)
        assert meta["cryptomatte/%s/hash" % key] == "MurmurHash3_32"
        assert meta["cryptomatte/%s/conversion" % key] == "uint32_to_float32"
        manifest = json.loads(meta["cryptomatte/%s/manifest" % key])
        ids = {int(v, 16) for v in manifest.values()}
        # Ids are floats holding the hash's bits; each one drawn is in the manifest.
        bits = [l[..., 0::2].copy().view(np.uint32) for l in layers]  # ranks 2L, 2L+1 of each layer
        coverage = [l[..., 1::2] for l in layers]
        drawn = set(np.unique(np.concatenate([b.ravel() for b in bits])).tolist()) - {0}
        assert drawn and drawn <= ids, (name, sorted(drawn - ids))
        ids_all = np.concatenate(bits, axis=-1)       # (h, w, 6) ranks 0..5
        cov_all = np.concatenate(coverage, axis=-1)
        assert ((cov_all > 0) == (ids_all != 0)).all(), "coverage where an id is, and only there"
        total = cov_all.sum(-1)
        assert total.max() < 1.001, "coverages sum to at most 1 (%g)" % total.max()
        assert (np.diff(cov_all, axis=-1) <= 1e-6).all(), "ranks are sorted by coverage"
        # Edges blend: pixels partly one id, and partly another or the background.
        top = cov_all[..., 0]
        soft = ((top > 0.05) & (top < 0.95)).sum()
        assert soft >= 8, (name, "soft pixels", soft)
        assert (total > 0.98).sum() > 0.2 * total.size, "most of the image is covered"
        # A decoder: the matte of one object, from its manifest hash.
        biggest = max(manifest, key=lambda n: (ids_all == int(manifest[n], 16)).sum())
        wanted = np.uint32(int(manifest[biggest], 16))
        matte = (cov_all * (ids_all == wanted)).sum(-1)
        assert matte.max() <= 1.001 and ((matte > 0.05) & (matte < 0.95)).sum() >= 4, (name, biggest, "soft matte")
        print(label, name, "ok:", len(drawn), "ids drawn,", int(soft), "soft pixels, matte of", biggest)
    # Prim ID: a float, 0 on the background, counting from 1 on objects.
    primid = parts["primid"][1][..., 0]
    assert primid.shape == layers[0].shape[:2] and primid[0, 0] == 0 and (primid > 0).mean() > 0.3, primid[0, 0]
    assert len(np.unique(primid)) >= 3, np.unique(primid)
    print(label, "primid ok:", np.unique(primid)[:8], primid.shape)
    return parts


native = check("native", None)
other = check("res", (160, 90))
# The same scene at another size: the beauty is lit and the ids still cover it.
assert other["C"][1].shape[:2] == (90, 160), other["C"][1].shape
assert abs(native["C"][1][..., :3].mean() - other["C"][1][..., :3].mean()) < 0.15 * native["C"][1][..., :3].mean()
print("sizes ok")
