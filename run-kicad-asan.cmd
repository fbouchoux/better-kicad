@echo off
setlocal

set "KICAD_SOURCE_DIR=%~dp0"
set "KICAD_ASAN_BUILD=%KICAD_SOURCE_DIR%build\msvc-win64-asan"
set "KICAD_ASAN_EXE=%KICAD_ASAN_BUILD%\kicad\kicad.exe"

if not exist "%KICAD_ASAN_EXE%" (
    echo ASan KiCad executable not found:
    echo   %KICAD_ASAN_EXE%
    echo Select the Win64 ASan preset in Visual Studio and build kicad first.
    exit /b 1
)

set "VS_INSTALL_DIR="
for %%E in (Community Professional Enterprise BuildTools) do if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" set "VS_INSTALL_DIR=%ProgramFiles%\Microsoft Visual Studio\2022\%%E"

if not defined VS_INSTALL_DIR (
    echo A Visual Studio installation with the C++ toolchain was not found.
    exit /b 1
)

call "%VS_INSTALL_DIR%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b %errorlevel%

if not exist "%KICAD_ASAN_BUILD%\common\gal\kigal.dll" (
    echo Missing KiCad GAL runtime:
    echo   %KICAD_ASAN_BUILD%\common\gal\kigal.dll
    exit /b 1
)

if not exist "%KICAD_ASAN_BUILD%\eeschema\_eeschema.dll" (
    echo Missing Eeschema module:
    echo   %KICAD_ASAN_BUILD%\eeschema\_eeschema.dll
    exit /b 1
)

where.exe clang_rt.asan_dynamic-x86_64.dll >nul 2>nul
if errorlevel 1 (
    echo The MSVC AddressSanitizer runtime is not available on PATH.
    exit /b 1
)

set "KICAD_RUN_FROM_BUILD_DIR=1"
set "KICAD_USE_EXTERNAL_PYTHONHOME=1"
set "PYTHONHOME=%KICAD_ASAN_BUILD%\vcpkg_installed\x64-windows\tools\python3"
set "PYTHONPATH=%KICAD_ASAN_BUILD%\pcbnew;%KICAD_SOURCE_DIR%scripting"
set "ASAN_OPTIONS=detect_stack_use_after_return=0"
set "ASAN_SAVE_DUMPS=%KICAD_ASAN_BUILD%\asan-kicad.dmp"

set "PATH=%KICAD_ASAN_BUILD%\vcpkg_installed\x64-windows\bin;%KICAD_ASAN_BUILD%\common;%KICAD_ASAN_BUILD%\api;%KICAD_ASAN_BUILD%\common\gal;%KICAD_ASAN_BUILD%\pcbnew;%KICAD_ASAN_BUILD%\eeschema;%KICAD_ASAN_BUILD%\gerbview;%KICAD_ASAN_BUILD%\3d-viewer\3d_cache\sg;%KICAD_ASAN_BUILD%\plugins\3d\idf;%KICAD_ASAN_BUILD%\plugins\3d\oce;%KICAD_ASAN_BUILD%\plugins\3d\vrml;%PATH%"

if /i "%~1"=="--check" (
    echo ASan KiCad runtime check passed.
    echo Executable: %KICAD_ASAN_EXE%
    echo kigal.dll: %KICAD_ASAN_BUILD%\common\gal\kigal.dll
    echo _eeschema.dll: %KICAD_ASAN_BUILD%\eeschema\_eeschema.dll
    where.exe clang_rt.asan_dynamic-x86_64.dll
    exit /b 0
)

echo Starting ASan KiCad from:
echo   %KICAD_ASAN_EXE%
echo ASan dumps will be written to:
echo   %ASAN_SAVE_DUMPS%
echo.

pushd "%KICAD_ASAN_BUILD%\kicad"
"%KICAD_ASAN_EXE%" %*
set "KICAD_EXIT_CODE=%ERRORLEVEL%"
popd

echo.
echo KiCad exited with code %KICAD_EXIT_CODE%.
exit /b %KICAD_EXIT_CODE%
