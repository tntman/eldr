"""Karma golden references: Karma CPU renders of a fixed set of tests/scenes, made once and kept
OUTSIDE the repo, and Eldr compared against them by a few coarse metrics (docs/testing.md).

  usdenv.cmd python tests\\karma_refs.py [--scenes a,b]       renders the references with husk
  usdenv.cmd python tests\\karma_refs.py --compare --exrinfo <exrinfo.exe> [--update] [--scenes a,b]
                                                              renders Eldr and compares (ctest karma_golden)

References go to %ELDR_REFS% (default E:\\eldr-refs): <scene>.exr plus manifest.json (scene, camera,
settings, husk, date, the scene file's sha1). Both renderers get the same wrapper layer: 320x180, a 16:9
aperture, linear colour (Karma: Path Traced, 256 samples; Eldr: final quality, no tonemap, bloom or
auto exposure). Metrics per scene, Eldr / Karma:
  lum     mean luminance ratio
  region  worst |ln ratio| of the luminance of a 4x4 grid of regions
  mald    mean |ln ratio| per channel over 32x18 cells (eps 1% of Karma's mean luminance)
tests/karma_golden.json keeps each scene's baseline and the tolerances; a scene fails when it moves
farther from Karma than its baseline plus tolerance. --update records the current values as baseline.
Exit 77 (ctest skip) when no reference exists."""
import argparse
import datetime
import hashlib
import json
import math
import os
import subprocess
import sys
import tempfile
import time

from pxr import Usd, UsdGeom

HERE = os.path.dirname(os.path.abspath(__file__))
REFS = os.environ.get("ELDR_REFS", r"E:\eldr-refs")
HFS = os.environ.get("ELDR_HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466")
BASELINE = os.path.join(HERE, "karma_golden.json")
W, H, SPP = 320, 180, 256
SPP_SCENE = {"emissive": 2048, "caustics": 1024}  # small emitters, caustics: Karma Path Traced is noisy at 256
# Per scene settings for both renderers: caustics without Karma's colour limit (it clamps every sample that sees
# the sun through the glass, so its caustics would be a fraction of the light's).
EXTRA_SCENE = {"caustics": "custom float karma:global:colorlimit = 100000"}
CELLS, REGIONS = (32, 18), (4, 4)
# lighting/exposure, materials, volumes, transmission, displacement, SSS, caustics
SCENES = ["cornell", "emissive", "lights", "step4", "sunangle", "mtlx", "glass", "volume", "displacement",
          "subsurface", "caustics", "transparency_order"]
CAMERA = "/cam"
ELDR = ["eldr:finalQuality=true", "eldr:tonemapper=none", "eldr:autoExposure=false",
        "eldr:exposureCompensation=0", "eldr:bloomIntensity=0", "eldr:productLinear=true",
        "eldr:domeCameraVisible=false"]  # Karma hides domes from the camera (renderlightgeo 0)


def scene_path(name):
    return os.path.join(HERE, "scenes", name + ".usda").replace("\\", "/")


def sha1(path, crlf=False):
    # Line endings normalised: git may check the scene out with CRLF.
    with open(path, "rb") as f:
        data = f.read().replace(b"\r\n", b"\n")
    return hashlib.sha1(data.replace(b"\n", b"\r\n") if crlf else data).hexdigest()


