@echo off
setlocal
rem Run in an isolated intermediate directory, never write binaries into the source tree.
if not exist "%~2" mkdir "%~2"
pushd "%~2" || exit /b 1
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc "%~1\tests\fsrd_input_math.cpp" /Fo:fsrd_input_math.obj /Fe:fsrd_input_math.exe
if errorlevel 1 exit /b 1
fsrd_input_math.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++20 /EHsc "%~1\tests\fsrd_submission_policy.cpp" /Fo:fsrd_submission_policy.obj /Fe:fsrd_submission_policy.exe
if errorlevel 1 exit /b 1
fsrd_submission_policy.exe
if errorlevel 1 exit /b 1
python -m unittest discover -s "%~1\tests" -p "test_fsrd_*.py" -v
if errorlevel 1 exit /b 1
popd
exit /b 0
