cmake_minimum_required(VERSION 3.24)
project(hdvk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # Same consumer-relevant flags USD itself builds with (cmake/defaults/msvcdefaults.cmake).
    add_compile_options(/permissive- /Zc:__cplusplus /Zc:inline)
    add_compile_definitions(NOMINMAX)
endif()

find_package(Vulkan REQUIRED)
# garch (GPU sharing with usdview's OpenGL) links OpenGL::GL.
find_package(OpenGL REQUIRED)
# -DCMAKE_PREFIX_PATH=<USD install root>; pxr_DIR alone misses its TBB/OpenSubdiv configs.
option(HDVK_HOUDINI "Build against Houdini's USD (see docs/houdini.md)" OFF)
if(HDVK_HOUDINI)
    include(houdini/houdini.cmake)
else()
    find_package(pxr CONFIG REQUIRED)
endif()

# Renderer layer: plain Vulkan, must never include USD headers.
# Shaders compile to SPIR-V word lists that renderer.cpp #includes.
# third_party/nanovdb: PNanoVDB.h of OpenVDB 10.1.0 (MPL-2.0, unmodified),
# NanoVDB's GPU reader, for volume.frag and the CPU side of the volumes.
foreach(shader mesh.vert mesh.frag mask.frag fullscreen.vert sky.frag cull.comp lights.comp skin.comp gtao.frag
               gtao_denoise.frag
               taa.frag lens_tiles.comp dof.frag motion_blur.frag motion_residual.frag histogram.comp exposure.comp bloom_down.frag bloom_up.frag
               tonemap.frag distortion.frag aov.frag aov.comp tlas.comp gi_update.comp gather_denoise.comp oit_resolve.frag
               shadow_tint.frag matte.frag subsurface_blur.frag fog_inject.comp fog_integrate.comp volume.frag
               volume_upsample.frag volume_shadow.frag shadow_min.comp rt_skin.comp rt_displace.comp film.comp
               shutter.comp light_shapes.frag background_unscale.comp)
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/${shader}.inc)
    add_custom_command(OUTPUT ${spv}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -I ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanovdb
                -o ${spv} ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/${shader}
        DEPENDS renderer/shaders/${shader} renderer/shaders/frame.glsl renderer/shaders/post.glsl
                renderer/shaders/scene.glsl renderer/shaders/lights.glsl renderer/shaders/material.glsl
                renderer/shaders/lens.glsl renderer/shaders/program.glsl renderer/shaders/noise.glsl
                renderer/shaders/brdf.glsl renderer/shaders/gi.glsl renderer/shaders/transparency.glsl
                renderer/shaders/subsurface.glsl
                renderer/shaders/fog.glsl renderer/shaders/probes.glsl third_party/nanovdb/PNanoVDB.h
                renderer/shaders/displace.glsl renderer/shaders/skinning.glsl renderer/shaders/volume.glsl)
    list(APPEND shader_incs ${spv})
endforeach()
# Ray tracing stages (raytracing.cpp), only used with Device::rayTracing.
foreach(shader rt_pixels.rgen rt_probes.rgen rt_refraction.rgen rt.rmiss rt_shadow.rmiss rt.rchit rt.rahit
        rt_shadow.rahit)
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/${shader}.inc)
    add_custom_command(OUTPUT ${spv}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -I ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nanovdb
                -o ${spv} ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/${shader}
        DEPENDS renderer/shaders/${shader} renderer/shaders/frame.glsl renderer/shaders/scene.glsl
                renderer/shaders/lights.glsl renderer/shaders/material.glsl renderer/shaders/rt.glsl
                renderer/shaders/rt_shade.glsl renderer/shaders/gi.glsl renderer/shaders/volume.glsl
                renderer/shaders/program.glsl renderer/shaders/noise.glsl
                third_party/nanovdb/PNanoVDB.h)
    list(APPEND shader_incs ${spv})
