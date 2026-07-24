@echo off

chcp 65001 >nul

set "VLINK_ROOT_DIR=%~dp0"
set "VLINK_ETC_DIR=%VLINK_ROOT_DIR%\etc"

set "VLINK_PROTO_DIR_CONFIG=%USERPROFILE%\.vlink_proto_dir"
set "VLINK_FBS_DIR_CONFIG=%USERPROFILE%\.vlink_fbs_dir"

cls
echo Setup vlink runtime...
echo +--------------------------------------+
echo ^|  _    __   __      _           __    ^|
echo ^| ^| ^|  / /  / /     ^(_^) ____    / /__  ^|
echo ^| ^| ^| / /  / /     / / / __ \  / //_ / ^|
echo ^| ^| ^|/ /  / /___  / / / / / / / ,^<     ^|
echo ^| ^|___/  /_____/ /_/ /_/ /_/ /_/^|_^|    ^|
echo ^|                                      ^|
echo +--------------------------------------+

setlocal DisableDelayedExpansion
set "version="
if exist "%VLINK_ROOT_DIR%\version.txt" set /p "version="<"%VLINK_ROOT_DIR%\version.txt"
if defined version echo Version: %version%
endlocal

setlocal DisableDelayedExpansion
set "proto_dir="
if exist "%VLINK_PROTO_DIR_CONFIG%" set /p "proto_dir="<"%VLINK_PROTO_DIR_CONFIG%"
if defined proto_dir (
    echo VLINK_PROTO_DIR: "%proto_dir%"
    endlocal & set "VLINK_PROTO_DIR=%proto_dir%"
) else (
    endlocal
)

setlocal DisableDelayedExpansion
set "fbs_dir="
if exist "%VLINK_FBS_DIR_CONFIG%" set /p "fbs_dir="<"%VLINK_FBS_DIR_CONFIG%"
if defined fbs_dir (
    echo VLINK_FBS_DIR: "%fbs_dir%"
    endlocal & set "VLINK_FBS_DIR=%fbs_dir%"
) else (
    endlocal
)

echo Support commands: [proxy] [info] [monitor] [bag] [trigger] [list] [eproto] [efbs] [parse] [check] [bench] [viewer] [player] [analyzer] [webviz]
echo.

echo ;%PATH%; | findstr /C:";%VLINK_ROOT_DIR%\bin;" >nul 2>&1 || set "PATH=%VLINK_ROOT_DIR%\bin;%PATH%"

set "VLINK_DIR=%VLINK_ROOT_DIR%"
set "vlink_DIR=%VLINK_ROOT_DIR%\lib\cmake\vlink"
echo ;%CMAKE_PREFIX_PATH%; | findstr /C:";%VLINK_ROOT_DIR%;" >nul 2>&1 || (if "%CMAKE_PREFIX_PATH%"=="" (set "CMAKE_PREFIX_PATH=%VLINK_ROOT_DIR%") else (set "CMAKE_PREFIX_PATH=%VLINK_ROOT_DIR%;%CMAKE_PREFIX_PATH%"))

@REM set "VLINK_PROTOC_PROGRAM=%VLINK_ROOT_DIR%\bin\protoc"
@REM set "VLINK_FLATC_PROGRAM=%VLINK_ROOT_DIR%\bin\flatc"

doskey proxy="%VLINK_ROOT_DIR%\bin\vlink-proxy" $*
doskey info="%VLINK_ROOT_DIR%\bin\vlink-info" $*
doskey monitor="%VLINK_ROOT_DIR%\bin\vlink-monitor" $*
doskey bag="%VLINK_ROOT_DIR%\bin\vlink-bag" $*
doskey trigger="%VLINK_ROOT_DIR%\bin\vlink-trigger" $*
doskey list="%VLINK_ROOT_DIR%\bin\vlink-list" $*
doskey eproto="%VLINK_ROOT_DIR%\bin\vlink-eproto" $*
doskey efbs="%VLINK_ROOT_DIR%\bin\vlink-efbs" $*
doskey parse="%VLINK_ROOT_DIR%\bin\vlink-parse" $*
doskey check="%VLINK_ROOT_DIR%\bin\vlink-check" $*
doskey bench="%VLINK_ROOT_DIR%\bin\vlink-bench" $*
doskey viewer="%VLINK_ROOT_DIR%\bin\vlink-viewer" $*
doskey player="%VLINK_ROOT_DIR%\bin\vlink-player" $*
doskey analyzer="%VLINK_ROOT_DIR%\bin\vlink-analyzer" $*
doskey webviz_foxglove="%VLINK_ROOT_DIR%\bin\vlink-foxglove" $*
doskey webviz="%VLINK_ROOT_DIR%\bin\vlink-foxglove" $*
doskey bag2mcap="%VLINK_ROOT_DIR%\bin\vlink-bag2mcap" $*
doskey kill_proxy=taskkill /IM vlink-proxy.exe /F
