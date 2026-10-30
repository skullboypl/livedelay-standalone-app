@echo off
rem Builds the C++ window. The delay engine is livedelay.dll from the plugin repo.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
set CMAKE="C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
cd /d %~dp0
%CMAKE% -S . -B build -G "Visual Studio 17 2022" -A x64 || exit /b 1
%CMAKE% --build build --config Release || exit /b 1
set DLL=%~dp0..\livedelay\target\release\livedelay.dll
if exist "%DLL%" (
  copy /Y "%DLL%" "build\Release\livedelay.dll" >nul
  echo Copied livedelay.dll from the plugin repo.
) else (
  echo No ..\livedelay\target\release\livedelay.dll yet. In the livedelay repo run: cargo build --release
)
