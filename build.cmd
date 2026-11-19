@echo off
rem Builds Release into .\b via a short subst drive (the libmdr object paths can exceed MAX_PATH).
rem Requires: VS 2022 Build Tools (C++ workload, CMake, Ninja) and git. Usage: build.cmd
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%I\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1 || (echo vcvars64.bat not found & exit /b 1)
if not exist "%ROOT%\third_party\libmdr\CMakeLists.txt" git -C "%ROOT%" -c core.longpaths=true submodule update --init || exit /b 1
rem Apply our libmdr fixes (each patch touches separate hunks); one that is already applied (it reverses cleanly) is skipped.
for %%P in ("%ROOT%\patches\libmdr-*.patch") do git -C "%ROOT%\third_party\libmdr" apply --reverse --check "%%~fP" >nul 2>&1 || git -C "%ROOT%\third_party\libmdr" apply "%%~fP" || (echo Failed to apply %%~nxP & exit /b 1)
if exist X:\ (echo Drive X: is in use; build.cmd needs it free for a temporary subst. & exit /b 1)
subst X: "%ROOT%" || exit /b 1
cd /d X:\
cmake -S . -B X:\b -G Ninja -DCMAKE_BUILD_TYPE=Release >X:\b-configure.log 2>&1 || (type X:\b-configure.log & cd /d C:\ & subst X: /D & exit /b 1)
cmake --build X:\b
set RC=%ERRORLEVEL%
cd /d C:\
subst X: /D
exit /b %RC%
