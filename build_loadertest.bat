@echo off
setlocal enabledelayedexpansion
where cl.exe >nul 2>&1
if errorlevel 1 call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%~dp0build\lt" mkdir "%~dp0build\lt"
cl /nologo /std:c++20 /EHsc /W4 /utf-8 /MT /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX "%~dp0tools\probe\LoaderTest.cpp" /Fo"%~dp0build\lt\\" /Fe"%~dp0build\LoaderTest.exe" /link /SUBSYSTEM:CONSOLE
endlocal
