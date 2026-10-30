"""Writes textures/spot_asym.ies: an LM-63 type C spot (beam to 40 degrees from
nadir) brighter towards phi 0 than phi 180 (1 : 0.3), to check the IES
orientation against Karma (ies.usda). Run: python tests\\scenes\\make_ies.py"""
import math
import os

V = [5.0 * i for i in range(19)]   # 0..90
H = [45.0 * i for i in range(9)]   # 0..360


def candela(theta, phi):
    beam = math.cos(math.radians(theta * 90.0 / 40.0)) ** 2 if theta < 40.0 else 0.0
    return 1000.0 * beam * (0.65 + 0.35 * math.cos(math.radians(phi)))


lines = ["IESNA:LM-63-2002", "[TEST] Eldr test profile (tests/scenes/make_ies.py)", "TILT=NONE",
         "1 -1 1 %d %d 1 2 0 0 0" % (len(V), len(H)), "1 1 100",
         " ".join("%g" % v for v in V), " ".join("%g" % h for h in H)]
lines += [" ".join("%.1f" % candela(v, h) for v in V) for h in H]
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "textures", "spot_asym.ies")
with open(path, "w") as f:
    f.write("\n".join(lines) + "\n")
print(path)
