@echo off
setlocal
cd /d "%~dp0"
if not exist build-mobile mkdir build-mobile
clang-cl /nologo /O2 /MD /std:c++17 /LD /Ivendor\cwmods TerrainVariance\TerrainVariance.cpp /Fobuild-mobile\TerrainVariance.obj /link d3d11.lib dxgi.lib d3dcompiler.lib user32.lib winmm.lib /OUT:build-mobile\TerrainVariance.dll
if errorlevel 1 exit /b 1
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG /Ivendor\cwmods TerrainVariance\TerrainRulesTest.cpp /Fobuild-mobile\TerrainRulesTest.obj /Febuild-mobile\TerrainRulesTest.exe
if errorlevel 1 exit /b 1
build-mobile\TerrainRulesTest.exe
if errorlevel 1 exit /b 1
clang-cl /nologo /O2 /MD /std:c++17 /UNDEBUG /Ivendor\cwmods TerrainVariance\PlacementTest.cpp /Fobuild-mobile\PlacementTest.obj /Febuild-mobile\PlacementTest.exe
if errorlevel 1 exit /b 1
build-mobile\PlacementTest.exe

