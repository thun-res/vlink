@echo off
::
:: Build vlink via Conan + CMake on Windows, then assemble a portable archive
:: and a QtIFW installer.
::
:: This is the Windows counterpart of build-conan.sh:
::   - Pulls all dependencies via Conan (no system libs required)
::   - Builds with viewer (Qt) + webviz (foxglove) + examples enabled
::   - Bundles Qt / FFmpeg / OpenSSL / SQLite / Protobuf / FlatBuffers / zstd
::     DLLs into the output tree
::   - Produces:
::       1) portable archive  : build-conan\packup\win32\vlink-<ver>-windows-<arch>.zip
::       2) QtIFW installer   : build-conan\packup\win32\vlink-<ver>-windows-<arch>.exe
::
:: For DEB / RPM / Arch distribution packages on Linux, use
:: build-deb.sh / build-rpm.sh / build-arch.sh.
::
:: Usage:
::   build-conan.bat {project dir}
::
:: Required env:
::   QT_DIR    path to a Qt 5.x or 6.x install
::   QTIFW_DIR path to Qt Installer Framework root (optional; auto-detected)
::
:: Required system tools:
::   python + pip on PATH (to bootstrap conan)
::   cmake >= 3.15 on PATH
::   Visual Studio 2019+ with C++ workload (cl.exe reachable via vcvars)
::
setlocal enabledelayedexpansion

set VSLANG=1033
set DOTNET_CLI_UI_LANGUAGE=en

set "WORK_DIR=%~dp0"
set "WORK_DIR=!WORK_DIR:\=/!"
if "!WORK_DIR:~-1!"=="/" set "WORK_DIR=!WORK_DIR:~0,-1!"

set "GITHUB_URL="
set "PROJECT_DIR="

:parse_args
if "%~1"=="" goto :done_args
if "%~1"=="--github_url" (
    set "GITHUB_URL=%~2"
    shift
    shift
    goto :parse_args
)
if "%~1"=="-h" goto :show_help
if "%~1"=="--help" goto :show_help
set "PROJECT_DIR=%~1"
shift
goto :parse_args

:show_help
echo Usage:
echo   build-conan.bat [--github_url ^<url^>] {project dir}
echo.
echo Options:
echo   --github_url ^<url^>  override CPM_GITHUB_URL (default: https://github.com)
exit /b 0

:done_args
if "%PROJECT_DIR%"=="" (
    echo Usage:
    echo   build-conan.bat [--github_url ^<url^>] {project dir}
    exit /b 0
)

if "%QT_DIR%"=="" (
    echo QT_DIR env is empty!
    exit /b 1
)

where conan >nul 2>nul || pip install conan --user

if %errorlevel% neq 0 (
    exit /b 2
)

conan profile detect >nul 2>nul

echo.
echo ********************************************
echo *** conan install...
echo ********************************************
echo.

set "SRC_DIR=%PROJECT_DIR%"
set "SRC_DIR=!SRC_DIR:\=/!"
if "!SRC_DIR:~-1!"=="/" set "SRC_DIR=!SRC_DIR:~0,-1!"
set "BUILD_DIR=!SRC_DIR!/build-conan"
set "INSTALL_DIR=!BUILD_DIR!/install"
set "PACKUP_DIR=!BUILD_DIR!/packup/win32/vlink"

if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"

conan install "%SRC_DIR%" --output-folder="%BUILD_DIR%" --build=missing --profile default --profile %WORK_DIR%\conan_profile

if %errorlevel% neq 0 (
    exit /b 2
)

echo.
echo ********************************************
echo *** cmake build...
echo ********************************************
echo.

cmake -E make_directory "%BUILD_DIR%/output/lib"
cmake -E make_directory "%BUILD_DIR%/output/bin"

set "CMAKE_GITHUB_ARG="
if not "%GITHUB_URL%"=="" set "CMAKE_GITHUB_ARG=-DCPM_GITHUB_URL=%GITHUB_URL%"

cmake -S "%SRC_DIR%" -B "%BUILD_DIR%" -DCMAKE_TOOLCHAIN_FILE="%BUILD_DIR%/conan/conan_toolchain.cmake" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_INSTALL_PREFIX="%INSTALL_DIR%" ^
    -DCMAKE_PREFIX_PATH="%QT_DIR%" ^
    -DENABLE_SYMLINKS=ON ^
    -DENABLE_COMPLETIONS=ON ^
    -DENABLE_CPM=ON ^
    -DENABLE_IOX_ROUDI=ON ^
    -DENABLE_VIEWER=ON ^
    -DENABLE_VIEWER_FFMPEG=ON ^
    -DENABLE_VIEWER_OSG=ON ^
    -DENABLE_WEBVIZ=ON ^
    -DENABLE_WEBVIZ_FOXGLOVE=ON ^
    -DENABLE_WEBVIZ_RERUN=OFF ^
    %CMAKE_GITHUB_ARG%

if %errorlevel% neq 0 (
    exit /b 2
)

cmake --build "%BUILD_DIR%" --config Release -j8

if %errorlevel% neq 0 (
    exit /b 2
)

cmake --install "%BUILD_DIR%" --config Release

if %errorlevel% neq 0 (
    exit /b 2
)

echo.
echo ********************************************
echo *** copy runtime files...
echo ********************************************
echo.

if exist "%PACKUP_DIR%" rmdir /s /q "%PACKUP_DIR%"
cmake -E make_directory "%PACKUP_DIR%/bin"
cmake -E make_directory "%PACKUP_DIR%/lib/cmake"
cmake -E make_directory "%PACKUP_DIR%/include"
cmake -E make_directory "%PACKUP_DIR%/etc"

for /f "delims=" %%f in ('dir /b "%INSTALL_DIR%\bin\vlink-*.exe" 2^>nul') do cmake -E copy "%INSTALL_DIR%/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%INSTALL_DIR%\bin\vlink*.dll" 2^>nul') do cmake -E copy "%INSTALL_DIR%/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%INSTALL_DIR%\lib\vlink*.lib" 2^>nul') do cmake -E copy "%INSTALL_DIR%/lib/%%f" "%PACKUP_DIR%/lib/"
for /f "delims=" %%d in ('dir /b /ad "%INSTALL_DIR%\lib\cmake\vlink*" 2^>nul') do cmake -E copy_directory "%INSTALL_DIR%/lib/cmake/%%d" "%PACKUP_DIR%/lib/cmake/%%d"

