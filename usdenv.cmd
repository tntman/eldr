@echo off
rem Runs a USD tool with this build's plugin, e.g.
rem   usdenv usdview --renderer HdVk tests\scenes\step2.usda
rem   usdenv usdrecord --renderer HdVk --camera /cam tests\scenes\step2.usda out.png
rem Set USD_ROOT / USD_VENV to override the install locations.
if not defined USD_ROOT set USD_ROOT=C:\dev\usd-26.08
if not defined USD_VENV set USD_VENV=C:\dev\usd-venv
set PATH=%USD_ROOT%\bin;%USD_ROOT%\lib;%USD_VENV%\Scripts;%PATH%
set PYTHONPATH=%USD_ROOT%\Lib\site-packages;%PYTHONPATH%
rem Plug only treats a path as a directory when it ends in a forward slash.
set PXR_PLUGINPATH_NAME=%~dp0build/
rem Lets test scenes use @StinsonBeach.hdr@, the HDR that ships with USD.
set PXR_AR_DEFAULT_SEARCH_PATH=%USD_ROOT%\lib\usd\hdx\resources\textures
%*
