@echo off
setlocal enableextensions

set "APP_NAME_VIEWER=VLink Viewer"
set "APP_NAME_PLAYER=VLink Player"
set "APP_NAME_ANALYZER=VLink Analyzer"
set "APP_NAME_CMD=VLink CMD"
set "APP_PATH_VIEWER=%~dp0bin\vlink-viewer.exe"
set "APP_PATH_PLAYER=%~dp0bin\vlink-player.exe"
set "APP_PATH_ANALYZER=%~dp0bin\vlink-analyzer.exe"
set "APP_PATH_CMD=%~dp0bin\run_cmd.bat"
set "APP_PATH_PROXY=%~dp0bin\vlink-proxy.exe"

for /f "delims=" %%D in ('powershell -NoProfile -Command "[Console]::Out.Write([Environment]::GetFolderPath('DesktopDirectory'))"') do set "DESKTOP_DIR=%%D"
for /f "delims=" %%S in ('powershell -NoProfile -Command "[Console]::Out.Write([Environment]::GetFolderPath('Programs'))"')         do set "PROGRAMS_DIR=%%S"
if not defined DESKTOP_DIR  set "DESKTOP_DIR=%USERPROFILE%\Desktop"
if not defined PROGRAMS_DIR set "PROGRAMS_DIR=%APPDATA%\Microsoft\Windows\Start Menu\Programs"
set "START_MENU_FOLDER=%PROGRAMS_DIR%\%APP_NAME_VIEWER%"

echo Installing...

if not exist "%DESKTOP_DIR%"       mkdir "%DESKTOP_DIR%"       >nul 2>&1
if not exist "%START_MENU_FOLDER%" mkdir "%START_MENU_FOLDER%" >nul 2>&1

call :make_lnk "%DESKTOP_DIR%\%APP_NAME_VIEWER%.lnk"          "%APP_PATH_VIEWER%"   "%APP_PATH_VIEWER%"   1
call :make_lnk "%DESKTOP_DIR%\%APP_NAME_PLAYER%.lnk"          "%APP_PATH_PLAYER%"   "%APP_PATH_PLAYER%"   1
call :make_lnk "%DESKTOP_DIR%\%APP_NAME_ANALYZER%.lnk"        "%APP_PATH_ANALYZER%" "%APP_PATH_ANALYZER%" 1
call :make_lnk "%DESKTOP_DIR%\%APP_NAME_CMD%.lnk"             "%APP_PATH_CMD%"      "%APP_PATH_PROXY%"    7
call :make_lnk "%START_MENU_FOLDER%\%APP_NAME_VIEWER%.lnk"    "%APP_PATH_VIEWER%"   "%APP_PATH_VIEWER%"   1
call :make_lnk "%START_MENU_FOLDER%\%APP_NAME_PLAYER%.lnk"    "%APP_PATH_PLAYER%"   "%APP_PATH_PLAYER%"   1
call :make_lnk "%START_MENU_FOLDER%\%APP_NAME_ANALYZER%.lnk"  "%APP_PATH_ANALYZER%" "%APP_PATH_ANALYZER%" 1
call :make_lnk "%START_MENU_FOLDER%\%APP_NAME_CMD%.lnk"       "%APP_PATH_CMD%"      "%APP_PATH_PROXY%"    7

for %%E in (vdb vdbx vcap vcapx) do (
    reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.%%E\UserChoice" /f >nul 2>&1
    reg delete "HKCU\Software\Classes\.%%E"  /f >nul 2>&1
)
for %%P in (vdbfile vdbxfile vcapfile vcapxfile) do (
    reg delete "HKCU\Software\Classes\%%P"   /f >nul 2>&1
)

reg add "HKCU\Software\Classes\.vdb"   /ve /t REG_SZ /d "vdbfile"   /f >nul 2>&1
reg add "HKCU\Software\Classes\.vdbx"  /ve /t REG_SZ /d "vdbxfile"  /f >nul 2>&1
reg add "HKCU\Software\Classes\.vcap"  /ve /t REG_SZ /d "vcapfile"  /f >nul 2>&1
reg add "HKCU\Software\Classes\.vcapx" /ve /t REG_SZ /d "vcapxfile" /f >nul 2>&1

for %%P in (vdbfile vdbxfile vcapfile vcapxfile) do (
    reg add "HKCU\Software\Classes\%%P"                    /ve /t REG_SZ /d "VLink Data"                    /f >nul 2>&1
    reg add "HKCU\Software\Classes\%%P\DefaultIcon"        /ve /t REG_SZ /d "%APP_PATH_PLAYER%,0"           /f >nul 2>&1
    reg add "HKCU\Software\Classes\%%P\shell\open\command" /ve /t REG_SZ /d "\"%APP_PATH_PLAYER%\" \"%%1\"" /f >nul 2>&1
)

powershell -NoProfile -Command "Add-Type -MemberDefinition '[DllImport(\"shell32.dll\")] public static extern void SHChangeNotify(int e, int f, IntPtr a, IntPtr b);' -Name N -Namespace VLink; [VLink.N]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)" >nul 2>&1

ie4uinit.exe -show           >nul 2>&1
ie4uinit.exe -ClearIconCache >nul 2>&1

echo Done.
exit /b 0

:make_lnk
set "SC_TARGET=%~1"
set "SC_EXE=%~2"
set "SC_ICON=%~3"
set "SC_WS=%~4"
powershell -NoProfile -Command "$ws = New-Object -ComObject WScript.Shell; $s = $ws.CreateShortcut($env:SC_TARGET); $s.TargetPath = $env:SC_EXE; $s.IconLocation = ($env:SC_ICON + ',0'); $s.WindowStyle = [int]$env:SC_WS; $s.Save()" 2>nul
exit /b 0