endforeach()
# Material programs' shading pass (GRAPH): mesh.vert exporting vertex-rate
# primvars, mesh.frag with the interpreter; other materials never run it.
# Extra AOVs' G-buffer pass likewise (aov.frag).
# Screen-space GI (gtao.cpp): the GTAO passes built with SSGI.
foreach(shader gtao.frag gtao_denoise.frag)
    string(REPLACE "gtao" "ssgi" variant ${shader})
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/${variant}.inc)
    add_custom_command(OUTPUT ${spv}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -DSSGI -o ${spv}
                ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/${shader}
        DEPENDS renderer/shaders/${shader})
    list(APPEND shader_incs ${spv})
endforeach()
foreach(pass 0 1 2)  # denoised SSR (ssr.cpp): its three passes
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/ssr_denoise${pass}.frag.inc)
    add_custom_command(OUTPUT ${spv}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -DPASS=${pass} -o ${spv}
                ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/ssr_denoise.frag
        DEPENDS renderer/shaders/ssr_denoise.frag)
    list(APPEND shader_incs ${spv})
endforeach()
set(spv ${CMAKE_CURRENT_BINARY_DIR}/ssgi_bitmask.frag.inc)  # SSGI with a visibility bitmask
add_custom_command(OUTPUT ${spv}
    COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -DSSGI -DBITMASK -o ${spv}
            ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/gtao.frag
    DEPENDS renderer/shaders/gtao.frag)
list(APPEND shader_incs ${spv})
foreach(shader mesh.vert mesh.frag aov.frag matte.frag)
    string(REGEX REPLACE "^([a-z]+)\\." "\\1_graph." graph ${shader})
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/${graph}.inc)
    add_custom_command(OUTPUT ${spv}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -DGRAPH -o ${spv}
                ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/${shader}
        DEPENDS renderer/shaders/${shader} renderer/shaders/frame.glsl renderer/shaders/scene.glsl
                renderer/shaders/lights.glsl renderer/shaders/material.glsl renderer/shaders/program.glsl
                renderer/shaders/noise.glsl renderer/shaders/brdf.glsl renderer/shaders/gi.glsl
                renderer/shaders/transparency.glsl renderer/shaders/subsurface.glsl
                renderer/shaders/fog.glsl renderer/shaders/probes.glsl renderer/shaders/displace.glsl
                renderer/shaders/skinning.glsl)
    list(APPEND shader_incs ${spv})
endforeach()
# Materials with lobes beyond the metallic base (LAYERS, brdf.glsl) shade
# with variants of their own, so plain materials don't pay for them.
foreach(variant layers graph_layers)
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/mesh_${variant}.frag.inc)
    set(defines -DLAYERS)
    if(variant STREQUAL "graph_layers")
        list(APPEND defines -DGRAPH)
    endif()
    add_custom_command(OUTPUT ${spv}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c ${defines} -o ${spv}
                ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/mesh.frag
        DEPENDS renderer/shaders/mesh.frag renderer/shaders/frame.glsl renderer/shaders/scene.glsl
                renderer/shaders/lights.glsl renderer/shaders/material.glsl renderer/shaders/program.glsl
                renderer/shaders/noise.glsl renderer/shaders/brdf.glsl renderer/shaders/gi.glsl
                renderer/shaders/transparency.glsl renderer/shaders/subsurface.glsl
                renderer/shaders/fog.glsl renderer/shaders/probes.glsl renderer/shaders/displace.glsl)
    list(APPEND shader_incs ${spv})
endforeach()
# Upscaling (TAAU): taa.frag reconstructing a larger history (UPSCALE).
set(spv ${CMAKE_CURRENT_BINARY_DIR}/taa_upscale.frag.inc)
add_custom_command(OUTPUT ${spv}
    COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -DUPSCALE -o ${spv}
            ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/taa.frag
    DEPENDS renderer/shaders/taa.frag)
list(APPEND shader_incs ${spv})
# The depth passes' vertex stage: mesh.vert exporting only what mask.frag reads.
set(spv ${CMAKE_CURRENT_BINARY_DIR}/mesh_depth.vert.inc)
add_custom_command(OUTPUT ${spv}
    COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -DDEPTH_ONLY -o ${spv}
            ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/mesh.vert
    DEPENDS renderer/shaders/mesh.vert renderer/shaders/scene.glsl renderer/shaders/frame.glsl
            renderer/shaders/displace.glsl renderer/shaders/skinning.glsl renderer/shaders/material.glsl
            renderer/shaders/program.glsl renderer/shaders/noise.glsl)
