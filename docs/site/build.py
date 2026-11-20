"""Builds the Eldr docs page: the user guide (docs/guide/*.md) and the reference pages (docs/*.md)
embedded in template.html, one chapter shown at a time, rendered in the browser by marked.
Run: python docs/site/build.py docs <out.html>; the integrator publishes the result as the Eldr
Documentation artifact after each merge that touches docs. Pictures under docs/images are embedded
as data URIs, so the page stays one file; a missing picture (a Houdini screenshot still to take)
shows as a placeholder box. The settings reference is generated from docs/settings.md's tables and
hydra/renderSettings.def: one card per setting, anchored #eldr-<name>. Plain Python 3, no packages."""
import base64
import html as H
import json
import pathlib
import re
import subprocess
import sys

# (file, nav title, part) in reading order.
DOCS = [
    ("index.md", "Home", "Eldr"),
    ("guide/getting-started.md", "Getting started", "User guide"),
    ("guide/tutorial.md", "Tutorial: your first scene", "User guide"),
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
    ("gallery.md", "Gallery", "User guide"),
    ("guide/showcase.md", "Showcase scenes", "User guide"),
    ("guide/troubleshooting.md", "Troubleshooting", "User guide"),
    ("guide/faq.md", "FAQ", "User guide"),
    ("settings.md", "Settings reference", "Reference"),
    ("features.md", "How Eldr works", "Reference"),
    ("houdini.md", "Houdini", "Reference"),
    ("environment.md", "Environment variables", "Reference"),
    ("materialx.md", "MaterialX coverage", "Reference"),
    ("comparison.md", "Eldr vs EEVEE and Unreal", "Reference"),
    ("goals.md", "Goals", "Reference"),
    ("developer.md", "Developer guide", "Developer"),
    ("testing.md", "Testing", "Developer"),
    ("README.md", "Writing the docs", "Developer"),
]
MIME = {".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".png": "image/png", ".webp": "image/webp", ".svg": "image/svg+xml"}
CHIPS = {"[RT]": ("rt", "Needs ray tracing"), "[film]": ("film", "Still camera and finals"),
         "[interactive]": ("interactive", "While moving only"), "[prepass]": ("prepass", "Needs Depth Prepass")}
ROW = re.compile(r"\| (.*?)<br>`(eldr:[\w:]+)` \| (.*?) \| (.*?) \|(.*)\|\s*$")


def embed(md, src):
    """(../)images/x.jpg -> a data URI when the file exists, else images/x.jpg (a placeholder)."""
    def one(m):
        rel = m.group(2)
        f = src / rel
        if f.is_file() and f.suffix.lower() in MIME:
            rel = "data:%s;base64,%s" % (MIME[f.suffix.lower()], base64.b64encode(f.read_bytes()).decode())
        return m.group(1) + rel + ")"
    return re.sub(r"(!\[[^\]]*\]\()(?:\.\./)?(images/[^)\s]+)\)", one, md)


def inline(md):
    """The inline markdown settings.md's cells use: `code`, **bold**, [text](link)."""
    parts = re.split(r"(`[^`]*`)", md)
    out = []
    for p in parts:
        if p.startswith("`") and p.endswith("`") and len(p) > 1:
            out.append("<code>%s</code>" % H.escape(p[1:-1]))
            continue
        p = H.escape(p, quote=False).replace("&lt;br&gt;", "<br>")
        p = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", p)
        p = re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", lambda m: '<a href="%s">%s</a>' % (H.escape(m.group(2)), m.group(1)), p)
        out.append(p)
    return "".join(out)


def read_def(path):
    """{name: (group, open, usdview label)} from renderSettings.def; {} when it's not there."""
    if not path.is_file():
        return {}
    out, group = {}, ("", False)
    for m in re.finditer(r'^HDVK_GROUP\("([^"]+)", (\w+)\)|^HDVK_(?:VDB_)?SETTING\((\w+), "([^"]*)"',
                         path.read_text(encoding="utf-8"), re.M):
        if m.group(1):
            group = (m.group(1), m.group(2) == "true")
        else:
            out["eldr:" + m.group(3).replace("__", ":")] = group + (m.group(4),)
    return out


def card(label, key, default, what, cost, info):
    """One setting: heading, metadata, tooltip text, cost."""
    group, _, uilabel = info.get(key, ("", False, label))
    chips = [c for c in CHIPS if c in what]
    for c in chips:
        what = what.replace(c, "")
    paren = re.search(r"\(([^()]*)\)\s*$", uilabel)
    paren = paren.group(1) if paren else ""
    kind = default.split()[0] if default else ""
    kind = "flag" if kind in ("on", "off") else kind
    rows = [("Default", inline(default)), ("Type", kind)]
    if kind == "string" and "," in paren:
        rows.append(("Values", ", ".join("<code>%s</code>" % H.escape(v.strip()) for v in paren.split(","))))
    elif re.fullmatch(r"[0-9.]+-[0-9.]+", paren):
        rows.append(("Range", paren.replace("-", " to ")))
    elif paren and kind != "flag":
        rows.append(("Format" if paren in ("r g b", "x y z") else "Unit", H.escape(paren)))
    karma = sorted(set(re.findall(r"`(karma:[\w:]+)`", what)))
    usd = "<code>%s</code>" % key + "".join(" <span class=\"twin\">Karma: <code>%s</code></span>" % k for k in karma)
    houdini = "Render Settings LOP &rsaquo; Eldr &rsaquo; %s &rsaquo; <strong>%s</strong>" % (
        H.escape(group or "Other"), H.escape(re.sub(r"\s*\([^()]*\)", "", uilabel)))
    rows += [("USD attribute", usd), ("Houdini", houdini)]
    sid = key.replace(":", "-")
    return ('<div class="setting" id="%s">\n<h4><a class="anchor" href="#%s">%s</a>%s</h4>\n'
            '<dl class="meta">%s</dl>\n<div class="what">%s</div>\n%s</div>\n') % (
        sid, sid, H.escape(label), "".join('<span class="chip %s" title="%s">%s</span>' % (CHIPS[c][0], CHIPS[c][1], c.strip("[]"))
                                         for c in chips),
        "".join("<dt>%s</dt><dd>%s</dd>" % r for r in rows), inline(what.strip()),
        '<div class="cost"><span>Cost</span> %s</div>\n' % inline(cost.strip()) if cost.strip() else "")


def settings_view(md, info):
    """settings.md with each table of settings replaced by its cards, folded as renderSettings.def folds
    the group; the count goes under the title."""
    lines, out, n, i = md.splitlines(), [], 0, 0
    while i < len(lines):
        if lines[i].startswith("| Setting |") and i + 2 < len(lines) and ROW.match(lines[i + 2]):
            i += 2
            cards, opened = [], True
            while i < len(lines) and ROW.match(lines[i]):
                m = ROW.match(lines[i])
                cards.append(card(m.group(1), m.group(2), m.group(3).strip(), m.group(4), m.group(5), info))
                opened = info.get(m.group(2), ("", True))[1]
                i += 1
            n += len(cards)
            out.append('<details class="sgroup"%s><summary>%d settings</summary>\n%s</details>\n' % (
                " open" if opened else "", len(cards), "".join(cards)))
            continue
        out.append(lines[i])
        i += 1
    text = "\n".join(out)
    return text.replace("\n", "\n\nEvery one of the %d render settings has its own card below, linkable as "
                        "`#eldr-<name>`: default, values, USD attribute, where it is in Houdini, then what it does "
                        "and what it costs.\n" % n, 1), n


def stamp(src):
    """'Built from <commit>, <date>' for the sidebar, so a reader can match the docs to a build; '' without git."""
    try:
        sha, date = subprocess.run(["git", "-C", str(src), "log", "-1", "--format=%h %cs"], capture_output=True,
                                   text=True, check=True).stdout.split()
    except (OSError, subprocess.CalledProcessError, ValueError):
        return ""
    return "<small>Built from %s, %s</small>" % (H.escape(sha), H.escape(date))


def main(src, out):
    info = read_def(src.parent / "hydra" / "renderSettings.def")
    docs, count = [], 0
    for f, t, p in DOCS:
        if not (src / f).exists():
            continue
        md = embed((src / f).read_text(encoding="utf-8"), src)
        if f == "settings.md":
            md, count = settings_view(md, info)
        docs.append({"id": pathlib.Path(f).stem, "title": t, "part": p, "md": md})
    data = json.dumps(docs, ensure_ascii=False).replace("</", "<\\/")
    page = (pathlib.Path(__file__).parent / "template.html").read_text(encoding="utf-8")
    page = page.replace("<!--BUILT-->", stamp(src))
    out.write_text(page.replace("/*DOCS*/[]", data), encoding="utf-8")
    print(out, "%d chapters, %d settings, %.1f MB" % (len(docs), count, out.stat().st_size / 1e6))


if __name__ == "__main__":
    main(pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]))
