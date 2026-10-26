"""Colour check: Eldr (and, if husk is installed, Karma) against a ColorChecker's published
reflectances, in four lightings (tests/scenes/macbeth.usda's "light" variants: hdri, white,
distant, rect). Linear scene-referred EXRs only: no tonemap, exposure, bloom, OCIO or sRGB
encode. Eldr is forced to eldr:tonemapper=none, eldr:autoExposure=false,
eldr:exposureCompensation=0, eldr:bloomIntensity=0 and eldr:productLinear=true; Karma's
husk EXR has no display transform.

For each variant it prints per patch: reference albedo, Eldr, Karma, Eldr/Karma, and a
CIEDE2000 summary (Lab from linear Rec.709, white = Karma's white patch Y). In "white"
(uniform dome, intensity 1) an ideal diffuse patch's radiance equals its albedo, and both
renderers are checked against that. A run using the HDRI is marked INVALID when its white
patch matches the white dome's (the HDRI did not load).

Run: usdenv.cmd python tests\\macbeth_compare.py --exrinfo <exrinfo.exe> [--variants hdri,white,...]
     [--no-karma] [--check]   (--check: fail unless Eldr matches the analytic white dome; for ctest)"""
import argparse
import math
import os
import subprocess
import sys
import tempfile

from pxr import Gf, Sdf, Usd, UsdGeom, UsdRender, UsdShade

HERE = os.path.dirname(os.path.abspath(__file__))
SCENE = os.path.join(HERE, "scenes", "macbeth.usda").replace("\\", "/")
W, H = 600, 400
PPU = W / 7.2  # pixels per scene unit at the chart (50 mm lens, 36 mm film, 10 units away)
BOX = 40  # inner box of each 83 px patch
HUSK = r"C:\Program Files\Side Effects Software\Houdini 22.0.466"
NAMES = ["dark skin", "light skin", "blue sky", "foliage", "blue flower", "bluish green", "orange",
         "purplish blue", "moderate red", "purple", "yellow green", "orange yellow", "blue", "green", "red",
         "yellow", "magenta", "cyan", "white 9.5", "neutral 8", "neutral 6.5", "neutral 5", "neutral 3.5", "black 2"]
WHITE = 18


def reference():
    stage = Usd.Stage.Open(SCENE)
    return [tuple(stage.GetPrimAtPath("/Macbeth/mtl/m%02d/pbr" % (i + 1)).GetAttribute("inputs:diffuseColor").Get())
            for i in range(24)]


def wrapper(folder, variant, settings):
    """A layer over macbeth.usda selecting the light variant; with `settings`, an EXR product for Eldr."""
    path = os.path.join(folder, "%s_%s.usda" % (variant, "eldr" if settings else "karma"))
    stage = Usd.Stage.CreateNew(path)
    stage.GetRootLayer().subLayerPaths.append(SCENE)
    stage.OverridePrim("/Macbeth").GetVariantSet("light").SetVariantSelection(variant)
    if settings:
        exr = os.path.join(folder, variant + "_eldr.exr").replace("\\", "/")
        s = UsdRender.Settings.Define(stage, "/Render/settings")
        p = UsdRender.Product.Define(stage, "/Render/product")
        p.CreateProductNameAttr(exr)
        p.CreateResolutionAttr(Gf.Vec2i(W, H))
        p.CreateCameraRel().SetTargets(["/Macbeth/cam"])
        s.CreateProductsRel().SetTargets([p.GetPath()])
        s.CreateCameraRel().SetTargets(["/Macbeth/cam"])
        v = UsdRender.Var.Define(stage, "/Render/Vars/C")
        v.CreateSourceTypeAttr("lpe")
        v.CreateSourceNameAttr("C.*[LO]")
        v.CreateDataTypeAttr("color4f")
        p.CreateOrderedVarsRel().AddTarget(v.GetPath())
    stage.GetRootLayer().Save()
    return path.replace("\\", "/")


def run_eldr(folder, variant):
    layer = wrapper(folder, variant, True)
    exr = os.path.join(folder, variant + "_eldr.exr")
    subprocess.run([sys.executable, os.path.join(HERE, "exr_record.py"), layer, "/Render/settings", "/Macbeth/cam",
                    "eldr:tonemapper=none", "eldr:autoExposure=false", "eldr:exposureCompensation=0",
                    "eldr:bloomIntensity=0", "eldr:productLinear=true"], check=True, timeout=900)
    return exr


