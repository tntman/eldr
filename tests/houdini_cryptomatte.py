r"""Renders step4 with husk and Eldr from a Karma-style Render Settings (Karma
Cryptomatte for primitives and materials), then checks the EXR with
OpenImageIO: Cryptomatte rank layers and manifests that parse. Houdini
only, not part of ctest.
Run: hython tests\houdini_cryptomatte.py <build-h22 dir>
husk runs with a throw-away HOUDINI_USER_PREF_DIR and
HOUDINI_PACKAGE_DIR=<build-h22>\houdini, so no other Eldr is loaded."""
import json
import os
import subprocess
import sys
import tempfile

import hou
import numpy as np
import OpenImageIO as oiio

build = os.path.abspath(sys.argv[1])
scene = os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "step4.usda")
work = tempfile.mkdtemp(prefix="eldr_crypto_")

# A Karma Render Settings LOP with the Cryptomatte vars.
stage = hou.node("/stage")
sub = stage.createNode("sublayer")
sub.parm("filepath1").set(scene)
std = stage.createNode("karmastandardrendervars")
std.setInput(0, sub)
crypto = stage.createNode("karmacryptomatte")
crypto.setInput(0, std)
crypto.parm("doprimcrypto").set(1)
crypto.parm("domtlcrypto").set(1)
settings = stage.createNode("karmarendersettings")
settings.setInput(0, crypto)
settings.parm("camera").set("/cam")
usd = os.path.join(work, "settings.usda")
settings.stage().Export(usd)

exr = os.path.join(work, "out.exr")
env = dict(os.environ)
env["HOUDINI_USER_PREF_DIR"] = os.path.join(tempfile.mkdtemp(prefix="eldr_pref_"), "houdini__HVER__")
os.makedirs(env["HOUDINI_USER_PREF_DIR"])
env["HOUDINI_PACKAGE_DIR"] = os.path.join(build, "houdini")
env["PXR_AR_DEFAULT_SEARCH_PATH"] = os.path.join(hou.text.expandString("$HFS"), "bin", "usd_plugins", "hdx",
                                                 "resources", "textures")
husk = os.path.join(hou.text.expandString("$HFS"), "bin", "husk")
subprocess.run([husk, "-R", "HdVkRendererPlugin", "-f", "1", "-n", "1", "-o", exr, usd], env=env, check=False)
assert os.path.exists(exr), "husk wrote no image"

image = oiio.ImageInput.open(exr)
parts = {}
part = 0
while image.seek_subimage(part, 0):
    spec = image.spec()
    parts[spec.get_string_attribute("name")] = (spec, np.array(image.read_image("float")).reshape(
        spec.height, spec.width, spec.nchannels))
    part += 1
header = oiio.ImageInput.open(exr).spec()
meta = {a.name: str(a.value) for a in header.extra_attribs if a.name.startswith("cryptomatte/")}

for name in ("CryptoPrimitives", "CryptoMaterials"):
    spec, layer = parts[name + "00"]  # rank layer: (id, coverage, id, coverage)
    assert spec.nchannels == 4, spec.channelnames
    key = next(k.split("/")[1] for k, v in meta.items() if k.endswith("/name") and v == name)
    assert meta["cryptomatte/%s/hash" % key] == "MurmurHash3_32"
    assert meta["cryptomatte/%s/conversion" % key] == "uint32_to_float32"
    manifest = json.loads(meta["cryptomatte/%s/manifest" % key])
    ids = {int(v, 16) for v in manifest.values()}
    # Ids are floats holding the hash's bits; each one drawn is in the manifest.
    drawn = set(np.unique(layer[..., 0].view(np.uint32)).tolist()) - {0}
    assert drawn and drawn <= ids, (name, sorted(drawn - ids))
    assert (layer[..., 1] == 1).sum() == (layer[..., 0].view(np.uint32) != 0).sum(), "coverage 1 where an id is"
    print(name, "ok:", len(drawn), "ids drawn,", len(manifest), "in the manifest")
