@echo off
setlocal

set "SUNSHINE_MSYS2_ROOT="

call :try_candidate "%MSYS2_ROOT%"
if defined SUNSHINE_MSYS2_ROOT goto found

if defined SystemDrive call :try_candidate "%SystemDrive%\msys64"
if defined SUNSHINE_MSYS2_ROOT goto found

for %%D in ("C:\msys64" "C:\tools\msys64") do (
    call :try_candidate "%%~fD"
    if defined SUNSHINE_MSYS2_ROOT goto found
)

for %%T in (msys2_shell.cmd bash.exe mingw32-make.exe clang++.exe clang.exe) do (
    for /f "delims=" %%I in ('where %%T 2^>nul') do (
        call :try_from_tool "%%~fI"
        if defined SUNSHINE_MSYS2_ROOT goto found
    )
)

echo MSYS2 installation not found. Set MSYS2_ROOT or add MSYS2 tools to PATH.>&2
exit /b 1

:found
set "SUNSHINE_MSYS2_SHELL=%SUNSHINE_MSYS2_ROOT%\msys2_shell.cmd"
set "SUNSHINE_MSYS2_USR_BIN=%SUNSHINE_MSYS2_ROOT%\usr\bin"
set "SUNSHINE_MSYS2_BASH=%SUNSHINE_MSYS2_USR_BIN%\bash.exe"

if not exist "%SUNSHINE_MSYS2_SHELL%" (
    echo MSYS2 shell not found at %SUNSHINE_MSYS2_SHELL%>&2
    exit /b 1
)
if not exist "%SUNSHINE_MSYS2_BASH%" (
    echo MSYS2 bash not found at %SUNSHINE_MSYS2_BASH%>&2
    exit /b 1
)

endlocal & (
    set "SUNSHINE_MSYS2_ROOT=%SUNSHINE_MSYS2_ROOT%"
    set "SUNSHINE_MSYS2_SHELL=%SUNSHINE_MSYS2_SHELL%"
    set "SUNSHINE_MSYS2_USR_BIN=%SUNSHINE_MSYS2_USR_BIN%"
    set "SUNSHINE_MSYS2_BASH=%SUNSHINE_MSYS2_BASH%"
)
exit /b 0

:try_candidate
set "CANDIDATE=%~1"
if not defined CANDIDATE exit /b 0
for %%R in ("%CANDIDATE%") do set "CANDIDATE=%%~fR"
if exist "%CANDIDATE%\msys2_shell.cmd" set "SUNSHINE_MSYS2_ROOT=%CANDIDATE%"
exit /b 0

:try_from_tool
set "TOOL_PATH=%~1"
if not defined TOOL_PATH exit /b 0

if /i "%~nx1"=="msys2_shell.cmd" (
    for %%R in ("%~dp1.") do call :try_candidate "%%~fR"
    exit /b 0
)

for %%R in ("%~dp1..\..") do call :try_candidate "%%~fR"
if defined SUNSHINE_MSYS2_ROOT exit /b 0

for %%R in ("%~dp1..") do call :try_candidate "%%~fR"
exit /b 0
