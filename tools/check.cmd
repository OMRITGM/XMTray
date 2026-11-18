@echo off
rem Usage: tools\check.cmd src\File.cpp  -- type-checks ONE translation unit (no link, no output files).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cl /nologo /std:c++20 /W4 /utf-8 /permissive- /EHsc /MT /Zs /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
  /I"%~dp0..\third_party\libmdr\libmdr\include" /I"%~dp0..\third_party\libmdr\libmdr-bt\include" "%~dp0..\%~1"
