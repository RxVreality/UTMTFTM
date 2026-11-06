@echo off
rem Configures, builds and runs the offline tests. Finished program: dist\
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -products * -latest -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build || exit /b 1
if "%1"=="notest" exit /b 0
dist\test_ghost.exe || exit /b 1
dist\test_macro.exe || exit /b 1
dist\test_session.exe || exit /b 1
dist\test_driver.exe || exit /b 1
dist\test_controllers.exe || exit /b 1
if not exist build\ui mkdir build\ui
dist\test_ui.exe build\ui || exit /b 1
dist\test_bindings.exe build\ui || exit /b 1