if exist "%INSTALL_DIR%\include\vlink" cmake -E copy_directory "%INSTALL_DIR%/include/vlink" "%PACKUP_DIR%/include/vlink"
if exist "%INSTALL_DIR%\etc\vlink" cmake -E copy_directory "%INSTALL_DIR%/etc/vlink" "%PACKUP_DIR%/etc/vlink"
if exist "%PACKUP_DIR%\etc\vlink\licenses" rmdir /s /q "%PACKUP_DIR%\etc\vlink\licenses"

cmake -E copy "%SRC_DIR%/version.txt"   "%PACKUP_DIR%/"

for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\*avcodec*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\*avformat*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\*avutil*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\*swscale*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\*crypto*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\*protobuf*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\*flatbuffers*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\*ssl*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\*sqlite3*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\*zstd*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"

for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\protoc*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\bin\flatc*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\lib\libprotoc*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/lib/%%f" "%PACKUP_DIR%/lib/"
for /f "delims=" %%f in ('dir /b "%BUILD_DIR%\output\lib\libflatc*" 2^>nul') do cmake -E copy "%BUILD_DIR%/output/lib/%%f" "%PACKUP_DIR%/lib/"

if exist "%BUILD_DIR%\output\bin\iox-roudi.exe" (
    cmake -E copy "%BUILD_DIR%/output/bin/iox-roudi.exe" "%PACKUP_DIR%/bin/"
) else if exist "%BUILD_DIR%\iox-roudi.exe" (
    cmake -E copy "%BUILD_DIR%/iox-roudi.exe" "%PACKUP_DIR%/bin/"
)

set QT_VERSION=
for /f "delims=" %%i in ('"%QT_DIR%\bin\qmake" -query QT_VERSION') do set QT_VERSION=%%i
echo QT_VERSION=%QT_VERSION%

echo.
echo ********************************************
echo *** copy qt files...
echo ********************************************
echo.

cmake -E make_directory "%PACKUP_DIR%/bin/platforms"
cmake -E make_directory "%PACKUP_DIR%/bin/imageformats"
cmake -E make_directory "%PACKUP_DIR%/bin/sqldrivers"
cmake -E make_directory "%PACKUP_DIR%/bin/styles"

