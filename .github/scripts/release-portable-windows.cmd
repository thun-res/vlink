@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File ".github\scripts\release-portable-windows.ps1"
exit /b %ERRORLEVEL%
