"""Builds the Eldr docs page: the user guide (docs/guide/*.md) and the reference pages (docs/*.md)
embedded in template.html, one chapter shown at a time, rendered in the browser by marked.
Run: python docs/site/build.py docs <out.html>; the integrator publishes the result as the Eldr
Documentation artifact after each merge that touches docs. Pictures under docs/images are embedded
as data URIs, so the page stays one file; a missing picture (a Houdini screenshot still to take)
shows as a placeholder box."""
import base64
import json
import pathlib
import re
import sys

# (file, nav title, part) in reading order; README is about maintaining the docs.
DOCS = [
    ("guide/getting-started.md", "Getting started", "User guide"),
    ("guide/solaris.md", "Rendering in Solaris", "User guide"),
    ("guide/lights.md", "Lights", "User guide"),
    ("guide/materials.md", "Materials", "User guide"),
    ("guide/sampling.md", "Sampling and denoising", "User guide"),
    ("guide/ray-tracing.md", "Ray-traced effects", "User guide"),
    ("guide/volumes.md", "Volumes", "User guide"),
    ("guide/cameras.md", "Cameras", "User guide"),
    ("guide/aovs.md", "AOVs and output", "User guide"),
    ("guide/performance.md", "Crowds and performance", "User guide"),
    ("guide/viewport.md", "Viewport", "User guide"),
    ("guide/karma.md", "Eldr and Karma", "User guide"),
    ("guide/troubleshooting.md", "Troubleshooting", "User guide"),
    ("settings.md", "Settings reference", "Reference"),
    ("features.md", "How Eldr works", "Reference"),
    ("houdini.md", "Houdini", "Reference"),
    ("materialx.md", "MaterialX coverage", "Reference"),
    ("comparison.md", "Eldr vs EEVEE and Unreal", "Reference"),
    ("goals.md", "Goals", "Reference"),
    ("testing.md", "Testing", "Reference"),
]
MIME = {".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".png": "image/png", ".webp": "image/webp"}

src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])


def embed(md):
    """(../)images/x.jpg -> a data URI when the file exists, else images/x.jpg (a placeholder)."""
    def one(m):
        rel = m.group(2)
        f = src / rel
        if f.is_file() and f.suffix.lower() in MIME:
            rel = "data:%s;base64,%s" % (MIME[f.suffix.lower()], base64.b64encode(f.read_bytes()).decode())
        return m.group(1) + rel + ")"
    return re.sub(r"(!\[[^\]]*\]\()(?:\.\./)?(images/[^)\s]+)\)", one, md)


docs = [{"id": pathlib.Path(f).stem, "title": t, "part": p, "md": embed((src / f).read_text(encoding="utf-8"))}
        for f, t, p in DOCS if (src / f).exists()]
data = json.dumps(docs, ensure_ascii=False).replace("</", "<\\/")
html = (pathlib.Path(__file__).parent / "template.html").read_text(encoding="utf-8")
out.write_text(html.replace("/*DOCS*/[]", data), encoding="utf-8")
print(out, "%d chapters, %.1f MB" % (len(docs), out.stat().st_size / 1e6))
