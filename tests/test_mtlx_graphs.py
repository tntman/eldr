"""MaterialX graphs Eldr compiles whole give MaterialX's values (the wrong results tests/mtlx_corpus.py
found against Karma and Storm): small inline graphs, each on a sphere as standard_surface emission
(unlit, so the pixels are the graph's value), rendered by Eldr alone, compared with the numbers
the stdlib definitions give.
Run (HDVK_BUILD = the build with hdVk.dll): usdenv.cmd python tests\\test_mtlx_graphs.py"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mtlx_corpus as corpus  # noqa: E402

from PySide6.QtGui import QColor, QImage  # noqa: E402

FLAT = (51, 102, 153)  # the flat image, 0.2 0.4 0.6 (a MaterialX file is linear)


def value(typ, v, name="in"):
    return '<input name="%s" type="%s" value="%s"/>' % (name, typ, v)


def link(typ, node, name="in"):
    return '<input name="%s" type="%s" nodename="%s"/>' % (name, typ, node)


def node(category, name, typ, *inputs):
    return '<%s name="%s" type="%s">%s</%s>' % (category, name, typ, "".join(inputs), category)


def to_color(typ, source):
    """The nodes that bring `source` (a node of type typ) to the color3 the emission reads."""
    return [node("convert", "final", "color3", link(typ, source))]


def cases(image, halves):
    f = lambda *v: ", ".join(str(x) for x in v)
    tests = []  # (name, nodes, expected rgb, tolerance)

    def case(name, nodes, rgb, tol=0.02):
        tests.append((name, nodes, rgb, tol))  # tol > 0.02: not uniform, the mean only

    # 1. vector2 to vector3 / color3: the third component is 0.
    case("vector2 + float, to color3",
         [node("add", "a", "vector2", value("vector2", f(0.1, 0.2), "in1"), value("float", 0.3, "in2"))] +
         to_color("vector2", "a"), (0.4, 0.5, 0.0))
    case("image vector2, to color3",
         [node("image", "i", "vector2", value("filename", image, "file"))] + to_color("vector2", "i"),
         (0.2, 0.4, 0.0))
    # 2. combine of vectors unpacks them.
    case("combine2 vector4 VV",
         [node("combine2", "c", "vector4", value("vector2", f(0.1, 0.2), "in1"), value("vector2", f(0.3, 0.4), "in2")),
          node("convert", "final", "color3", link("vector4", "c"))], (0.1, 0.2, 0.3))
    case("combine2 vector4 VF, w",
         [node("combine2", "c", "vector4", value("vector3", f(0.1, 0.2, 0.3), "in1"), value("float", 0.4, "in2")),
          node("extract", "e", "float", link("vector4", "c"), value("integer", 3, "index"))] +
         to_color("float", "e"), (0.4, 0.4, 0.4))
    case("combine2 color4 CF",
         [node("combine2", "c", "color4", value("color3", f(0.1, 0.2, 0.3), "in1"), value("float", 0.4, "in2")),
          node("convert", "final", "color3", link("color4", "c"))], (0.1, 0.2, 0.3))
    # 3. vector4 dot and normalize use all four components.
    case("dotproduct vector4",
         [node("dotproduct", "d", "float", value("vector4", f(1, 2, 3, 4), "in1"),
               value("vector4", f(0.01, 0.02, 0.03, 0.04), "in2"))] + to_color("float", "d"), (0.3, 0.3, 0.3))
    case("normalize vector4, w",
         [node("normalize", "n", "vector4", value("vector4", f(1, 1, 1, 1))),
          node("extract", "e", "float", link("vector4", "n"), value("integer", 3, "index"))] +
         to_color("float", "e"), (0.5, 0.5, 0.5))
    # 4. vector2 power and modulo.
    case("power vector2",
         [node("power", "p", "vector2", value("vector2", f(0.5, 0.25), "in1"), value("vector2", f(2, 2), "in2"))] +
         to_color("vector2", "p"), (0.25, 0.0625, 0.0))
    case("modulo vector2",
         [node("modulo", "m", "vector2", value("vector2", f(0.75, 0.6), "in1"), value("vector2", f(0.5, 0.5), "in2"))] +
         to_color("vector2", "m"), (0.25, 0.1, 0.0))
    # 5. boolean comparisons.
    for category, v1, v2, expected in (("ifgreater", 2, 1, 1.0), ("ifgreatereq", 1, 2, 0.0), ("ifequal", 1, 1, 1.0)):
        case("%s boolean" % category,
             [node(category, "b", "boolean", value("float", v1, "value1"), value("float", v2, "value2"))] +
             [node("convert", "f", "float", link("boolean", "b"))] + to_color("float", "f"), (expected,) * 3)
    case("ifequal color3 with boolean values",
         [node("ifequal", "final", "color3", value("color3", f(0.3, 0.6, 0.9), "in1"), value("color3", f(0, 0, 0), "in2"),
               value("boolean", "true", "value1"), value("boolean", "true", "value2"))], (0.3, 0.6, 0.9))
    # 6. a float tiledimage is its red channel.
    case("float tiledimage, straight into the input",
         [node("tiledimage", "t", "float", value("filename", image, "file"))] + to_color("float", "t"),
         (0.2, 0.2, 0.2))
    # 7. a missing vector2 primvar is its default (the mesh's uv is not).
    case("geompropvalue vector2, missing",
         [node("geompropvalue", "p", "vector2", value("string", "nothere", "geomprop"),
               value("vector2", f(0.3, 0.6), "default"))] + to_color("vector2", "p"), (0.3, 0.6, 0.0))
    # 8. address mode constant: the image's default outside [0, 1].
    for u, expected in ((1.5, (1.0, 0.0, 0.0)), (0.5, (0.2, 0.4, 0.6))):
        case("image address mode constant, u = %s" % u,
             [node("constant", "uv", "vector2", value("vector2", f(u, 0.5), "value")),
              node("image", "final", "color3", value("filename", image, "file"), value("color3", f(1, 0, 0), "default"),
                   link("vector2", "uv", "texcoord"), value("string", "constant", "uaddressmode"))], expected)
    case("image at a constant uv (right half)",
         [node("constant", "uv", "vector2", value("vector2", f(0.75, 0.5), "value")),
          node("image", "final", "color3", value("filename", halves, "file"), link("vector2", "uv", "texcoord"))],
         (200 / 255, 100 / 255, 50 / 255))
    case("image address mode periodic, u = 1.5",
         [node("constant", "uv", "vector2", value("vector2", f(1.5, 0.5), "value")),
          node("image", "final", "color3", value("filename", image, "file"), value("color3", f(1, 0, 0), "default"),
               link("vector2", "uv", "texcoord"))], (0.2, 0.4, 0.6))
    # 9. a normalmap used as a colour is MaterialX's world-space normal. A flat map on the sphere seen
    # from +z: the mean of max(N, 0) over the disk of radius 0.8 the cells average.
    case("normalmap as a colour (world-space normal)",
         [node("normalmap", "n", "vector3", value("vector3", f(0.5, 0.5, 1)))] + to_color("vector3", "n"),
         (0.17, 0.17, 0.817), 0.04)
    return tests


def document(tests):
    out = ['<?xml version="1.0"?>', '<materialx version="1.39">']
    for i, (name, nodes, _, _) in enumerate(tests):
        out.append('<nodegraph name="g%d">%s<output name="out" type="color3" nodename="final"/></nodegraph>' %
                   (i, "".join(nodes)))
        out.append('<standard_surface name="s%d" type="surfaceshader">'
                   '<input name="base" type="float" value="0"/><input name="specular" type="float" value="0"/>'
                   '<input name="emission" type="float" value="1"/>'
                   '<input name="emission_color" type="color3" nodegraph="g%d" output="out"/></standard_surface>' %
                   (i, i))
        out.append('<surfacematerial name="m%d" type="material">'
                   '<input name="surfaceshader" type="surfaceshader" nodename="s%d"/></surfacematerial>' % (i, i))
    # 10. a surface of BSDFs only renders the default grey (and Eldr says it doesn't read it).
    out.append('<oren_nayar_diffuse_bsdf name="bsdf" type="BSDF"><input name="color" type="color3" '
               'value="0.8, 0.8, 0.8"/></oren_nayar_diffuse_bsdf>')
    out.append('<surface name="sb" type="surfaceshader"><input name="bsdf" type="BSDF" nodename="bsdf"/></surface>')
    out.append('<surfacematerial name="mb" type="material">'
               '<input name="surfaceshader" type="surfaceshader" nodename="sb"/></surfacematerial>')
    out.append("</materialx>")
    return "\n".join(out)


def main():
    folder = tempfile.mkdtemp(prefix="mtlx_graphs_")
    image = os.path.join(folder, "flat.png").replace("\\", "/")
    img = QImage(4, 4, QImage.Format_RGB888)
    img.fill(QColor(*FLAT))
    assert img.save(image)
    halves = os.path.join(folder, "halves.png").replace("\\", "/")  # left FLAT, right 200 100 50
    img = QImage(8, 4, QImage.Format_RGB888)
    img.fill(QColor(*FLAT))
    for x in range(4, 8):
        for y in range(4):
            img.setPixelColor(x, y, QColor(200, 100, 50))
    assert img.save(halves)
    tests = cases(image, halves)
    src = os.path.join(folder, "graphs.mtlx")
    with open(src, "w") as f:
        f.write(document(tests))
    cells = [(src, "/MaterialX", "Materials/m%d" % i) for i in range(len(tests))] + [(src, "/MaterialX", "Materials/mb")]
    scene, cols = corpus.scene(folder, "graphs", cells)
    exr, _, rc, err = corpus.run_eldr(folder, "graphs", scene, cols)
    assert rc == 0, err
    got = corpus.read_exr(folder, exr)
    failures = []
    for i, (name, _, rgb, tol) in enumerate(tests):
        mean, px = corpus.cell_stats(got, i, cols)
        spread = abs(px - mean).max()
        ok = all(abs(m - e) <= tol for m, e in zip(mean, rgb)) and (tol > 0.02 or spread <= tol)
        print("%s %-44s expected %s got %s" % ("ok  " if ok else "FAIL", name, rgb, tuple(round(float(m), 3) for m in mean)))
        if not ok:
            failures.append(name)
    mean, px = corpus.cell_stats(got, len(tests), cols)
    ok = float(mean.min()) > 0.02 and float(mean.max() - mean.min()) < 0.01  # lit default grey, not black
    print("%s %-44s expected grey, got %s" % ("ok  " if ok else "FAIL", "surface of BSDFs only",
                                             tuple(round(float(m), 3) for m in mean)))
    if not ok:
        failures.append("surface of BSDFs only")
    if "surface shader 'ND_surface' isn't supported" not in err:
        print("FAIL: no warning for the unsupported surface shader")
        failures.append("warning")
    print("%d failed" % len(failures) if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
