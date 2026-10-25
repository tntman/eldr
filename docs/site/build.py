"""Builds the Eldr docs page: docs/*.md embedded in template.html, rendered
in the browser by marked. Run: python docs/site/build.py docs <out.html>; the integrator publishes
the result as the Eldr Documentation artifact after each merge that touches docs."""
import json
import pathlib
import sys

# (file, nav title) in reading order; README is about maintaining the docs.
DOCS = [
    ("goals.md", "Goals"),
    ("features.md", "How Eldr works"),
    ("settings.md", "Render settings"),
    ("houdini.md", "Houdini"),
    ("eevee-lardomar.md", "EEVEE study (Swedish)"),
]

src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
docs = [{"id": f[:-3], "title": t, "md": (src / f).read_text(encoding="utf-8")}
        for f, t in DOCS if (src / f).exists()]
data = json.dumps(docs, ensure_ascii=False).replace("</", "<\\/")
html = (pathlib.Path(__file__).parent / "template.html").read_text(encoding="utf-8")
out.write_text(html.replace("/*DOCS*/[]", data), encoding="utf-8")
print(out, sum(len(d["md"]) for d in docs), "chars")
