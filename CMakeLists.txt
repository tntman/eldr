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
foreach(shader mesh.vert mesh.frag sky.vert sky.frag)
    set(spv ${CMAKE_CURRENT_BINARY_DIR}/${shader}.inc)
    add_custom_command(OUTPUT ${spv}
        COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=c -o ${spv}
                ${CMAKE_CURRENT_SOURCE_DIR}/renderer/shaders/${shader}
        DEPENDS renderer/shaders/${shader} renderer/shaders/frame.glsl)
    list(APPEND shader_incs ${spv})
endforeach()

add_library(vkrenderer STATIC
    renderer/device.cpp renderer/scene.cpp renderer/renderer.cpp renderer/ibl.cpp ${shader_incs})
target_include_directories(vkrenderer PUBLIC renderer PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(vkrenderer PUBLIC Vulkan::Vulkan)

add_executable(vkrtest renderer/vkrtest.cpp)
target_link_libraries(vkrtest PRIVATE vkrenderer)

# Hydra layer.
add_library(hdVk SHARED
    hydra/rendererPlugin.cpp hydra/renderDelegate.cpp hydra/renderPass.cpp
    hydra/mesh.cpp hydra/material.cpp hydra/light.cpp hydra/instancer.cpp
    hydra/sceneIndexPlugin.cpp)
target_link_libraries(hdVk PRIVATE hd hdsi hio vkrenderer)
target_compile_definitions(hdVk PRIVATE MFB_PACKAGE_NAME=hdVk MFB_ALT_PACKAGE_NAME=hdVk)
# plugInfo.json goes next to the dll; PXR_PLUGINPATH_NAME=<that dir>/ finds it.
file(GENERATE OUTPUT $<TARGET_FILE_DIR:hdVk>/plugInfo.json INPUT plugInfo.json)

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
endif()
set_tests_properties(plugin PROPERTIES ENVIRONMENT_MODIFICATION
    "PXR_PLUGINPATH_NAME=set:$<TARGET_FILE_DIR:hdVk>/;PATH=path_list_prepend:${pxr_DIR}/bin;PATH=path_list_prepend:${pxr_DIR}/lib")