if "%QT_VERSION:~0,2%"=="5." (
    cmake -E copy "%QT_DIR%/bin/Qt5Core.dll"                                "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt5Gui.dll"                                 "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt5Widgets.dll"                             "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt5Sql.dll"                                 "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt5OpenGL.dll"                              "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt5Network.dll"                             "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt5Svg.dll"                                 "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/plugins/platforms/qwindows.dll"                 "%PACKUP_DIR%/bin/platforms/"
    cmake -E copy "%QT_DIR%/plugins/imageformats/qgif.dll"                  "%PACKUP_DIR%/bin/imageformats/"
    cmake -E copy "%QT_DIR%/plugins/imageformats/qico.dll"                  "%PACKUP_DIR%/bin/imageformats/"
    cmake -E copy "%QT_DIR%/plugins/imageformats/qjpeg.dll"                 "%PACKUP_DIR%/bin/imageformats/"
    cmake -E copy "%QT_DIR%/plugins/imageformats/qsvg.dll"                  "%PACKUP_DIR%/bin/imageformats/"
    cmake -E copy "%QT_DIR%/plugins/sqldrivers/qsqlite.dll"                 "%PACKUP_DIR%/bin/sqldrivers/"
    cmake -E copy "%QT_DIR%/plugins/styles/qwindowsvistastyle.dll"          "%PACKUP_DIR%/bin/styles/"

) else if "%QT_VERSION:~0,2%"=="6." (
    cmake -E copy "%QT_DIR%/bin/Qt6Core.dll"                                "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt6Gui.dll"                                 "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt6Widgets.dll"                             "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt6Sql.dll"                                 "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt6OpenGL.dll"                              "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt6OpenGLWidgets.dll"                       "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt6Network.dll"                             "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/bin/Qt6Svg.dll"                                 "%PACKUP_DIR%/bin/"
    cmake -E copy "%QT_DIR%/plugins/platforms/qwindows.dll"                 "%PACKUP_DIR%/bin/platforms/"
    cmake -E copy "%QT_DIR%/plugins/imageformats/qgif.dll"                  "%PACKUP_DIR%/bin/imageformats/"
    cmake -E copy "%QT_DIR%/plugins/imageformats/qico.dll"                  "%PACKUP_DIR%/bin/imageformats/"
    cmake -E copy "%QT_DIR%/plugins/imageformats/qjpeg.dll"                 "%PACKUP_DIR%/bin/imageformats/"
    cmake -E copy "%QT_DIR%/plugins/imageformats/qsvg.dll"                  "%PACKUP_DIR%/bin/imageformats/"
    cmake -E copy "%QT_DIR%/plugins/sqldrivers/qsqlite.dll"                 "%PACKUP_DIR%/bin/sqldrivers/"
    if exist "%QT_DIR%\plugins\styles\qwindowsvistastyle.dll"  cmake -E copy "%QT_DIR%/plugins/styles/qwindowsvistastyle.dll"  "%PACKUP_DIR%/bin/styles/"
    if exist "%QT_DIR%\plugins\styles\qmodernwindowsstyle.dll" cmake -E copy "%QT_DIR%/plugins/styles/qmodernwindowsstyle.dll" "%PACKUP_DIR%/bin/styles/"

) else (
    echo QT_VERSION error, QT_VERSION=%QT_VERSION%
    exit /b 3
)

set OSG_VERSION=3.6.5
set OSG_PREFIX2=ot21-
set OSG_PREFIX3=osg161-

