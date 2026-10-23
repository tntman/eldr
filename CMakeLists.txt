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
# -DCMAKE_PREFIX_PATH=<USD install root>; pxr_DIR alone misses its TBB/OpenSubdiv configs.
find_package(pxr CONFIG REQUIRED)

# Renderer layer: plain Vulkan, must never include USD headers.
# Shaders compile to SPIR-V word lists that renderer.cpp #includes.
foreach(shader mesh.vert mesh.frag mask.frag fullscreen.vert sky.frag cull.comp lights.comp skin.comp gtao.frag
               gtao_denoise.frag
               taa.frag lens_tiles.comp dof.frag motion_blur.frag histogram.comp exposure.comp bloom_down.frag bloom_up.frag
               tonemap.frag)
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/${shader}.inc)
    add_custom_command(OUTPUT ${spv}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -o ${spv}
                ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/${shader}
        DEPENDS renderer/shaders/${shader} renderer/shaders/frame.glsl renderer/shaders/post.glsl
                renderer/shaders/scene.glsl renderer/shaders/lights.glsl renderer/shaders/material.glsl
                renderer/shaders/lens.glsl renderer/shaders/program.glsl renderer/shaders/noise.glsl
                renderer/shaders/brdf.glsl)
    list(APPEND shader_incs ${spv})
endforeach()
# Material programs' shading pass (GRAPH): mesh.vert exporting vertex-rate
# primvars, mesh.frag with the interpreter; other materials never run it.
foreach(shader mesh.vert mesh.frag)
    string(REPLACE "mesh." "mesh_graph." graph ${shader})
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/${graph}.inc)
    add_custom_command(OUTPUT ${spv}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -DGRAPH -o ${spv}
                ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/${shader}
        DEPENDS renderer/shaders/${shader} renderer/shaders/frame.glsl renderer/shaders/scene.glsl
                renderer/shaders/lights.glsl renderer/shaders/material.glsl renderer/shaders/program.glsl
                renderer/shaders/noise.glsl renderer/shaders/brdf.glsl)
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
                renderer/shaders/noise.glsl renderer/shaders/brdf.glsl)
    list(APPEND shader_incs ${spv})
endforeach()
# The depth passes' vertex stage: mesh.vert exporting only what mask.frag reads.
set(spv ${CMAKE_CURRENT_BINARY_DIR}/mesh_depth.vert.inc)
add_custom_command(OUTPUT ${spv}
    COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -DDEPTH_ONLY -o ${spv}
            ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/mesh.vert
    DEPENDS renderer/shaders/mesh.vert renderer/shaders/scene.glsl)
list(APPEND shader_incs ${spv})

add_library(vkrenderer STATIC
    renderer/device.cpp renderer/scene.cpp renderer/renderer.cpp renderer/ibl.cpp
    renderer/gtao.cpp renderer/post.cpp renderer/taa.cpp renderer/lens.cpp renderer/fullscreen.cpp
    ${shader_incs})
target_include_directories(vkrenderer PUBLIC renderer PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(vkrenderer PUBLIC Vulkan::Vulkan)

add_executable(vkrtest renderer/vkrtest.cpp)
target_link_libraries(vkrtest PRIVATE vkrenderer)

# Hydra layer.
add_library(hdVk SHARED
    hydra/rendererPlugin.cpp hydra/renderDelegate.cpp hydra/renderPass.cpp
    hydra/mesh.cpp hydra/curves.cpp hydra/material.cpp hydra/light.cpp hydra/instancer.cpp
    hydra/sceneIndexPlugin.cpp)
target_link_libraries(hdVk PRIVATE ar hd hdsi hio sdr vkrenderer)
target_compile_definitions(hdVk PRIVATE MFB_PACKAGE_NAME=hdVk MFB_ALT_PACKAGE_NAME=hdVk)
# plugInfo.json goes next to the dll; PXR_PLUGINPATH_NAME=<that dir>/ finds it.
file(GENERATE OUTPUT $<TARGET_FILE_DIR:hdVk>/plugInfo.json INPUT plugInfo.json)
# usdview's HdVk menu (render statistics, attribute editor), in
# <that dir>/usdview: usdenv.cmd adds it to the plugin and Python paths.
file(GENERATE OUTPUT $<TARGET_FILE_DIR:hdVk>/usdview/plugInfo.json INPUT usdview/plugInfo.json)
foreach(py __init__.py attributes.py)
    file(GENERATE OUTPUT $<TARGET_FILE_DIR:hdVk>/usdview/hdvkUsdview/${py} INPUT usdview/hdvkUsdview/${py})
endforeach()

add_executable(testPlugin tests/testPlugin.cpp)
target_link_libraries(testPlugin PRIVATE hd)
add_dependencies(testPlugin hdVk)

enable_testing()
add_test(NAME vkrtest COMMAND vkrtest)
add_test(NAME plugin COMMAND testPlugin)
if(WIN32)
    # Picking through UsdImagingGL like usdview (needs the usdenv.cmd environment).
    file(TO_NATIVE_PATH ${CMAKE_SOURCE_DIR}/usdenv.cmd usdenv)
    add_test(NAME selection COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_selection.py)
    set_tests_properties(selection PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME purpose COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_purpose.py)
    set_tests_properties(purpose PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME curves COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_curves.py)
    set_tests_properties(curves PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME stats COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_stats.py)
    set_tests_properties(stats PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME materialx COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_materialx.py)
    set_tests_properties(materialx PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME attributes COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_attributes.py)
    set_tests_properties(attributes PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME lobes COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_lobes.py)
    set_tests_properties(lobes PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME textures COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_textures.py)
    set_tests_properties(textures PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
    add_test(NAME procedural COMMAND ${usdenv} python ${CMAKE_SOURCE_DIR}/tests/test_procedural.py)
    set_tests_properties(procedural PROPERTIES ENVIRONMENT "HDVK_BUILD=$<TARGET_FILE_DIR:hdVk>")
endif()
set_tests_properties(plugin PROPERTIES ENVIRONMENT_MODIFICATION
    "PXR_PLUGINPATH_NAME=set:$<TARGET_FILE_DIR:hdVk>/;PATH=path_list_prepend:${pxr_DIR}/bin;PATH=path_list_prepend:${pxr_DIR}/lib")
