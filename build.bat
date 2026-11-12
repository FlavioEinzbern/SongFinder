@echo off
setlocal
REM Add local portable w64devkit GCC and windres to PATH for this build session
set "PATH=%~dp0w64devkit\bin;%PATH%"

echo [1/4] Freezing recognize_ui.py into standalone worker executable...
pyinstaller --onefile --noconsole --clean --collect-all shazamio --collect-all ytmusicapi --name recognize_ui recognize_ui.py
if %ERRORLEVEL% NEQ 0 (
    echo PyInstaller step failed. Make sure you ran: pip install pyinstaller
    pause
    exit /b 1
)

echo [2/4] Packing worker executable into Windows RCDATA resource...
echo 101 RCDATA "dist/recognize_ui.exe" > resources.rc
windres resources.rc -O coff -o resources.o
if %ERRORLEVEL% NEQ 0 (
    echo windres step failed. Check that w64devkit\bin\windres.exe exists.
    pause
    exit /b 1
)

echo [3/4] Compiling single standalone song_finder.exe...
gcc host_tray.c resources.o -o song_finder.exe -O2 -s -mwindows -lshell32 -lole32 -luser32 -lgdi32
if %ERRORLEVEL% NEQ 0 (
    echo GCC compilation failed. Make sure song_finder.exe is not currently running in the tray.
    pause
    exit /b 1
)

echo [4/4] Cleaning up temporary build folders...
rmdir /s /q build dist 2>nul
del /q recognize_ui.spec resources.rc resources.o 2>nul

echo.
echo Build succeeded! song_finder.exe is now a 100%% standalone single-file executable.
start "" song_finder.exe
