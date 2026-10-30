"""Renders the user guide's pictures (docs/images/*.jpg) from the showcase scenes.
Run from the repo root: usdenv.cmd python docs\\site\\render_images.py [name ...]
(names: only those images). HDVK_BUILD picks the Eldr build (usdenv's default: build).
Pictures rendered with husk need Houdini (ELDR_HFS, default 22.0.466) and, for Eldr, its Houdini
build (ELDR_H22_BUILD, default build-h22 next to HDVK_BUILD). Images are 480 wide JPEGs."""
import os
import pathlib
import subprocess
import sys
import tempfile

from PySide6.QtGui import QImage

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / "docs" / "images"
USDRECORD = pathlib.Path(os.environ.get("USD_ROOT", r"C:\dev\usd-26.08-vdb")) / "bin" / "usdrecord"
HFS = pathlib.Path(os.environ.get("ELDR_HFS", r"C:\Program Files\Side Effects Software\Houdini 22.0.466"))
H22 = pathlib.Path(os.environ.get("ELDR_H22_BUILD",
                                  pathlib.Path(os.environ.get("HDVK_BUILD", ROOT / "build")).parent / "build-h22"))
WIDTH = 480

# (image, showcase folder suffix, settings prim[, how]): Eldr through usdrecord, or with husk:
# "karma" (Karma CPU) or "husk" (Eldr's Houdini build), side by side in the same framing.
IMAGES = [
    ("hero", "30_22_material_lobes", "eldr_final"),
    ("first_render", "30_10_shadow_modes", "eldr_default"),
    ("solaris_default", "28_03_emitters_shadows_reflections", "eldr_default"),
    ("solaris_final", "28_03_emitters_shadows_reflections", "eldr_final"),
    # Lights
    ("light_shapes", "30_07_light_shapes", "shapes"),
    ("light_shapes_visible", "30_07_light_shapes", "shapesVisible"),
    ("many_lights", "30_06_many_lights", "lights"),
    ("dome_sun_on", "30_08_dome_sun", "domeSun"),
    ("dome_sun_off", "30_08_dome_sun", "domeSunOff"),
    ("linking_on", "29_15_light_linking_ies", "default"),
    ("linking_off", "29_15_light_linking_ies", "noLinking"),
    ("ies_off", "29_15_light_linking_ies", "noIes"),
    ("shadows_maps", "30_10_shadow_modes", "maps"),
    ("shadows_pcss", "30_10_shadow_modes", "pcss"),
    ("shadows_hard", "30_10_shadow_modes", "hard"),
    ("shadows_traced", "30_10_shadow_modes", "traced"),
    ("emissive_lights_on", "28_03_emitters_shadows_reflections", "emissiveLightsOn"),
    ("emissive_lights_off", "28_03_emitters_shadows_reflections", "emissiveLightsOff"),
    ("colour_limit_20", "29_17_karma_colour_limit", "limit20"),
    ("colour_limit_off", "29_17_karma_colour_limit", "fullHdr"),
    # Materials
    ("lobes", "30_22_material_lobes", "lobes"),
    ("lobes_off", "30_22_material_lobes", "lobesOff"),
    ("toon", "30_22_material_lobes", "toon"),
    ("mtlx_graphs_on", "29_16_mtlx_coverage", "graphsOn"),
    ("mtlx_graphs_off", "29_16_mtlx_coverage", "graphsOff"),
    ("sss_on", "30_13_subsurface", "subsurface"),
    ("sss_off", "30_13_subsurface", "subsurfaceOff"),
    ("displacement_on", "30_18_displacement", "displacement"),
    ("displacement_off", "30_18_displacement", "displacementOff"),
    ("textures", "30_21_textures_colour", "textures"),
    ("textures_aniso1", "30_21_textures_colour", "aniso1"),
    # Sampling and denoising
    ("effects_raw4", "29_11_effect_denoise", "raw4"),
    ("effects_denoise4", "29_11_effect_denoise", "denoise4"),
    ("effects_denoise64", "29_11_effect_denoise", "denoise64"),
    ("oidn_raw16", "29_07_oidn_adaptive_tiles", "raw16"),
    ("oidn_denoise16", "29_07_oidn_adaptive_tiles", "denoise16"),
    ("quality_default", "29_12_quality_multipliers", "default"),
    ("quality_diffuse2", "29_12_quality_multipliers", "diffuse2"),
    # Ray-traced effects
    ("gi_default", "28_04_gi_final_gather", "eldr_default"),
    ("gi_probes", "28_04_gi_final_gather", "probesOnly"),
    ("gi_final_gather", "28_04_gi_final_gather", "finalGather"),
    ("gi_ssgi", "28_04_gi_final_gather", "ssgi"),
    ("rt_shadow_maps", "28_03_emitters_shadows_reflections", "shadowMapsOnly"),
    ("rt_shadows", "28_03_emitters_shadows_reflections", "rtShadows"),
    ("rt_reflections", "28_03_emitters_shadows_reflections", "rtReflectionsHitShadows2"),
    ("caustics_on", "29_13_caustics", "caustics"),
    ("caustics_off", "29_13_caustics", "noCaustics"),
    ("glass_default", "29_09_transparency_order", "default"),
    ("glass_final", "29_09_transparency_order", "final"),
    ("ssr_on", "30_11_reflections_ssr", "ssr"),
    ("ssr_off", "30_11_reflections_ssr", "ssrOff"),
    ("reflection_probe", "30_11_reflections_ssr", "probe"),
    ("ao_on", "30_09_ambient", "ambient"),
    ("ao_off", "30_09_ambient", "aoOff"),
    # Volumes
    ("volume_shadows_on", "29_05_coloured_volume_shadows", "maps"),
    ("volume_shadows_off", "29_05_coloured_volume_shadows", "shadowsOff"),
    ("volume_limit1", "29_05_coloured_volume_shadows", "volumeLimit1"),
    ("volume_limit16", "29_05_coloured_volume_shadows", "volumeLimit16"),
    ("volume_default", "29_08_volume_karma_defaults", "newPhase"),
    ("fog_on", "30_12_fog", "fog"),
    ("fog_off", "30_12_fog", "fogOff"),
    ("fog_dense", "30_12_fog", "fogDense"),
    ("volume_blur", "29_18_volume_motion_blur", "blur"),
    ("volume_sharp", "29_18_volume_motion_blur", "sharp"),
    # Cameras
    ("exposure_camera", "30_05_camera_exposure", "camera"),
    ("exposure_ignored", "30_05_camera_exposure", "ignored"),
    ("lens_dof", "30_14_lens_dof", "lens"),
    ("lens_dof_off", "30_14_lens_dof", "dofOff"),
    ("lens_distortion_off", "30_14_lens_dof", "distortionOff"),
    ("motion_blur_on", "28_02_velocity_blur", "blurOn"),
    ("motion_blur_off", "28_02_velocity_blur", "blurOff"),
    ("tonemap_aces", "30_17_bloom_tonemap", "aces"),
    ("tonemap_agx", "30_17_bloom_tonemap", "agx"),
    ("tonemap_linear", "30_17_bloom_tonemap", "linear"),
    ("tonemap_bloom", "30_17_bloom_tonemap", "bloom"),
    # AOVs and output
    ("intensity_neutral", "30_16_light_path_intensity", "neutral"),
    ("intensity_indirect", "30_16_light_path_intensity", "indirectBoost"),
    ("holdouts_on", "28_06_eldr_geometry_tab", "holdoutScene"),
    ("holdouts_off", "28_06_eldr_geometry_tab", "holdoutsOff"),
    # Crowds, geometry and performance
    ("curves_tubes", "30_19_curves_points", "curves"),
    ("curves_ribbons", "30_19_curves_points", "ribbons"),
    ("instancing", "30_20_instancing", "instances"),
    # Eldr next to Karma
    ("karma_lobes", "30_22_material_lobes", "karma", "karma"),
    ("eldr_lobes", "30_22_material_lobes", "eldr_final", "husk"),
    ("karma_light_linking", "29_15_light_linking_ies", "karma", "karma"),
    ("eldr_light_linking", "29_15_light_linking_ies", "eldr_final", "husk"),
    ("karma_subsurface", "30_13_subsurface", "karma", "karma"),
    ("eldr_subsurface", "30_13_subsurface", "eldr_final", "husk"),
]


