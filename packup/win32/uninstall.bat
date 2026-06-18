@echo off
setlocal enableextensions

set "APP_NAME_VIEWER=VLink Viewer"
set "APP_NAME_PLAYER=VLink Player"
set "APP_NAME_ANALYZER=VLink Analyzer"
set "APP_NAME_CMD=VLink CMD"
set "TERMINAL_DIR=%~dp0terminal"

for /f "delims=" %%D in ('powershell -NoProfile -Command "[Console]::Out.Write([Environment]::GetFolderPath('DesktopDirectory'))"') do set "DESKTOP_DIR=%%D"
for /f "delims=" %%S in ('powershell -NoProfile -Command "[Console]::Out.Write([Environment]::GetFolderPath('Programs'))"')         do set "PROGRAMS_DIR=%%S"
if not defined DESKTOP_DIR  set "DESKTOP_DIR=%USERPROFILE%\Desktop"
if not defined PROGRAMS_DIR set "PROGRAMS_DIR=%APPDATA%\Microsoft\Windows\Start Menu\Programs"
set "START_MENU_FOLDER=%PROGRAMS_DIR%\%APP_NAME_VIEWER%"

echo Uninstall...

for %%N in ("%APP_NAME_VIEWER%" "%APP_NAME_PLAYER%" "%APP_NAME_ANALYZER%" "%APP_NAME_CMD%") do (
    if exist "%DESKTOP_DIR%\%%~N.lnk"           del /q "%DESKTOP_DIR%\%%~N.lnk"           >nul 2>&1
    if exist "%USERPROFILE%\Desktop\%%~N.lnk"   del /q "%USERPROFILE%\Desktop\%%~N.lnk"   >nul 2>&1
    if exist "%PUBLIC%\Desktop\%%~N.lnk"        del /q "%PUBLIC%\Desktop\%%~N.lnk"        >nul 2>&1
)

if exist "%START_MENU_FOLDER%"                                                          rmdir /s /q "%START_MENU_FOLDER%"                                                          >nul 2>&1
if exist "%APPDATA%\Microsoft\Windows\Start Menu\Programs\%APP_NAME_VIEWER%"            rmdir /s /q "%APPDATA%\Microsoft\Windows\Start Menu\Programs\%APP_NAME_VIEWER%"            >nul 2>&1
if exist "%ProgramData%\Microsoft\Windows\Start Menu\Programs\%APP_NAME_VIEWER%"        rmdir /s /q "%ProgramData%\Microsoft\Windows\Start Menu\Programs\%APP_NAME_VIEWER%"        >nul 2>&1

set "PS_TERM=%TERMINAL_DIR%"
powershell -NoProfile -Command "if (Test-Path $env:PS_TERM) { Remove-Item ($env:PS_TERM + '\*') -Recurse -Force }" >nul 2>&1
if exist "%TERMINAL_DIR%" rmdir /s /q "%TERMINAL_DIR%" >nul 2>&1

for %%E in (vdb vdbx vcap vcapx) do (
    reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.%%E\UserChoice" /f >nul 2>&1
    reg delete "HKCU\Software\Classes\.%%E"  /f >nul 2>&1
)
for %%P in (vdbfile vdbxfile vcapfile vcapxfile) do (
    reg delete "HKCU\Software\Classes\%%P"   /f >nul 2>&1
)

powershell -NoProfile -Command "Add-Type -MemberDefinition '[DllImport(\"shell32.dll\")] public static extern void SHChangeNotify(int e, int f, IntPtr a, IntPtr b);' -Name N -Namespace VLink; [VLink.N]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)" >nul 2>&1

ie4uinit.exe -show           >nul 2>&1
ie4uinit.exe -ClearIconCache >nul 2>&1

echo Done.
exit /b 0
