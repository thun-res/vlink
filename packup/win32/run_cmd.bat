@echo off
for %%I in ("%~dp0..") do set "VLINK_BIN_DIR=%%~fI"
for %%I in ("%VLINK_BIN_DIR%\terminal\wt.exe") do set "VLINK_WT=%%~sI"
for %%I in ("%~dp0..\setup_runtime.bat") do set "VLINK_SETUP=%%~sI"

"%VLINK_WT%" -p "Command Prompt" --title "VLink CMD" --startingDirectory "%USERPROFILE%" cmd /k "call \"%VLINK_SETUP%\""