def wrapper(folder, name, exr=None):
    """A layer over the scene: 16:9 camera, /Render/golden (Karma's samples, Eldr's linear settings);
    with `exr`, an EXR product for Eldr (husk names its own output with -o)."""
    scene = scene_path(name)
    stage = Usd.Stage.Open(scene)  # kept alive while the camera is read
    cam = UsdGeom.Camera(stage.GetPrimAtPath(CAMERA))
    vap = cam.GetHorizontalApertureAttr().Get() * H / W
    product = ""
    if exr:
        product = f"""
        rel products = </Render/product>
    }}
    def RenderProduct "product" {{
        token productName = "{exr.replace(os.sep, "/")}"
        int2 resolution = ({W}, {H})
        rel camera = <{CAMERA}>
        rel orderedVars = </Render/C>
    }}
    def RenderVar "C" {{
        uniform token sourceType = "lpe"
        uniform string sourceName = "C.*[LO]"
        uniform token dataType = "color4f"
"""
    path = os.path.join(folder, name + ("_eldr" if exr else "_karma") + ".usda")
    with open(path, "w") as f:
        f.write(f"""#usda 1.0
(
    subLayers = [@{scene}@]
)
over "{CAMERA[1:]}" {{
    float verticalAperture = {vap}
}}
def Scope "Render" {{
    def RenderSettings "golden" {{
        rel camera = <{CAMERA}>
        int2 resolution = ({W}, {H})
        custom token karma:global:convergence_mode = "Path Traced"
        custom int karma:global:pathtracedsamples = {SPP_SCENE.get(name, SPP)}
        custom bool eldr:finalQuality = 1
        custom string eldr:tonemapper = "none"
        custom bool eldr:autoExposure = 0
        custom float eldr:bloomIntensity = 0
        custom bool eldr:domeCameraVisible = 0
        {EXTRA_SCENE.get(name, "")}{product}
    }}
}}
""")
    return path.replace("\\", "/")


def render_refs(names):
    husk = os.path.join(HFS, "bin", "husk.exe")
    if not os.path.exists(husk):
        sys.exit("husk not found: %s (set ELDR_HFS)" % husk)
    os.makedirs(REFS, exist_ok=True)
    manifest_path = os.path.join(REFS, "manifest.json")
    manifest = json.load(open(manifest_path)) if os.path.exists(manifest_path) else {"scenes": {}}
    env = {k: v for k, v in os.environ.items() if not k.startswith(("PXR_", "PYTHON"))}
    env["HFS"] = HFS
    env["PATH"] = os.path.join(HFS, "bin") + os.pathsep + os.environ.get("SystemRoot", r"C:\Windows") + r"\System32"
    env["PXR_AR_DEFAULT_SEARCH_PATH"] = os.environ.get("PXR_AR_DEFAULT_SEARCH_PATH", "")  # StinsonBeach.hdr
    work = tempfile.mkdtemp(prefix="eldr_karma_refs_")
    manifest.update(husk=husk, houdini=os.path.basename(HFS), resolution=[W, H],
                    settings={"renderer": "BRAY_HdKarma", "karma:global:convergence_mode": "Path Traced",
                              "karma:global:pathtracedsamples": SPP, "per scene": SPP_SCENE})
    for name in names:
        exr = os.path.join(REFS, name + ".exr")
        t = time.time()
        subprocess.run([husk, "-R", "BRAY_HdKarma", "-s", "/Render/golden", "-o", exr, wrapper(work, name)],
                       env=env, check=True, timeout=1800)
        manifest["scenes"][name] = {"scene": "tests/scenes/%s.usda" % name, "camera": CAMERA, "exr": name + ".exr",
                                    "sha1": sha1(scene_path(name)), "samples": SPP_SCENE.get(name, SPP), "seconds": round(time.time() - t, 1),
                                    "date": datetime.datetime.now().isoformat(timespec="seconds")}
        print("%-12s %6.1f s  %s" % (name, time.time() - t, exr), flush=True)
        with open(manifest_path, "w") as f:  # after every scene: an interrupted run keeps what it made
            json.dump(manifest, f, indent=2)


