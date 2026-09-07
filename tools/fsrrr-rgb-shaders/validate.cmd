@echo off
setlocal
rem Resolve input/output before changing directory. No shader or game files are edited.
set "fsrd_rgb_generated=%~f1"
set "fsrd_rgb_validator_output=%~f2"
if not exist "%fsrd_rgb_generated%\FSRDFogRgbWrite_VS.h" exit /b 1
if not exist "%fsrd_rgb_generated%\FSRDFogRgbWrite_PS.h" exit /b 1
if not exist "%fsrd_rgb_validator_output%" mkdir "%fsrd_rgb_validator_output%"
if errorlevel 1 exit /b 1
set "fsrd_rgb_vs_install="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "fsrd_rgb_vs_install=%%i"
if not defined fsrd_rgb_vs_install exit /b 1
call "%fsrd_rgb_vs_install%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
pushd "%fsrd_rgb_validator_output%" || exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /WX /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I"%fsrd_rgb_generated%" "%~dp0validate.cpp" /Fe:fsrrr-rgb-shaders-validate.exe /link d3dcompiler.lib
if errorlevel 1 exit /b 1
fsrrr-rgb-shaders-validate.exe
if errorlevel 1 exit /b 1
popd
exit /b 0
