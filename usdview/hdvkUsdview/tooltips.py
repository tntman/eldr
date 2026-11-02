"""Tooltips of Eldr's render settings: the "What it does" column of
docs/settings.md, markdown stripped; and their groups, from
hydra/renderSettings.def. Used by usdview's settings window and by
houdini/gen_ui.py (loaded by path, so no Qt or pxr imports here), so the docs
and both UIs share one text. tests/test_docs.py limits their length."""
import os
import re

MAX = 260  # characters; longer rows get shortened in the docs
MARKS = {"[film]": "(film only)", "[interactive]": "(interactive only)",
         "[RT]": "(needs ray tracing)", "[prepass]": "(needs depth prepass)"}


def Find(here=None, name="settings.md", source="docs"):
    """`name` next to this file (a build copy), else the source tree's."""
    here = here or os.path.dirname(os.path.abspath(__file__))
    for p in (os.path.join(here, name), os.path.join(here, "..", "..", source, name)):
        if os.path.exists(p):
            return p
    return None


def ReadGroups(path=None):
    """[(title, open, "key key ...")] of renderSettings.def, in its order."""
    with open(path or Find(name="renderSettings.def", source="hydra"), encoding="utf-8") as f:
        text = f.read()
    out = []
    for m in re.finditer(r'^HDVK_GROUP\("([^"]+)", (\w+)\)|^HDVK_(?:VDB_)?SETTING\((\w+),', text, re.M):
        if m.group(1):
            out.append((m.group(1), m.group(2) == "true", []))
        else:
            out[-1][2].append(m.group(3).replace("__", ":"))  # object__volumelimit: object:volumelimit
    return [(title, isOpen, " ".join(keys)) for title, isOpen, keys in out]


def Plain(text):
    for k, v in MARKS.items():
        text = text.replace(k, v)
    return re.sub(r"\s+", " ", text.replace("`", "").replace("**", "").replace("<br>", " ")).strip()


def Tooltips(path=None):
    """{"eldr:key": "plain text"}; {} when the docs are not found."""
    path = path or Find()
    out = {}
    if path:
        with open(path, encoding="utf-8") as f:
            for line in f:
                m = re.match(r"\| [^|]*<br>`(eldr:[\w:]+)` \| (.*?) \| (.*?) \|", line)
                if m:
                    out[m.group(1)] = Plain(m.group(3))
    return out
