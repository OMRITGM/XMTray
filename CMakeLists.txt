cmake_minimum_required(VERSION 3.31)
project(XMTray VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(MDR_BUILD_CLIENT   OFF CACHE BOOL "" FORCE)
set(MDR_BUILD_TESTING  OFF CACHE BOOL "" FORCE)
set(MDR_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(MDR_BUILD_CODEGEN  OFF CACHE BOOL "" FORCE)
set(MDR_BT_BLE_SUPPORT OFF CACHE BOOL "" FORCE)
set(MDR_BUILD_WITH_IPO OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libmdr libmdr EXCLUDE_FROM_ALL)

file(GLOB XM_SOURCES CONFIGURE_DEPENDS src/*.cpp src/*.rc src/*.manifest)
add_executable(XMTray WIN32 ${XM_SOURCES})
target_compile_definitions(XMTray PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(XMTray PRIVATE /W4 /utf-8 /permissive-)
target_link_libraries(XMTray PRIVATE mdr mdr-bt
    d2d1 dwrite dwmapi shcore windowsapp shell32 ole32 uuid user32 gdi32 advapi32 setupapi cfgmgr32)
# Portable exe: resolve imported DLLs from System32 only (no planted DLL next to the exe).
target_link_options(XMTray PRIVATE /DEPENDENTLOADFLAG:0x800
    # Bind the non-KnownDLLs lazily, after wWinMain has restricted the DLL search path to System32.
    /DELAYLOAD:d2d1.dll /DELAYLOAD:dwrite.dll /DELAYLOAD:dwmapi.dll /DELAYLOAD:cfgmgr32.dll /DELAYLOAD:bthprops.cpl)
target_link_libraries(XMTray PRIVATE delayimp)