list(APPEND shader_incs ${spv})

add_library(vkrenderer STATIC
    renderer/device.cpp renderer/scene.cpp renderer/renderer.cpp renderer/ibl.cpp
    renderer/gtao.cpp renderer/post.cpp renderer/taa.cpp renderer/lens.cpp renderer/fullscreen.cpp
    renderer/distortion.cpp renderer/aov.cpp renderer/interop.cpp renderer/raytracing.cpp
    renderer/transparency.cpp renderer/subsurface.cpp renderer/ssr.cpp
    renderer/fog.cpp renderer/probes.cpp renderer/volume.cpp renderer/lod.cpp
    ${shader_incs})
target_include_directories(vkrenderer PUBLIC renderer third_party/nanovdb PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(vkrenderer PUBLIC Vulkan::Vulkan)

add_executable(vkrtest renderer/vkrtest.cpp)
target_link_libraries(vkrtest PRIVATE vkrenderer)
if(MSVC)
    # main() holds every test's locals; under the validation layer the 1 MB
    # default stack overflows.
    target_link_options(vkrtest PRIVATE /STACK:8388608)
endif()

# Hydra layer.
add_library(hdVk SHARED
    hydra/rendererPlugin.cpp hydra/renderDelegate.cpp hydra/processMemory.cpp hydra/renderPass.cpp
    hydra/mesh.cpp hydra/curves.cpp hydra/material.cpp hydra/light.cpp hydra/instancer.cpp
    hydra/sceneIndexPlugin.cpp hydra/glInterop.cpp hydra/matteSceneIndex.cpp hydra/subdivision.cpp
    hydra/camera.cpp hydra/renderProducts.cpp)
target_link_libraries(hdVk PRIVATE ar cameraUtil garch hd hdMtlx hdsi hgi hio pxOsd sdr vkrenderer)
target_compile_definitions(hdVk PRIVATE MFB_PACKAGE_NAME=hdVk MFB_ALT_PACKAGE_NAME=hdVk)
# OpenVDB volumes when USD was built with OpenVDB (build_usd.py --openvdb,
# which installs OpenVDB and its NanoVDB headers beside USD): the volume
# rprim and openvdbAsset fields, grids converted to NanoVDB on load.
# Without, HdVk has no volume prims.
find_path(HDVK_NANOVDB_INCLUDE nanovdb/util/OpenToNanoVDB.h)
if(TARGET hioOpenVDB AND HDVK_NANOVDB_INCLUDE)
    message(STATUS "HdVk: volumes (OpenVDB) on")
    target_sources(hdVk PRIVATE hydra/volume.cpp hydra/field.cpp)
    # _USE_MATH_DEFINES: OpenVDB's headers use M_PI (as hioOpenVDB builds).
    # NANOVDB_USE_TBB: without it NanoVDB's conversion runs on raw std::threads
    # (nested: ~100 per grid), where any exception, a failed thread start
    # included, terminates the host (Houdini crashed playing a VDB sequence).
    target_compile_definitions(hdVk PRIVATE HDVK_VOLUMES _USE_MATH_DEFINES NANOVDB_USE_TBB)
    target_include_directories(hdVk PRIVATE ${HDVK_NANOVDB_INCLUDE})
    target_link_libraries(hdVk PRIVATE hioOpenVDB)
    # Writes tests/scenes/volume.vdb.
    add_executable(make_volume tests/make_volume.cpp)
    target_compile_definitions(make_volume PRIVATE _USE_MATH_DEFINES)
    target_link_libraries(make_volume PRIVATE hioOpenVDB)
    set(HDVK_VOLUMES ON)
elseif(HDVK_HOUDINI)
    # Houdini's USD has no hioOpenVDB: houdini/field.cpp reads the grids with
    # Houdini's own OpenVDB (openvdb_sesi) and NanoVDB headers instead.
    message(STATUS "HdVk: volumes (Houdini's OpenVDB) on")
    target_sources(hdVk PRIVATE hydra/volume.cpp hydra/field.cpp houdini/field.cpp)
    target_compile_definitions(hdVk PRIVATE HDVK_VOLUMES HDVK_HOUDINI NANOVDB_USE_OPENVDB NANOVDB_USE_TBB)
    target_include_directories(hdVk PRIVATE hydra)
    set(HDVK_VOLUMES ON)
else()
    message(STATUS "HdVk: volumes off (USD without OpenVDB)")
endif()
# Karma's lens materials as camera lens distortion: a UsdImaging adapter,
# in a library of its own so hdVk itself doesn't link UsdImaging.
add_library(hdVkUsdImaging SHARED hydra/karmaLensAdapter.cpp)
target_link_libraries(hdVkUsdImaging PRIVATE hd usd usdGeom usdImaging)
target_compile_definitions(hdVkUsdImaging PRIVATE MFB_PACKAGE_NAME=hdVkUsdImaging
                           MFB_ALT_PACKAGE_NAME=hdVkUsdImaging)
add_dependencies(hdVk hdVkUsdImaging)
# Render products (UsdRender) as EXR files through the OpenImageIO USD was
# built with; without it they aren't written.
find_package(OpenImageIO CONFIG QUIET)
if(TARGET OpenImageIO::OpenImageIO)
    target_compile_definitions(hdVk PRIVATE HDVK_EXR)
    target_link_libraries(hdVk PRIVATE OpenImageIO::OpenImageIO)
    # Prints an image's channels, metadata and pixels (tests/test_exr.py).
    add_executable(exrinfo tests/exrinfo.cpp)
    target_link_libraries(exrinfo PRIVATE OpenImageIO::OpenImageIO)
    set(HDVK_EXR ON)
endif()
# plugInfo.json goes next to the dll; PXR_PLUGINPATH_NAME=<that dir>/ finds it.
file(GENERATE OUTPUT $<TARGET_FILE_DIR:hdVk>/plugInfo.json INPUT plugInfo.json)
# usdview's HdVk menu (render statistics, attribute editor), in
# <that dir>/usdview: usdenv.cmd adds it to the plugin and Python paths.
file(GENERATE OUTPUT $<TARGET_FILE_DIR:hdVk>/usdview/plugInfo.json INPUT usdview/plugInfo.json)
foreach(py __init__.py attributes.py settings.py tooltips.py)
    file(GENERATE OUTPUT $<TARGET_FILE_DIR:hdVk>/usdview/hdvkUsdview/${py} INPUT usdview/hdvkUsdview/${py})
endforeach()
# The settings' tooltips are its "What it does" column (tooltips.py).
file(GENERATE OUTPUT $<TARGET_FILE_DIR:hdVk>/usdview/hdvkUsdview/settings.md INPUT docs/settings.md)
# Their groups are hydra/renderSettings.def's (tooltips.py ReadGroups).
file(GENERATE OUTPUT $<TARGET_FILE_DIR:hdVk>/usdview/hdvkUsdview/renderSettings.def INPUT hydra/renderSettings.def)

add_executable(testPlugin tests/testPlugin.cpp)
target_link_libraries(testPlugin PRIVATE hd)
add_dependencies(testPlugin hdVk)
if(NOT HDVK_HOUDINI)
    # Frame times with Houdini's viewport AOV bindings, and its id check (docs/houdini.md).
    add_executable(viewportBench tests/viewportBench.cpp)
    target_link_libraries(viewportBench PRIVATE cameraUtil hd hdx usdImaging usdGeom)
    add_dependencies(viewportBench hdVk)
endif()

enable_testing()
add_test(NAME vkrtest COMMAND vkrtest)
add_test(NAME plugin COMMAND testPlugin)
if(WIN32)
    # Picking through UsdImagingGL like usdview (needs the usdenv.cmd environment).
    file(TO_NATIVE_PATH ${CMAKE_SOURCE_DIR}/usdenv.cmd usdenv)
    add_test(NAME selection COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_selection.py)
    set_tests_properties(selection PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    # Houdini's viewport bindings: lazy readback, unread extra AOVs, ids vs the render index.
    if(TARGET viewportBench)  # not in the Houdini build
        add_test(NAME viewport_bindings COMMAND ${usdenv} $<TARGET_FILE:viewportBench>
                 ${CMAKE_SOURCE_DIR}/tests/scenes/step4.usda /cam --size 320x180 --check --ids)
        set_tests_properties(viewport_bindings PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
        # Houdini's camera tools: depth under the cursor, a still camera's film.
        add_test(NAME viewport_nav COMMAND ${usdenv} $<TARGET_FILE:viewportBench>
                 ${CMAKE_SOURCE_DIR}/tests/scenes/step4.usda /cam --size 320x180 --nav)
        set_tests_properties(viewport_nav PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
        # eldr:overlapReadback: moving frames one late but unchanged, still ones at once.
        add_test(NAME viewport_overlap COMMAND ${usdenv} $<TARGET_FILE:viewportBench>
                 ${CMAKE_SOURCE_DIR}/tests/scenes/step4.usda /cam --size 320x180 --aovs color,depth,primId --overlap)
        set_tests_properties(viewport_overlap PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    endif()
    add_test(NAME purpose COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_purpose.py)
    set_tests_properties(purpose PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME curves COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_curves.py)
    set_tests_properties(curves PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME stats COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_stats.py)
    set_tests_properties(stats PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME materialx COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_materialx.py)
    set_tests_properties(materialx PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    # MaterialX nodes one inline graph each, unlit: pixel = the graph's value.
    add_test(NAME mtlx_ops COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_mtlx_ops.py)
    set_tests_properties(mtlx_ops PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    # Small MaterialX graphs against the values the stdlib gives (found wrong by the corpus audit).
    add_test(NAME mtlx_graphs COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_mtlx_graphs.py)
    set_tests_properties(mtlx_graphs PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    # The MaterialX corpus audit's wrapping and node rule, no rendering (the audit: tests/mtlx_corpus.py --out).
    add_test(NAME mtlx_corpus COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/mtlx_corpus.py --selftest)
    add_test(NAME attributes COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_attributes.py)
    set_tests_properties(attributes PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME settings COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_settings_window.py)
    set_tests_properties(settings PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME viewport COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_viewport.py)
    set_tests_properties(viewport PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>" TIMEOUT 600)
    # Houdini's free camera after a scene camera with an open shutter: no camera blur carried over.
    add_test(NAME motion_camera_switch COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_camera_switch.py)
    set_tests_properties(motion_camera_switch PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME docs COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_docs.py)
    set_tests_properties(docs PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME light_shapes COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_light_shapes.py)
    set_tests_properties(light_shapes PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME lobes COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_lobes.py)
    set_tests_properties(lobes PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME furnace COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_furnace.py)
    set_tests_properties(furnace PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME textures COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_textures.py)
    set_tests_properties(textures PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME procedural COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_procedural.py)
    set_tests_properties(procedural PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME distortion COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_distortion.py)
    set_tests_properties(distortion PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    if(HDVK_EXR)
        add_test(NAME exr COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_exr.py $<TARGET_FILE:exrinfo>)
        set_tests_properties(exr PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
        add_test(NAME aov_sums COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_aov_sums.py $<TARGET_FILE:exrinfo>)
        set_tests_properties(aov_sums PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
        # ColorChecker under a white dome: linear radiance = albedo (tests/macbeth_compare.py is the Karma comparison).
        add_test(NAME macbeth COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/macbeth_compare.py
                 --exrinfo $<TARGET_FILE:exrinfo> --variants white --no-karma --check)
        set_tests_properties(macbeth PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
        add_test(NAME motion COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_motion.py $<TARGET_FILE:exrinfo>)
        set_tests_properties(motion PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    endif()
    add_test(NAME aovs COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_aovs.py)
    set_tests_properties(aovs PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME interop COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_interop.py)
    set_tests_properties(interop PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME transmission COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_transmission.py)
    set_tests_properties(transmission PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME rt_refraction COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_rt_refraction.py)
    set_tests_properties(rt_refraction PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME holdout COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_holdout.py)
    set_tests_properties(holdout PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME lighting COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_lighting.py)
    set_tests_properties(lighting PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    if(HDVK_VOLUMES)
        add_test(NAME volume COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_volume.py)
        set_tests_properties(volume PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
        if(TARGET make_volume)  # A VDB sequence played through time codes (usdview's and Houdini's Play).
            add_test(NAME volume_sequence COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_volume_sequence.py
                     $<TARGET_FILE:make_volume>)
            set_tests_properties(volume_sequence PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
            # Through UsdImaging's legacy delegate, as Houdini's viewport.
            add_test(NAME volume_sequence_legacy COMMAND ${usdenv} python
                     ${CMAKE_SOURCE_DIR}/tests/test_volume_sequence.py $<TARGET_FILE:make_volume>)
            set_tests_properties(volume_sequence_legacy PROPERTIES ENVIRONMENT
                "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>;USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX=0;USDIMAGINGGL_ENGINE_ENABLE_SCENE_INDEX_DEPRECATION_WARNING=0")
        endif()
        add_test(NAME traced_media COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_traced_media.py)
        set_tests_properties(traced_media PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    endif()
    add_test(NAME subdivision COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_subdivision.py)
    set_tests_properties(subdivision PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME displacement COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_displacement.py)
    set_tests_properties(displacement PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME displacement_graphs COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_displacement_graphs.py)
    set_tests_properties(displacement_graphs PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME lod COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_lod.py)
    set_tests_properties(lod PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME screenspace COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_screenspace.py)
    set_tests_properties(screenspace PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME crowd_instancing COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_crowd_instancing.py)
    set_tests_properties(crowd_instancing PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
endif()
set_tests_properties(plugin PROPERTIES ENVIRONMENT_MODIFICATION
    "PXR_PLUGINPATH_NAME=set:$<TARGET_FILE_DIR:hdVk>/;PATH=path_list_prepend:${pxr_DIR}/bin;PATH=path_list_prepend:${pxr_DIR}/lib")

# The Python tests run usdenv.cmd against the USD this build uses.
file(TO_NATIVE_PATH ${pxr_DIR} usdRoot)
get_property(allTests DIRECTORY PROPERTY TESTS)
set_property(TEST ${allTests} APPEND PROPERTY ENVIRONMENT "USD_ROOT=${usdRoot}")
# Tests wait for the delegate's Vulkan teardown, so the validation layer's leak check runs.
set_property(TEST ${allTests} APPEND PROPERTY ENVIRONMENT "HDVK_SYNC_TEARDOWN=1")
# Under VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation any validation message
# fails a test (the Python ones would otherwise pass over it).
set_property(TEST ${allTests} PROPERTY FAIL_REGULAR_EXPRESSION "Validation Error")
# A hung test fails after 30 minutes (viewport's own 600 s stays) instead of blocking ctest.
foreach(t ${allTests})
    get_property(hasTimeout TEST ${t} PROPERTY TIMEOUT SET)
    if(NOT hasTimeout)
        set_property(TEST ${t} PROPERTY TIMEOUT 1800)
    endif()
endforeach()
# Labels: slow = over about a minute on a GTX 1080 Ti (CTestCostData, 2026-10-29),
# fast = the rest. Lanes run `ctest -j1 -L fast`; the integrator runs everything.
set_property(TEST ${allTests} PROPERTY LABELS fast)
foreach(t vkrtest aov_sums motion lighting traced_media screenspace displacement_graphs exr)
    if(TEST ${t})
        set_property(TEST ${t} PROPERTY LABELS slow)
    endif()
endforeach()
