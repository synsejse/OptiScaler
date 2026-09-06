@echo off
setlocal
if not exist "%~2" mkdir "%~2"
pushd "%~2" || exit /b 1
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I"%~1\external\nlohmann" /I"%~1\external\FidelityFX-SDK\ffx-api\include\ffx_api" /I"%~1\OptiScaler\include\fsr-rr" "%~1\tools\fsrrr-replay\replay.cpp" /Fe:fsrrr-replay.exe /link d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
popd
exit /b 0