def folder(suffix):
    return next((ROOT / "showcase").glob("2026-10-" + suffix))


def run(cmd, cwd, env=None):
    """Runs in cwd (the temporary folder), where EXR products some prims write land."""
    print(" ".join(map(str, cmd)), flush=True)
    r = subprocess.run(list(map(str, cmd)), cwd=cwd, env=env)
    if r.returncode:
        print("  failed:", r.returncode, file=sys.stderr)
    return r.returncode == 0


def render(name, suffix, prim, kind="eldr", tmp=None):
    scene = folder(suffix) / "scene.usda"
    png = pathlib.Path(tmp) / (name + ".png")
    if kind in ("karma", "husk"):
        env = dict(os.environ, HOUDINI_USER_PREF_DIR=str(pathlib.Path(tmp) / "hprefs__HVER__"),
                   PXR_AR_DEFAULT_SEARCH_PATH=str(HFS / "bin/usd_plugins/hdx/resources/textures"))
        for k in ("PXR_PLUGINPATH_NAME", "PYTHONPATH"):  # husk must not load the USD 26.08 Eldr
            env.pop(k, None)
        if kind == "husk":  # Eldr's Houdini build: same framing and linear output as Karma's
            env.update(HOUDINI_PACKAGE_DIR=str(H22 / "houdini"), PXR_PLUGINPATH_NAME=str(H22))
        ok = run([HFS / "bin" / "husk.exe", "-R", "BRAY_HdKarma" if kind == "karma" else "HdVkRendererPlugin",
                  "-s", "/Render/" + prim, "--res", WIDTH, WIDTH * 9 // 16, "-o", png, scene], tmp, env)
    else:
        # usdrecord writes a prim's render products instead of the PNG: a layer over the scene drops them.
        layer = pathlib.Path(tmp) / (name + ".usda")
        layer.write_text('#usda 1.0\n(\n    subLayers = [@%s@]\n)\nover "Render"\n{\n    over "%s"\n    {\n'
                         '        rel products = []\n    }\n}\n' % (scene.as_posix(), prim))
        scene = layer
        ok = run([sys.executable, USDRECORD, "--renderer", "Eldr", "--camera", "/cam",
                  "--renderSettingsPrimPath", "/Render/" + prim, "--imageWidth", WIDTH,
                  "--disableCameraLight", scene, png], tmp)
    if ok and png.exists():
        QImage(str(png)).save(str(OUT / (name + ".jpg")), "JPG", 82)
    return ok


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    only = set(sys.argv[1:])
    failed = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, suffix, prim, *kind in IMAGES:
            if not only or name in only:
                if not render(name, suffix, prim, *kind, tmp=tmp):
                    failed.append(name)
    total = sum(f.stat().st_size for f in OUT.rglob("*") if f.is_file())
    print("images: %.2f MB; failed: %s" % (total / 1e6, ", ".join(failed) or "none"))
