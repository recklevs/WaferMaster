@echo off
call "G:\visual studio\2022\community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
"G:\visual studio\2022\community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" %*
