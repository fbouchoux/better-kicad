@echo off
setlocal

rem ============================================================
rem KiCad development launcher - Windows / RelWithDebInfo
rem
rem Put this file in the KiCad source root, next to CMakeLists.txt:
rem   C:\workspace\kicad\kicad-master\run-kicad-dev.cmd
rem ============================================================

set "SOURCE_ROOT=%~dp0"
set "BUILD_ROOT=%SOURCE_ROOT%build\msvc-win64-relwithdebinfo"
set "VCPKG_INSTALLED=%BUILD_ROOT%\vcpkg_installed\x64-windows"
set "KICAD_EXE=%BUILD_ROOT%\kicad\kicad.exe"

if not exist "%KICAD_EXE%" (
    echo.
    echo ERROR: kicad.exe was not found:
    echo   "%KICAD_EXE%"
    echo.
    echo Build the complete KiCad project first ^(Build All in Visual Studio^).
    echo.
    pause
    exit /b 1
)

if not exist "%VCPKG_INSTALLED%\bin" (
    echo.
    echo ERROR: vcpkg runtime directory was not found:
    echo   "%VCPKG_INSTALLED%\bin"
    echo.
    echo Reconfigure/build KiCad before launching it.
    echo.
    pause
    exit /b 1
)

if not exist "%VCPKG_INSTALLED%\tools\python3" (
    echo.
    echo ERROR: the vcpkg Python runtime was not found:
    echo   "%VCPKG_INSTALLED%\tools\python3"
    echo.
    pause
    exit /b 1
)

if not exist "%BUILD_ROOT%\resources\images.tar.gz" (
    echo.
    echo ERROR: KiCad build resources are incomplete:
    echo   "%BUILD_ROOT%\resources\images.tar.gz"
    echo.
    echo Run Build All once in Visual Studio.
    echo.
    pause
    exit /b 1
)

if not exist "%BUILD_ROOT%\schemas" (
    echo.
    echo WARNING: schema directory was not found:
    echo   "%BUILD_ROOT%\schemas"
    echo.
    echo KiCad will still be started. If it reports missing schema files,
    echo rebuild the api_schema_build_copy target or run Build All.
    echo.
)

set "KICAD_RUN_FROM_BUILD_DIR=1"
set "KICAD_USE_EXTERNAL_PYTHONHOME=1"
set "PYTHONHOME=%VCPKG_INSTALLED%\tools\python3"
set "PYTHONPATH=%BUILD_ROOT%\pcbnew;%SOURCE_ROOT%scripting"
set "PATH=%VCPKG_INSTALLED%\bin;%BUILD_ROOT%\common;%BUILD_ROOT%\api;%BUILD_ROOT%\common\gal;%PATH%"
set "KICAD10_SYMBOL_DIR=C:\Program Files\KiCad\10.0\share\kicad\symbols"
set "KICAD10_FOOTPRINT_DIR=C:\Program Files\KiCad\10.0\share\kicad\footprints"
set "KICAD10_3DMODEL_DIR=C:\Program Files\KiCad\10.0\share\kicad\3dmodels"
set "KICAD10_TEMPLATE_DIR=C:\Program Files\KiCad\10.0\share\kicad\template"

echo Starting KiCad development build...
echo   %KICAD_EXE%
echo.

start "KiCad Dev" /D "%BUILD_ROOT%" "%KICAD_EXE%" %*

endlocal
exit /b 0
