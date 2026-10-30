"""The final-render settings (tests/scenes/final_settings.usda's /Render/final):
FINAL as a dict for SetRendererSetting, FINAL_ARGS as exr_record.py's key=value."""
import os

from pxr import Sdf

_layer = Sdf.Layer.FindOrOpen(os.path.join(os.path.dirname(os.path.abspath(__file__)), "scenes", "final_settings.usda"))
FINAL = {p.name: p.default for p in _layer.GetPrimAtPath("/Render/final").properties if p.name.startswith("eldr:")}
FINAL_ARGS = ["%s=%s" % (k, str(v).lower() if isinstance(v, bool) else v) for k, v in FINAL.items()]
