@echo off
setlocal enabledelayedexpansion
where cl.exe >nul 2>&1
if errorlevel 1 call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist "%~dp0build\xr" mkdir "%~dp0build\xr"
cl /nologo /std:c++20 /EHsc /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /MT /O2 /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I"%~dp0src" "%~dp0tools\probe\XrefProbe.cpp" "%~dp0src\Core\Result.cpp" "%~dp0src\Core\Log.cpp" "%~dp0src\Core\Json.cpp" "%~dp0src\Blam\ModuleImage.cpp" "%~dp0src\Blam\PatternScanner.cpp" "%~dp0src\Blam\SymbolRegistry.cpp" /Fo"%~dp0build\xr\\" /Fe"%~dp0build\XrefProbe.exe" /link /SUBSYSTEM:CONSOLE
endlocal