if not "%OSG_DIR%"=="" (
    echo OSG_VERSION=%OSG_VERSION%

    echo.
    echo ********************************************
    echo *** copy osg files...
    echo ********************************************
    echo.

    cmake -E make_directory "%PACKUP_DIR%/bin/osgPlugins-%OSG_VERSION%"
    cmake -E copy "%OSG_DIR%/bin/%OSG_PREFIX2%OpenThreads.dll"                               "%PACKUP_DIR%/bin/"
    cmake -E copy "%OSG_DIR%/bin/%OSG_PREFIX3%osg.dll"                                       "%PACKUP_DIR%/bin/"
    cmake -E copy "%OSG_DIR%/bin/%OSG_PREFIX3%osgDB.dll"                                     "%PACKUP_DIR%/bin/"
    cmake -E copy "%OSG_DIR%/bin/%OSG_PREFIX3%osgGA.dll"                                     "%PACKUP_DIR%/bin/"
    cmake -E copy "%OSG_DIR%/bin/%OSG_PREFIX3%osgManipulator.dll"                            "%PACKUP_DIR%/bin/"
    cmake -E copy "%OSG_DIR%/bin/%OSG_PREFIX3%osgUtil.dll"                                   "%PACKUP_DIR%/bin/"
    cmake -E copy "%OSG_DIR%/bin/%OSG_PREFIX3%osgText.dll"                                   "%PACKUP_DIR%/bin/"
    cmake -E copy "%OSG_DIR%/bin/%OSG_PREFIX3%osgViewer.dll"                                 "%PACKUP_DIR%/bin/"
    cmake -E copy "%OSG_DIR%/bin/osgPlugins-%OSG_VERSION%/osgdb_freetype.dll"                "%PACKUP_DIR%/bin/osgPlugins-%OSG_VERSION%/"
    cmake -E copy "%OSG_DIR%/bin/osgPlugins-%OSG_VERSION%/osgdb_obj.dll"                     "%PACKUP_DIR%/bin/osgPlugins-%OSG_VERSION%/"
    cmake -E copy "%OSG_DIR%/bin/osgPlugins-%OSG_VERSION%/osgdb_osg.dll"                     "%PACKUP_DIR%/bin/osgPlugins-%OSG_VERSION%/"
    cmake -E copy "%OSG_DIR%/bin/osgPlugins-%OSG_VERSION%/osgdb_serializers_osg.dll"         "%PACKUP_DIR%/bin/osgPlugins-%OSG_VERSION%/"
    if exist "%OSG_DIR%\bin\zlib.dll" cmake -E copy "%OSG_DIR%/bin/zlib.dll"                 "%PACKUP_DIR%/bin/"
)

for /f "delims=" %%f in ('dir /b "%QT_DIR%\bin\libcrypto*.dll" 2^>nul') do cmake -E copy "%QT_DIR%/bin/%%f" "%PACKUP_DIR%/bin/"
for /f "delims=" %%f in ('dir /b "%QT_DIR%\bin\libssl*.dll" 2^>nul') do cmake -E copy "%QT_DIR%/bin/%%f" "%PACKUP_DIR%/bin/"

for %%f in (run_cmd.bat qt.conf) do (
    if exist "%WORK_DIR%\win32\%%f" cmake -E copy "%WORK_DIR%/win32/%%f" "%PACKUP_DIR%/bin/"
)
if exist "%WORK_DIR%\win32\install.bat" cmake -E copy "%WORK_DIR%/win32/install.bat" "%PACKUP_DIR%/"
if exist "%WORK_DIR%\win32\uninstall.bat" cmake -E copy "%WORK_DIR%/win32/uninstall.bat" "%PACKUP_DIR%/"
if exist "%WORK_DIR%\win32\setup_runtime.bat" cmake -E copy "%WORK_DIR%/win32/setup_runtime.bat" "%PACKUP_DIR%/"
for %%f in (msvcp140.dll msvcp140_1.dll msvcp140_2.dll vcruntime140.dll vcruntime140_1.dll vcruntime140_threads.dll) do (
    if exist "%WORK_DIR%\win32\%%f" cmake -E copy "%WORK_DIR%/win32/%%f" "%PACKUP_DIR%/bin/"
)
if exist "%WORK_DIR%\win32\terminal.zip" powershell -NoProfile -Command "Expand-Archive -Path '%WORK_DIR%/win32/terminal.zip' -DestinationPath '%PACKUP_DIR%' -Force"

echo.
echo ********************************************
echo *** aggregating third-party license files...
echo ********************************************
echo.

cmake -E make_directory "%PACKUP_DIR%/licenses"

set "_lic_found="
for /d %%d in ("%INSTALL_DIR%\*") do (
    if exist "%%d\vlink\licenses" if not defined _lic_found (
        cmake -E copy_directory "%%d/vlink/licenses" "%PACKUP_DIR%/licenses"
        set "_lic_found=1"
    )
)

if exist "%BUILD_DIR%\output\licenses" (
    for /d %%d in ("%BUILD_DIR%\output\licenses\*") do (
        cmake -E make_directory "%PACKUP_DIR%/licenses/%%~nxd"
        cmake -E copy_directory "%%d" "%PACKUP_DIR%/licenses/%%~nxd"
    )
)

