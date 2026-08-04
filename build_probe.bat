@echo off
setlocal enabledelayedexpansion

rem Builds tools\probe\SymbolProbe.exe, the offline validator for Blam symbol
rem discovery. Kept separate from build.bat because it is a developer tool and is
rem never shipped to players.

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
    if not defined VCVARS (
        echo ERROR: vcvars64.bat not found.
        exit /b 1
    )
    call "!VCVARS!" >nul
)

if not exist "%OUT%" mkdir "%OUT%"
rem Objects go in their own directory: /Fo needs a trailing separator to mean
rem "directory", and sharing one with the mod build would mix object files.
if not exist "%OUT%\probe" mkdir "%OUT%\probe"

set FLAGS=/nologo /std:c++20 /EHsc /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8 /MT /O2 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /I"%ROOT%src"

echo === Building SymbolProbe.exe ===
cl %FLAGS% ^
 "%ROOT%tools\probe\SymbolProbe.cpp" ^
 "%ROOT%src\Core\Result.cpp" ^
 "%ROOT%src\Core\Log.cpp" ^
 "%ROOT%src\Core\Json.cpp" ^
 "%ROOT%src\Blam\ModuleImage.cpp" ^
 "%ROOT%src\Blam\PatternScanner.cpp" ^
 "%ROOT%src\Blam\SymbolRegistry.cpp" ^
 /Fo"%OUT%\probe\\" /Fe"%OUT%\SymbolProbe.exe" /link /SUBSYSTEM:CONSOLE

if errorlevel 1 (
    echo BUILD FAILED
    exit /b 1
)
echo === Built %OUT%\SymbolProbe.exe ===
endlocal
