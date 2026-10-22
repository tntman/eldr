@echo off
rem usdview with this build's plugin. Set USD_ROOT / USD_VENV to override.
if not defined USD_ROOT set USD_ROOT=C:\dev\usd-26.08
if not defined USD_VENV set USD_VENV=C:\dev\usd-venv
set PATH=%USD_ROOT%\bin;%USD_ROOT%\lib;%USD_VENV%\Scripts;%PATH%
set PYTHONPATH=%USD_ROOT%\Lib\site-packages;%PYTHONPATH%
rem Plug only treats a path as a directory when it ends in a forward slash.
set PXR_PLUGINPATH_NAME=%~dp0build/
usdview %*
