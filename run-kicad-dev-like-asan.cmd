@echo off
setlocal

rem Launch the RelWithDebInfo build with an environment matching run-kicad-asan.cmd.
rem ASan-specific options are intentionally omitted.

set "KICAD_SOURCE_DIR=%~dp0"
set "KICAD_DEV_BUILD=%KICAD_SOURCE_DIR%build\msvc-win64-relwithdebinfo"
set "KICAD_DEV_EXE=%KICAD_DEV_BUILD%\kicad\kicad.exe"

if not exist "%KICAD_DEV_EXE%" (
    echo Development KiCad executable not found:
    echo   %KICAD_DEV_EXE%
    echo Select the Win64 RelWithDebInfo preset in Visual Studio and build kicad first.
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

if not exist "%KICAD_DEV_BUILD%\common\gal\kigal.dll" (
    echo Missing KiCad GAL runtime:
    echo   %KICAD_DEV_BUILD%\common\gal\kigal.dll
    exit /b 1
)

if not exist "%KICAD_DEV_BUILD%\eeschema\_eeschema.dll" (
    echo Missing Eeschema module:
    echo   %KICAD_DEV_BUILD%\eeschema\_eeschema.dll
    exit /b 1
)

set "KICAD_RUN_FROM_BUILD_DIR=1"
set "KICAD_USE_EXTERNAL_PYTHONHOME=1"
set "PYTHONHOME=%KICAD_DEV_BUILD%\vcpkg_installed\x64-windows\tools\python3"
set "PYTHONPATH=%KICAD_DEV_BUILD%\pcbnew;%KICAD_SOURCE_DIR%scripting"

set "PATH=%KICAD_DEV_BUILD%\vcpkg_installed\x64-windows\bin;%KICAD_DEV_BUILD%\common;%KICAD_DEV_BUILD%\api;%KICAD_DEV_BUILD%\common\gal;%KICAD_DEV_BUILD%\pcbnew;%KICAD_DEV_BUILD%\eeschema;%KICAD_DEV_BUILD%\gerbview;%KICAD_DEV_BUILD%\3d-viewer\3d_cache\sg;%KICAD_DEV_BUILD%\plugins\3d\idf;%KICAD_DEV_BUILD%\plugins\3d\oce;%KICAD_DEV_BUILD%\plugins\3d\vrml;%PATH%"

if /i "%~1"=="--check" (
    echo Development KiCad runtime check passed.
    echo Executable: %KICAD_DEV_EXE%
    echo kigal.dll: %KICAD_DEV_BUILD%\common\gal\kigal.dll
    echo _eeschema.dll: %KICAD_DEV_BUILD%\eeschema\_eeschema.dll
    exit /b 0
)

echo Starting RelWithDebInfo KiCad with the ASan-equivalent environment:
echo   %KICAD_DEV_EXE%
echo.

pushd "%KICAD_DEV_BUILD%\kicad"
"%KICAD_DEV_EXE%" %*
set "KICAD_EXIT_CODE=%ERRORLEVEL%"
popd

echo.
echo KiCad exited with code %KICAD_EXIT_CODE%.
exit /b %KICAD_EXIT_CODE%