if not "%QT_DIR%"=="" (
    cmake -E make_directory "%PACKUP_DIR%/licenses/qt"
    if exist "%WORK_DIR%\licenses\qt\README.md" cmake -E copy "%WORK_DIR%/licenses/qt/README.md" "%PACKUP_DIR%/licenses/qt/"
    for %%p in (LICENSE LGPL GPL COPYING) do (
        for /f "delims=" %%f in ('dir /b "%QT_DIR%\%%p*" 2^>nul') do cmake -E copy "%QT_DIR%/%%f" "%PACKUP_DIR%/licenses/qt/"
    )
    if exist "%QT_DIR%\licenses" (
        cmake -E copy_directory "%QT_DIR%/licenses" "%PACKUP_DIR%/licenses/qt/"
    ) else if exist "%QT_DIR%\Licenses" (
        cmake -E copy_directory "%QT_DIR%/Licenses" "%PACKUP_DIR%/licenses/qt/"
    ) else if exist "%QT_DIR%\..\Licenses" (
        cmake -E copy_directory "%QT_DIR%/../Licenses" "%PACKUP_DIR%/licenses/qt/"
    )
    set "_has_icu=0"
    for /f "delims=" %%f in ('dir /b "%PACKUP_DIR%\bin\icu*.dll" 2^>nul') do set "_has_icu=1"
    if "!_has_icu!"=="1" (
        cmake -E make_directory "%PACKUP_DIR%/licenses/icu"
        if exist "%WORK_DIR%\licenses\icu\README.md" cmake -E copy "%WORK_DIR%/licenses/icu/README.md" "%PACKUP_DIR%/licenses/icu/"
    )
)

if not "%OSG_DIR%"=="" (
    cmake -E make_directory "%PACKUP_DIR%/licenses/osg"
    if exist "%WORK_DIR%\licenses\osg\README.md" cmake -E copy "%WORK_DIR%/licenses/osg/README.md" "%PACKUP_DIR%/licenses/osg/"
    for %%p in (LICENSE COPYING) do (
        for /f "delims=" %%f in ('dir /b "%OSG_DIR%\%%p*" 2^>nul') do cmake -E copy "%OSG_DIR%/%%f" "%PACKUP_DIR%/licenses/osg/"
    )
    if exist "%OSG_DIR%\share\OpenSceneGraph\LICENSE.txt" cmake -E copy "%OSG_DIR%/share/OpenSceneGraph/LICENSE.txt" "%PACKUP_DIR%/licenses/osg/"
    if exist "%OSG_DIR%\doc\LICENSE.txt" cmake -E copy "%OSG_DIR%/doc/LICENSE.txt" "%PACKUP_DIR%/licenses/osg/"
)

echo Licenses aggregated to: %PACKUP_DIR%\licenses
dir /b "%PACKUP_DIR%\licenses" 2>nul

echo.
echo ********************************************
echo *** creating portable archive...
echo ********************************************
echo.

for /f "usebackq delims=" %%a in ("%SRC_DIR%\version.txt") do set "VERSION=%%a"
set "ARCHIVE_NAME=vlink-%VERSION%-win64.zip"
set "ARCHIVE_DIR=%BUILD_DIR%\packup\win32"
set "ARCHIVE_PATH=%ARCHIVE_DIR%\%ARCHIVE_NAME%"

echo Creating portable archive: %ARCHIVE_PATH%
powershell -NoProfile -Command "Compress-Archive -Path '%PACKUP_DIR%' -DestinationPath '%ARCHIVE_PATH%' -Force"
echo Portable archive: %ARCHIVE_PATH%

echo.
echo ********************************************
echo *** creating installer package...
echo ********************************************
echo.

set "BINARYCREATOR="

if not "%QTIFW_DIR%"=="" (
    set "BINARYCREATOR=%QTIFW_DIR%\bin\binarycreator.exe"
    goto :found_bc
)

where binarycreator.exe >nul 2>nul
if %errorlevel% equ 0 (
    set "BINARYCREATOR=binarycreator.exe"
    goto :found_bc
)

