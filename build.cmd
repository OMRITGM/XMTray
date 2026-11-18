@echo off
rem Builds Release into .\b via a short subst drive (the libmdr object paths can exceed MAX_PATH).
rem Requires: VS 2022 Build Tools (C++ workload, CMake, Ninja) and git. Usage: build.cmd
setlocal
set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>&1 || (echo vcvars64.bat not found & exit /b 1)
if not exist "%ROOT%\third_party\libmdr\CMakeLists.txt" git -C "%ROOT%" -c core.longpaths=true submodule update --init || exit /b 1
rem Apply our libmdr-bt fix once (skipped when already applied).
git -C "%ROOT%\third_party\libmdr" apply --check "%ROOT%\patches\libmdr-bt-handle-leak.patch" >nul 2>&1 && git -C "%ROOT%\third_party\libmdr" apply "%ROOT%\patches\libmdr-bt-handle-leak.patch"
subst X: /D >nul 2>&1
subst X: "%ROOT%" || exit /b 1
cd /d X:\
cmake -S . -B X:\b -G Ninja -DCMAKE_BUILD_TYPE=Release >X:\b-configure.log 2>&1 || (type X:\b-configure.log & cd /d C:\ & subst X: /D & exit /b 1)
cmake --build X:\b
set RC=%ERRORLEVEL%
cd /d C:\
subst X: /D
exit /b %RC%
