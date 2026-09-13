@echo off
title Slippi blocklist - uninstall
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dist\install.ps1" -Uninstall
echo.
pause
