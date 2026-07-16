@echo off
REM Build the active example plugin (example.dll). Deploy to
REM <server>\Pal\Binaries\Win64\PalApi\Plugins\Example\ with PluginInfo.json.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (echo VCVARS FAIL & exit /b 1)
cd /d "%~dp0"
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W4 /I ..\..\include example.cpp /Fe:example.dll /link /MACHINE:X64
if errorlevel 1 (echo BUILD FAIL & exit /b 1)
echo BUILD OK
dir example.dll
endlocal
