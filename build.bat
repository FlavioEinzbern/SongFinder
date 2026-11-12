@echo off
REM Temporarily add local portable GCC to PATH for this script only
set "PATH=%~dp0w64devkit\bin;%PATH%"

echo Compiling C System Tray & WASAPI Loopback Host...
gcc host_tray.c -o song_finder.exe -O2 -mwindows -lshell32 -lole32 -luser32 -lgdi32

if %ERRORLEVEL% EQU 0 (
    echo Build succeeded! Launching song_finder.exe...
    start "" song_finder.exe
) else (
    echo Build failed. Check that w64devkit\bin\gcc.exe and miniaudio.h exist.
    pause
)
