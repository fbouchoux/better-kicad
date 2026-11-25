@echo off
setlocal

rem Keep all editable paths together so the runtime layout is easy to audit
set "KICAD_SOURCE_ROOT=%~dp0"
set "KICAD_BUILD_ROOT=%KICAD_SOURCE_ROOT%build\msvc-win64-relwithdebinfo"
set "KICAD_VCPKG_ROOT=%KICAD_BUILD_ROOT%\vcpkg_installed\x64-windows"
set "KICAD_RELEASE_ROOT=%ProgramFiles%\KiCad\10.0"
set "KICAD_RELEASE_DATA=%KICAD_RELEASE_ROOT%\share\kicad"
set "KICAD_EXE=%KICAD_BUILD_ROOT%\kicad\kicad.exe"

rem Save launcher options before subroutine calls replace the active batch arguments
if /i "%~1"=="--check" set "KICAD_CHECK_ONLY=1"

rem Refuse to start when a partial build could make Windows find a release KiCad DLL
for %%F in (
    "%KICAD_EXE%"
    "%KICAD_BUILD_ROOT%\common\kicommon.dll"
    "%KICAD_BUILD_ROOT%\api\kiapi.dll"
    "%KICAD_BUILD_ROOT%\common\gal\kigal.dll"
    "%KICAD_BUILD_ROOT%\eeschema\_eeschema.dll"
    "%KICAD_BUILD_ROOT%\pcbnew\_pcbnew.dll"
    "%KICAD_BUILD_ROOT%\gerbview\_gerbview.dll"
    "%KICAD_BUILD_ROOT%\cvpcb\_cvpcb.dll"
    "%KICAD_BUILD_ROOT%\pagelayout_editor\_pl_editor.dll"
    "%KICAD_BUILD_ROOT%\pcb_calculator\_pcb_calculator.dll"
    "%KICAD_BUILD_ROOT%\3d-viewer\3d_cache\sg\kicad_3dsg.dll"
    "%KICAD_BUILD_ROOT%\plugins\3d\idf\s3d_plugin_idf.dll"
    "%KICAD_BUILD_ROOT%\plugins\3d\oce\s3d_plugin_oce.dll"
    "%KICAD_BUILD_ROOT%\plugins\3d\vrml\s3d_plugin_vrml.dll"
    "%KICAD_BUILD_ROOT%\resources\images.tar.gz"
    "%KICAD_VCPKG_ROOT%\tools\python3\python.exe"
) do if not exist "%%~F" (
    echo.
    echo ERROR: Missing development runtime file
    echo   %%~F
    exit /b 1
)

rem Use the release installation only for stock data that is not built in this tree
for %%D in (
    "%KICAD_BUILD_ROOT%\schemas"
    "%KICAD_VCPKG_ROOT%\bin"
    "%KICAD_RELEASE_DATA%\symbols"
    "%KICAD_RELEASE_DATA%\footprints"
    "%KICAD_RELEASE_DATA%\3dmodels"
    "%KICAD_RELEASE_DATA%\template"
    "%KICAD_RELEASE_DATA%\scripting"
    "%KICAD_RELEASE_ROOT%\etc\fonts"
) do if not exist "%%~D\" (
    echo.
    echo ERROR: Missing runtime directory
    echo   %%~D
    exit /b 1
)

rem Tell KiCad to resolve code, plugins, schemas and images from the build tree
set "KICAD_RUN_FROM_BUILD_DIR=1"

rem Use the Python interpreter and extension module built by this configuration
set "KICAD_USE_EXTERNAL_PYTHONHOME=1"
set "PYTHONHOME=%KICAD_VCPKG_ROOT%\tools\python3"
set "PYTHONPATH=%KICAD_BUILD_ROOT%\pcbnew;%KICAD_SOURCE_ROOT%scripting"

rem Point stock libraries and non-code support data at the matching installed release
set "KICAD_STOCK_DATA_HOME=%KICAD_RELEASE_DATA%"
set "KICAD10_SYMBOL_DIR=%KICAD_RELEASE_DATA%\symbols"
set "KICAD10_FOOTPRINT_DIR=%KICAD_RELEASE_DATA%\footprints"
set "KICAD10_3DMODEL_DIR=%KICAD_RELEASE_DATA%\3dmodels"
set "KICAD10_TEMPLATE_DIR=%KICAD_RELEASE_DATA%\template"
set "KICAD10_SCRIPTING_DIR=%KICAD_RELEASE_DATA%\scripting"
set "FONTCONFIG_PATH=%KICAD_RELEASE_ROOT%\etc\fonts"

rem Isolate DLL lookup from the machine PATH so installed KiCad binaries cannot be loaded
set "PATH=%KICAD_BUILD_ROOT%\kicad;%KICAD_BUILD_ROOT%\common;%KICAD_BUILD_ROOT%\api;%KICAD_BUILD_ROOT%\common\gal;%KICAD_BUILD_ROOT%\pcbnew;%KICAD_BUILD_ROOT%\eeschema;%KICAD_BUILD_ROOT%\gerbview;%KICAD_BUILD_ROOT%\cvpcb;%KICAD_BUILD_ROOT%\pagelayout_editor;%KICAD_BUILD_ROOT%\bitmap2component;%KICAD_BUILD_ROOT%\pcb_calculator;%KICAD_BUILD_ROOT%\3d-viewer\3d_cache\sg;%KICAD_BUILD_ROOT%\plugins\3d\idf;%KICAD_BUILD_ROOT%\plugins\3d\oce;%KICAD_BUILD_ROOT%\plugins\3d\vrml;%KICAD_VCPKG_ROOT%\bin;%PYTHONHOME%;%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem;%SystemRoot%\System32\WindowsPowerShell\v1.0"

rem Print the effective split between development binaries and installed stock data
echo KiCad development startup paths
echo   Source tree:       %KICAD_SOURCE_ROOT%
echo   Build tree:        %KICAD_BUILD_ROOT%
echo   Executable:        %KICAD_EXE%
echo   Build DLLs:        build module directories and %KICAD_VCPKG_ROOT%\bin
echo   Build resources:   %KICAD_BUILD_ROOT%\resources
echo   Build schemas:     %KICAD_BUILD_ROOT%\schemas
echo   Build Python:      %PYTHONHOME%
echo   Release data root: %KICAD_RELEASE_DATA%
echo   Symbols:           %KICAD10_SYMBOL_DIR%
echo   Footprints:        %KICAD10_FOOTPRINT_DIR%
echo   3D models:         %KICAD10_3DMODEL_DIR%
echo   Templates:         %KICAD10_TEMPLATE_DIR%
echo   Scripting data:    %KICAD10_SCRIPTING_DIR%
echo   Fontconfig:        %FONTCONFIG_PATH%
echo   Machine PATH:      intentionally excluded from DLL lookup

if defined KICAD_CHECK_ONLY (
    echo.
    echo Startup environment check passed
    exit /b 0
)

rem Keep the process attached so crashes and the real exit code remain visible
echo.
echo Starting KiCad development build
pushd "%KICAD_BUILD_ROOT%\kicad"
"%KICAD_EXE%" %*
set "KICAD_EXIT_CODE=%ERRORLEVEL%"
popd

echo.
echo KiCad exited with code %KICAD_EXIT_CODE%
exit /b %KICAD_EXIT_CODE%
