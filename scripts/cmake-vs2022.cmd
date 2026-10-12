@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
rem Use the Qt-bundled CMake 3.30.5.  VS CMake 3.31.6 currently crashes
rem during spdlog's AUTOMOC step in this project.
set "CMAKE_EXE=G:\qt\Tools\CMake_64\bin\cmake.exe"
if not exist "%CMAKE_EXE%" (
    echo CMake not found: "%CMAKE_EXE%" 1>&2
    exit /b 1
)
"%CMAKE_EXE%" %*
