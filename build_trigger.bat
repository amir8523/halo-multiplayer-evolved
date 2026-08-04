@echo off
setlocal enabledelayedexpansion
set ROOT=%~dp0
set OUT=%ROOT%build
where cl.exe >nul 2>&1
if errorlevel 1 (
    set VCVARS=
    for %%E in (Community Professional Enterprise BuildTools) do (
        if exist "%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat" (
            if not defined VCVARS set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\%%E\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
    if not defined VCVARS ( echo ERROR: vcvars64.bat not found & exit /b 1 )
    call "!VCVARS!" >nul
)
if not exist "%OUT%\trig" mkdir "%OUT%\trig"
cl /nologo /std:c++20 /EHsc /W4 /permissive- /utf-8 /MT /O2 /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS "%ROOT%tools\trigger\FE_Trigger.cpp" /Fo"%OUT%\trig\\" /Fe"%OUT%\FE_Trigger.exe" /link /SUBSYSTEM:CONSOLE
if errorlevel 1 ( echo BUILD FAILED & exit /b 1 )
echo === Built FE_Trigger.exe ===
endlocal
