@echo off
title Slippi blocklist - uninstall
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall
echo.
pause
