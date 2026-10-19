@echo off
rem Runs package_release.ps1 (builds release\MW5 Tactical Command Suite from the finished build)
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File package_release.ps1 > package_release.log 2>&1
echo done >> package_release.log
