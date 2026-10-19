@echo off
rem Builds everything in out\build\poc-x64-Debug (same as VS Build All), output in build_all.log.
rem vcvars64 and the build run together in one child cmd (calling vcvars from this script ended the script).
set "ROOT=%~dp0"
cd /d "%ROOT%"
set "VSCMD_START_DIR=%ROOT%"
echo started %date% %time% > "%ROOT%build_all.log"
cmd /c ""C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1 & cd /d "%ROOT%" & "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "%ROOT%out\build\poc-x64-Debug"" >> "%ROOT%build_all.log" 2>&1
echo exit %errorlevel% %time% >> "%ROOT%build_all.log"