def grid(exrinfo, exr, nx, ny):
    """Per cell (row major) the (R, G, B) mean."""
    lines = subprocess.run([exrinfo, exr, "--grid", str(nx), str(ny)], capture_output=True, text=True,
                           check=True, timeout=120).stdout.splitlines()
    chans = [l.split()[1] for l in lines if l.startswith("channel ")]
    idx = [next(i for i, n in enumerate(chans) if n == c or n.endswith("." + c)) for c in "RGB"]
    return [tuple(float(l.split()[3 + i]) for i in idx) for l in lines if l.startswith("cell ")]


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def metrics(exrinfo, eldr, karma):
    ke, kk = grid(exrinfo, eldr, *CELLS), grid(exrinfo, karma, *CELLS)
    re, rk = grid(exrinfo, eldr, *REGIONS), grid(exrinfo, karma, *REGIONS)
    mk = sum(map(lum, kk)) / len(kk)
    eps = 0.01 * mk + 1e-4
    ratio = (sum(map(lum, ke)) / len(ke)) / max(mk, 1e-9)
    region = max(abs(math.log((lum(a) + eps) / (lum(b) + eps))) for a, b in zip(re, rk))
    mald = sum(abs(math.log((x + eps) / (y + eps))) for a, b in zip(ke, kk) for x, y in zip(a, b)) / (3 * len(ke))
    return {"lum": round(ratio, 4), "region": round(region, 4), "mald": round(mald, 4)}


def distances(m):  # distance to Karma per metric (0 = Karma)
    return {"lum": abs(math.log(max(m["lum"], 1e-9))), "region": m["region"], "mald": m["mald"]}


def compare(names, exrinfo, update):
    manifest_path = os.path.join(REFS, "manifest.json")
    refs = json.load(open(manifest_path))["scenes"] if os.path.exists(manifest_path) else {}
    base = json.load(open(BASELINE))
    work = tempfile.mkdtemp(prefix="eldr_karma_golden_")
    print("references: %s\n%-12s %-22s %-22s %-22s %s" % (REFS, "scene", "lum E/K (base)", "region (base)",
                                                           "mald (base)", "status"))
    failed, compared, better = [], 0, []
    for name in names:
        ref = refs.get(name)
        karma = os.path.join(REFS, name + ".exr")
        if not ref or not os.path.exists(karma):
            print("%-12s no reference: skipped" % name)
            continue
        if ref["sha1"] not in (sha1(scene_path(name)), sha1(scene_path(name), crlf=True)):
            print("%-12s scene changed since its reference: skipped (rerun tests/karma_refs.py --scenes %s)"
                  % (name, name))
            continue
        exr = os.path.join(work, name + "_eldr.exr")
        subprocess.run([sys.executable, os.path.join(HERE, "exr_record.py"), wrapper(work, name, exr),
                        "/Render/golden", CAMERA] + ELDR, check=True, timeout=900)
        m = metrics(exrinfo, exr, karma)
        compared += 1
        b = base["scenes"].get(name)
        status = "no baseline"
        if b:
            tol = dict(base["tolerance"], **b.get("tolerance", {}))
            d, db = distances(m), distances(b)
            worse = [k for k in d if d[k] > db[k] + tol[k]]
            if worse:
                failed.append("%s: %s farther from Karma" % (name, ", ".join(worse)))
            if any(d[k] < db[k] - tol[k] for k in d):
                better.append(name)
            status = "FAIL (%s)" % ", ".join(worse) if worse else "ok"
        cell = lambda k: "%.3f (%s)" % (m[k], "%.3f" % b[k] if b else "-")
        print("%-12s %-22s %-22s %-22s %s" % (name, cell("lum"), cell("region"), cell("mald"), status), flush=True)
        if update:
            base["scenes"].setdefault(name, {}).update(m)
    if update:
        with open(BASELINE, "w") as f:
            json.dump(base, f, indent=2)
            f.write("\n")
        print("baseline updated: " + BASELINE)
    if better and not update:
        print("closer to Karma than the baseline (record with --update): " + ", ".join(better))
    if not compared:
        print("no Karma references in %s: run tests/karma_refs.py once (skipped)" % REFS)
        return 77
    if failed and not update:
        print("FAIL: " + "; ".join(failed), file=sys.stderr)
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scenes", default=",".join(SCENES))
    ap.add_argument("--compare", action="store_true")
    ap.add_argument("--exrinfo")
    ap.add_argument("--update", action="store_true")
    a = ap.parse_args()
    names = a.scenes.split(",")
    if not a.compare:
        render_refs(names)
        return 0
    if not a.exrinfo:
        sys.exit("--compare needs --exrinfo")
    return compare(names, a.exrinfo, a.update)


if __name__ == "__main__":
    sys.exit(main())
