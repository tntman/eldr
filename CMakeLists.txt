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
add_library(vkrenderer STATIC renderer/device.cpp)
target_include_directories(vkrenderer PUBLIC renderer)
target_link_libraries(vkrenderer PUBLIC Vulkan::Vulkan)

add_executable(vkinfo renderer/vkinfo.cpp)
target_link_libraries(vkinfo PRIVATE vkrenderer)

# Hydra layer.
add_library(hdVk SHARED hydra/rendererPlugin.cpp hydra/renderDelegate.cpp)
target_link_libraries(hdVk PRIVATE hd vkrenderer)
target_compile_definitions(hdVk PRIVATE MFB_PACKAGE_NAME=hdVk MFB_ALT_PACKAGE_NAME=hdVk)
# plugInfo.json goes next to the dll; PXR_PLUGINPATH_NAME=<that dir>/ finds it.
file(GENERATE OUTPUT $<TARGET_FILE_DIR:hdVk>/plugInfo.json INPUT plugInfo.json)

add_executable(testPlugin tests/testPlugin.cpp)
target_link_libraries(testPlugin PRIVATE hd)
add_dependencies(testPlugin hdVk)

enable_testing()
add_test(NAME vkinfo COMMAND vkinfo)
add_test(NAME plugin COMMAND testPlugin)
set_tests_properties(plugin PROPERTIES ENVIRONMENT_MODIFICATION
    "PXR_PLUGINPATH_NAME=set:$<TARGET_FILE_DIR:hdVk>/;PATH=path_list_prepend:${pxr_DIR}/bin;PATH=path_list_prepend:${pxr_DIR}/lib")
