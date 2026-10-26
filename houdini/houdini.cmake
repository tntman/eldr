# Optional build against Houdini's own USD (cmake -DHDVK_HOUDINI=ON
# -DHOUDINI_ROOT=<Houdini install>). The normal build never reads this file.
# Houdini ships no pxrConfig.cmake: its HoudiniConfig.cmake provides
# Houdini::Dep::pxr_* imported targets, which are re-exposed here under the
# plain names (hd, usd, ...) the rest of CMakeLists.txt links.
set(HOUDINI_ROOT "" CACHE PATH "Houdini install root (HFS)")
if(NOT HOUDINI_ROOT)
    message(FATAL_ERROR "HDVK_HOUDINI needs -DHOUDINI_ROOT=<Houdini install>")
endif()
list(APPEND CMAKE_PREFIX_PATH ${HOUDINI_ROOT}/toolkit/cmake)
find_package(Houdini REQUIRED)

# HoudiniConfig has no Dep target for garch, hgi and hio: link those import
# libraries by path.
set(hdvk_pxr_libs ar cameraUtil garch hd hdsi hgi hio pxOsd sdr usd usdGeom usdImaging)
foreach(lib ${hdvk_pxr_libs})
    add_library(${lib} INTERFACE)
    if(TARGET Houdini::Dep::pxr_${lib})
        target_link_libraries(${lib} INTERFACE Houdini::Dep::pxr_${lib})
    else()
        target_link_libraries(${lib} INTERFACE ${HOUDINI_ROOT}/custom/houdini/dsolib/libpxr_${lib}.lib)
    endif()
endforeach()
# Headers (pxr/..., hboost, python, tbb), Houdini's compile definitions and, via
# HoudiniThirdParty, the rest of its USD libraries (tf, sdf, python, ...).
foreach(lib ${hdvk_pxr_libs})
    target_link_libraries(${lib} INTERFACE Houdini HoudiniThirdParty)
endforeach()
set(pxr_DIR ${HOUDINI_ROOT})  # used by CMakeLists.txt for the test environment

# A Houdini package next to the plugin: copy or reference <build>/houdini/eldr.json
# from $HOUDINI_USER_PREF_DIR/packages (docs/houdini.md). Its path is the
# build dir's houdini/ (UsdRenderers.json, so Solaris lists "Eldr") and
# PXR_PLUGINPATH_NAME finds plugInfo.json beside the dll.
file(GENERATE OUTPUT $<TARGET_FILE_DIR:hdVk>/houdini/eldr.json CONTENT
"{
    \"path\": \"$<TARGET_FILE_DIR:hdVk>/houdini\",
    \"env\": [
        {\"PXR_PLUGINPATH_NAME\": {\"method\": \"append\", \"value\": \"$<TARGET_FILE_DIR:hdVk>\"}},
        {\"PXR_AR_DEFAULT_SEARCH_PATH\": {\"method\": \"append\", \"value\": \"$HFS/bin/usd_plugins/hdx/resources/textures\"}}
    ]
}
")

# Eldr's Solaris UI (Render Settings / Render Geometry Settings tabs, viewport
# options): dialog scripts generated from the renderer's own setting
# descriptors by hython, into <build>/houdini/soho/parameters. hython runs with
# its own empty preferences and packages, so nothing installed loads beside it.
set(hdvk_ui_dir ${CMAKE_BINARY_DIR}/houdini_ui)
file(MAKE_DIRECTORY ${hdvk_ui_dir}/pref/houdini__HVER__ ${hdvk_ui_dir}/packages)
add_custom_target(eldrHoudiniUi ALL
    COMMAND ${CMAKE_COMMAND} -E env
        HOUDINI_USER_PREF_DIR=${hdvk_ui_dir}/pref/houdini__HVER__
        HOUDINI_PACKAGE_DIR=${hdvk_ui_dir}/packages
        PXR_PLUGINPATH_NAME=$<TARGET_FILE_DIR:hdVk>
        ${HOUDINI_ROOT}/bin/hython.exe ${CMAKE_CURRENT_LIST_DIR}/gen_ui.py
        $<TARGET_FILE_DIR:hdVk>/houdini ${CMAKE_CURRENT_LIST_DIR}/../usdview/hdvkUsdview/settings.py
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/gen_ui.py ${CMAKE_CURRENT_LIST_DIR}/../usdview/hdvkUsdview/settings.py
            ${CMAKE_CURRENT_LIST_DIR}/../usdview/hdvkUsdview/tooltips.py ${CMAKE_CURRENT_LIST_DIR}/../docs/settings.md
            ${CMAKE_CURRENT_LIST_DIR}/../usdview/hdvkUsdview/__init__.py ${CMAKE_CURRENT_LIST_DIR}/UsdRenderers.json
    COMMENT "Generating Eldr's Solaris UI"
    VERBATIM)
add_dependencies(eldrHoudiniUi hdVk)