for /d %%d in ("%USERPROFILE%\Qt\Tools\QtInstallerFramework\*") do (
    if exist "%%d\bin\binarycreator.exe" (
        set "BINARYCREATOR=%%d\bin\binarycreator.exe"
        goto :found_bc
    )
)
for /d %%d in ("C:\Qt\Tools\QtInstallerFramework\*") do (
    if exist "%%d\bin\binarycreator.exe" (
        set "BINARYCREATOR=%%d\bin\binarycreator.exe"
        goto :found_bc
    )
)

echo Warning: binarycreator not found, skipping installer creation.
echo Install Qt Installer Framework and set QTIFW_DIR or add binarycreator to PATH.
echo.
echo Packup directory: %PACKUP_DIR%
echo Done.
exit /b 0

:found_bc

if not exist "%WORK_DIR%\installer\config\config.xml" (
    echo Warning: Installer templates not found, skipping installer creation.
    echo Packup directory: %PACKUP_DIR%
    echo Done.
    exit /b 0
)

echo Using binarycreator: %BINARYCREATOR%

for /f "usebackq delims=" %%a in ("%SRC_DIR%\version.txt") do set "VERSION=%%a"
for /f %%i in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd"') do set DATE=%%i

set "INSTALLER_DIR=%BUILD_DIR%/installer"
set "INSTALLER_CONFIG=%INSTALLER_DIR%/config"
set "INSTALLER_PKG=%INSTALLER_DIR%/packages/com.vlink"
set "INSTALLER_META=%INSTALLER_PKG%/meta"
set "INSTALLER_DATA=%INSTALLER_PKG%/data"

if exist "%INSTALLER_DIR%" rmdir /s /q "%INSTALLER_DIR%"
cmake -E make_directory "%INSTALLER_CONFIG%"
cmake -E make_directory "%INSTALLER_META%"
cmake -E make_directory "%INSTALLER_DATA%"

cmake -E copy_directory "%WORK_DIR%/installer/config" "%INSTALLER_CONFIG%"

powershell -NoProfile -Command ^
    "(Get-Content '%WORK_DIR%\installer\config\config.xml' -Encoding UTF8) -replace '@VERSION@','%VERSION%' -replace '@DEFAULT_TARGET_DIR@','@HomeDir@/vlink' -replace '@MAINTENANCE_NAME@','vlink-uninstall' | Set-Content '%INSTALLER_CONFIG%\config.xml' -Encoding UTF8"

powershell -NoProfile -Command ^
    "(Get-Content '%WORK_DIR%\installer\packages\com.vlink\meta\package.xml' -Encoding UTF8) -replace '@VERSION@','%VERSION%' -replace '@DATE@','%DATE%' | Set-Content '%INSTALLER_META%\package.xml' -Encoding UTF8"

cmake -E copy "%WORK_DIR%/installer/packages/com.vlink/meta/installscript.qs" "%INSTALLER_META%/installscript.qs"
cmake -E copy "%SRC_DIR%/LICENSE" "%INSTALLER_META%/LICENSE"
if exist "%PACKUP_DIR%\licenses\LICENSE_NOTICES.md" (
    cmake -E copy "%PACKUP_DIR%/licenses/LICENSE_NOTICES.md" "%INSTALLER_META%/LICENSE_NOTICES.md"
) else if exist "%BUILD_DIR%\LICENSE_NOTICES.md" (
    cmake -E copy "%BUILD_DIR%/LICENSE_NOTICES.md" "%INSTALLER_META%/LICENSE_NOTICES.md"
)

echo Copying packup files to installer data directory...
cmake -E copy_directory "%PACKUP_DIR%" "%INSTALLER_DATA%"

set "OUTPUT_NAME=vlink-%VERSION%-win64-setup"
set "OUTPUT_DIR=%BUILD_DIR%\packup\win32"
set "OUTPUT_PATH=%OUTPUT_DIR%\%OUTPUT_NAME%.exe"

echo Creating installer: %OUTPUT_PATH%
echo.

"%BINARYCREATOR%" --offline-only ^
    -c "%INSTALLER_CONFIG%/config.xml" ^
    -p "%INSTALLER_DIR%/packages" ^
    "%OUTPUT_PATH%"

if %errorlevel% neq 0 (
    echo Error: binarycreator failed!
    echo Packup directory: %PACKUP_DIR%
    exit /b 2
) else (
    echo.
    echo ********************************************
    echo *** Installer created: %OUTPUT_PATH%
    echo ********************************************
)

echo.
echo Done.

exit /b 0
