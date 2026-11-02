"""eldr:tileSize: usdrecord renders showcase/2026-11-02_01_tiles (bloom,
depth of field, screen-space reflections, AO) at 1920x1080 in 512-pixel tiles
and whole; the images match within noise, with no seam along the tile edges.
Run: usdenv.cmd python tests\\test_tiles.py"""
import os
import subprocess
import sys
import tempfile

from PySide6.QtGui import QImage
from PySide6.QtWidgets import QApplication

app = QApplication(sys.argv)
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
scene = os.path.join(root, "showcase", "2026-11-02_01_tiles", "scene.usda")
folder = tempfile.mkdtemp(prefix="eldr_tiles_")
W, H, TILE = 1920, 1080, 512
failures = []


def expect(ok, what):
    if not ok:
        failures.append(what)
        print("FAIL:", what, file=sys.stderr)


def render(prim):
    """The settings prim's usdrecord image: rows top down, RGB bytes."""
    path = os.path.join(folder, prim + ".png")
    subprocess.run(["usdrecord", "--renderer", "Eldr", "--disableCameraLight", "--camera", "/cam",
                    "--renderSettingsPrimPath", "/Render/" + prim, "-w", str(W), scene, path],
                   check=True, shell=True, timeout=600)
    image = QImage(path).convertToFormat(QImage.Format_RGB888)
    assert image.width() == W and image.height() == H, (image.width(), image.height())
    data, stride = bytes(image.constBits()), image.bytesPerLine()
    return [data[y * stride:y * stride + 3 * W] for y in range(H)]


tiles, whole = render("tiles"), render("whole")
# Per pixel the mean absolute difference of the channels (0-255).
diff = [[sum(abs(a[3 * x + c] - b[3 * x + c]) for c in range(3)) / 3.0 for x in range(W)]
        for a, b in zip(tiles, whole)]
mean = sum(map(sum, diff)) / (W * H)
columns = [sum(row[x] for row in diff) / H for x in range(W)]
rows = [sum(row) / W for row in diff]  # top down
typical = sorted(columns)[W // 2]
seams = [columns[x] for k in range(1, (W - 1) // TILE + 1) for x in (k * TILE - 1, k * TILE)]
seams += [rows[H - 1 - y] for k in range(1, (H - 1) // TILE + 1) for y in (k * TILE - 1, k * TILE)]
print("tiles vs whole: mean abs diff %.3f / 255, median column %.3f, worst tile edge line %.3f" %
      (mean, typical, max(seams)))
expect(mean < 1.0, "tiled and whole images match within noise (mean abs diff %.3f / 255)" % mean)
expect(max(seams) < 3.0 * max(typical, mean) + 0.5, "no seam along the tile edges (%.3f / 255)" % max(seams))
print("FAILED" if failures else "OK")
sys.exit(1 if failures else 0)
