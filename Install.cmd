@echo off
title Slippi blocklist - install
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dist\install.ps1"
echo.
pause