def run_karma(folder, variant, husk):
    layer = wrapper(folder, variant, False)
    exr = os.path.join(folder, variant + "_karma.exr")
    env = {k: v for k, v in os.environ.items() if not k.startswith(("PXR_", "PYTHON"))}
    env["HFS"] = HUSK
    env["PATH"] = os.path.join(HUSK, "bin") + os.pathsep + os.environ.get("SystemRoot", r"C:\Windows") + r"\System32"
    env["PXR_AR_DEFAULT_SEARCH_PATH"] = os.environ.get("PXR_AR_DEFAULT_SEARCH_PATH", "")
    subprocess.run([husk, "-c", "/Macbeth/cam", "--res", str(W), str(H), "-o", exr, layer], check=True, env=env,
                   timeout=1800)
    return exr


def sample(exrinfo, exr):
    """24 patch means (R, G, B) over the inner box of each patch."""
    out = []
    for i in range(24):
        cx = W / 2 + (i % 6 - 2.5) * 1.1 * PPU
        cy = H / 2 + (i // 6 - 1.5) * 1.1 * PPU
        text = subprocess.run([exrinfo, exr, str(int(cx - BOX / 2)), str(int(cy - BOX / 2)), str(BOX), str(BOX)],
                              capture_output=True, text=True, check=True, timeout=60).stdout.splitlines()
        px = {l.split()[1]: float(l.split()[2]) for l in text if l.startswith("pixel ")}
        out.append((px["R"], px["G"], px["B"]))
    return out


# --- colour: linear Rec.709 -> XYZ (D65) -> Lab -> CIEDE2000 --------------------------------
def lab(rgb, yn):
    r, g, b = rgb
    xyz = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b, 0.2126729 * r + 0.7151522 * g + 0.0721750 * b,
           0.0193339 * r + 0.1191920 * g + 0.9503041 * b)
    f = lambda t: t ** (1 / 3) if t > 216 / 24389 else (24389 / 27 * t + 16) / 116
    fx, fy, fz = (f(max(c, 0) / (n * yn)) for c, n in zip(xyz, (0.95047, 1.0, 1.08883)))
    return 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)


def de2000(l1, l2):
    L1, a1, b1 = l1
    L2, a2, b2 = l2
    C1, C2 = math.hypot(a1, b1), math.hypot(a2, b2)
    G = 0.5 * (1 - math.sqrt(((C1 + C2) / 2) ** 7 / (((C1 + C2) / 2) ** 7 + 25 ** 7)))
    a1, a2 = a1 * (1 + G), a2 * (1 + G)
    C1, C2 = math.hypot(a1, b1), math.hypot(a2, b2)
    h1, h2 = math.degrees(math.atan2(b1, a1)) % 360, math.degrees(math.atan2(b2, a2)) % 360
    dL, dC = L2 - L1, C2 - C1
    dh = h2 - h1
    dh = dh - 360 if dh > 180 else dh + 360 if dh < -180 else dh
    if C1 * C2 == 0:
        dh = 0
    dH = 2 * math.sqrt(C1 * C2) * math.sin(math.radians(dh) / 2)
    Lm, Cm = (L1 + L2) / 2, (C1 + C2) / 2
    hm = (h1 + h2) / 2 if C1 * C2 and abs(h1 - h2) <= 180 else (h1 + h2 + 360) / 2 if C1 * C2 and h1 + h2 < 360 \
        else (h1 + h2 - 360) / 2 if C1 * C2 else h1 + h2
    T = (1 - 0.17 * math.cos(math.radians(hm - 30)) + 0.24 * math.cos(math.radians(2 * hm)) +
         0.32 * math.cos(math.radians(3 * hm + 6)) - 0.20 * math.cos(math.radians(4 * hm - 63)))
    SL = 1 + 0.015 * (Lm - 50) ** 2 / math.sqrt(20 + (Lm - 50) ** 2)
    SC, SH = 1 + 0.045 * Cm, 1 + 0.015 * Cm * T
    RT = -2 * math.sqrt(Cm ** 7 / (Cm ** 7 + 25 ** 7)) * math.sin(math.radians(60 * math.exp(-(((hm - 275) / 25) ** 2))))
    return math.sqrt((dL / SL) ** 2 + (dC / SC) ** 2 + (dH / SH) ** 2 + RT * (dC / SC) * (dH / SH))


