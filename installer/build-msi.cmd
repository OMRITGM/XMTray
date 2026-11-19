@echo off
setlocal
rem Usage: build-msi.cmd [version]   (default: project VERSION in CMakeLists.txt)
set "R=%~dp0.."
for %%I in ("%R%") do set "R=%%~fI"
set "VER=%~1"
if "%VER%"=="" for /f "tokens=3" %%V in ('findstr /b /c:"project(XMTray VERSION" "%R%\CMakeLists.txt"') do set "VER=%%V"
if "%VER%"=="" (echo ERROR: version not found & exit /b 1)
if not exist "%R%\b\XMTray.exe" (echo ERROR: %R%\b\XMTray.exe missing - run build.cmd first & exit /b 1)
set "WB="
if exist "%R%\tools\wix314\candle.exe" set "WB=%R%\tools\wix314"
if not defined WB if defined WIX if exist "%WIX%bin\candle.exe" set "WB=%WIX%bin"
if not defined WB (echo ERROR: WiX 3.14 not found. Set WIX or extract binaries to %R%\tools\wix314 & exit /b 1)
if not exist "%R%\dist" mkdir "%R%\dist"
if not exist "%R%\b\wix" mkdir "%R%\b\wix"
pushd "%~dp0"
"%WB%\candle.exe" -nologo -arch x64 -dVersion=%VER% "-dExePath=%R%\b\XMTray.exe" "-dLicensePath=%R%\LICENSE" "-dReadmePath=%~dp0README.txt" "-dRtfPath=%~dp0License.rtf" "-dIconPath=%R%\src\app.ico" -ext WixUIExtension -ext WixUtilExtension -out "%R%\b\wix\XMTray.wixobj" XMTray.wxs || (popd & exit /b 1)
"%WB%\light.exe" -nologo -ext WixUIExtension -ext WixUtilExtension -out "%R%\dist\XMTray-%VER%-x64.msi" "%R%\b\wix\XMTray.wixobj" || (popd & exit /b 1)
popd
echo OK: %R%\dist\XMTray-%VER%-x64.msi
