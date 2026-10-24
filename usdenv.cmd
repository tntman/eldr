@echo off
rem Runs a USD tool with this build's plugin, e.g.
rem   usdenv usdview --renderer Eldr tests\scenes\step2.usda
rem   usdenv usdrecord --renderer Eldr --camera /cam tests\scenes\step2.usda out.png
rem Set USD_ROOT / USD_VENV / HDVK_BUILD to override the default locations.
rem The default USD is built with OpenVDB (volumes); HDVK_BUILD must be
rem configured against the same USD (-DCMAKE_PREFIX_PATH=%USD_ROOT%).
if not defined USD_ROOT set USD_ROOT=C:\dev\usd-26.08-vdb
if not defined USD_VENV set USD_VENV=C:\dev\usd-venv
if not defined HDVK_BUILD set HDVK_BUILD=%~dp0build
set PATH=%USD_ROOT%\bin;%USD_ROOT%\lib;%USD_VENV%\Scripts;%PATH%
set PYTHONPATH=%USD_ROOT%\Lib\site-packages;%HDVK_BUILD%\usdview;%PYTHONPATH%
rem Plug only treats a path as a directory when it ends in a forward slash.
rem The second is usdview's Eldr menu (render statistics).
set PXR_PLUGINPATH_NAME=%HDVK_BUILD%/;%HDVK_BUILD%/usdview/
rem Lets test scenes use @StinsonBeach.hdr@, the HDR that ships with USD.
set PXR_AR_DEFAULT_SEARCH_PATH=%USD_ROOT%\lib\usd\hdx\resources\textures
%*