def summary(a, b, yn):
    d = [de2000(lab(x, yn), lab(y, yn)) for x, y in zip(a, b)]
    worst = max(range(24), key=d.__getitem__)
    return sum(d) / 24, d[worst], NAMES[worst], d


def lum(c):
    return 0.2126729 * c[0] + 0.7151522 * c[1] + 0.0721750 * c[2]


def table(variant, ref, eldr, karma):
    exp = ref if variant == "white" else None
    print("\n=== %s ===" % variant)
    print("%-14s %-22s %-22s %-22s %s" % ("patch", "reference albedo", "Eldr", "Karma", "Eldr/Karma (R G B)"))
    for i in range(24):
        f = lambda c: "%.4f %.4f %.4f" % c if c else "-"
        ratio = " ".join("%.3f" % (e / k if k > 1e-6 else float("nan")) for e, k in zip(eldr[i], karma[i])) if karma else "-"
        print("%-14s %-22s %-22s %-22s %s" % (NAMES[i], f(ref[i]), f(eldr[i]), f(karma[i]) if karma else "-", ratio))
    if karma:
        yn = lum(karma[WHITE])
        m, w, wn, _ = summary(eldr, karma, yn)
        print("dE2000 Eldr vs Karma (Lab white = Karma patch 19 Y): mean %.2f, worst %.2f (%s)" % (m, w, wn))
    if exp:
        for name, img in (("Eldr", eldr), ("Karma", karma)):
            if img:
                m, w, wn, _ = summary(img, exp, 1.0)
                rel = max(abs(c / r - 1) for p, q in zip(img, exp) for c, r in zip(p, q) if r > 0.02)
                print("analytic (radiance = albedo x 1) vs %s: dE2000 mean %.2f, worst %.2f (%s); max rel error %.1f%% (channels > 0.02)"
                      % (name, m, w, wn, 100 * rel))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exrinfo", required=True)
    ap.add_argument("--variants", default="hdri,white,distant,rect")
    ap.add_argument("--no-karma", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    folder = a.out or tempfile.mkdtemp(prefix="eldr_macbeth_")
    os.makedirs(folder, exist_ok=True)
    husk = os.path.join(HUSK, "bin", "husk.exe")
    karma_on = not a.no_karma and os.path.exists(husk)
    if not a.no_karma and not karma_on:
        print("husk not found (%s): Eldr only" % husk)
    ref = reference()
    variants = a.variants.split(",")
    if "hdri" in variants:  # the HDRI check compares against the white dome, so that runs first
        variants = ["white"] + [v for v in variants if v != "white"]
    results = {}
    bad = []
    for v in variants:
        eldr = sample(a.exrinfo, run_eldr(folder, v))
        karma = sample(a.exrinfo, run_karma(folder, v, husk)) if karma_on else None
        results[v] = (eldr, karma)
        table(v, ref, eldr, karma)
        if v == "hdri" and "white" in results:
            for name, i in (("Eldr", 0), ("Karma", 1)):
                img, white = results["hdri"][i], results["white"][i]
                if img is None:
                    continue
                dl = lum(img[WHITE]) / lum(white[WHITE]) - 1
                dc = (img[WHITE][0] / img[WHITE][2]) / (white[WHITE][0] / white[WHITE][2]) - 1
                ok = abs(dl) > 0.05 or abs(dc) > 0.05
                print("HDRI loaded in %s: %s (white patch vs white dome: luminance %+.1f%%, R/B %+.1f%%)"
                      % (name, "yes" if ok else "NO - comparison INVALID", 100 * dl, 100 * dc))
    if a.check:
        eldr = results["white"][0]
        for i in range(24):
            for c, r in zip(eldr[i], ref[i]):
                if abs(c - r) > 0.01 + 0.03 * r:
                    bad.append("%s: %.4f vs albedo %.4f" % (NAMES[i], c, r))
        if bad:
            print("FAIL: Eldr's white dome is not albedo x intensity:\n  " + "\n  ".join(bad), file=sys.stderr)
            return 1
        print("white dome matches albedo x intensity within 0.01 + 3%")
    return 0


if __name__ == "__main__":
    sys.exit(main())
