@echo off
setlocal
REM Add local portable w64devkit GCC and windres to PATH for this build session
set "PATH=%~dp0w64devkit\bin;%PATH%"

echo [1/5] Building recognize_ui.py in fast --onedir mode...
pyinstaller --onedir --noconsole --clean --collect-all shazamio --collect-all ytmusicapi --name recognize_ui recognize_ui.py
if %ERRORLEVEL% NEQ 0 (
    echo PyInstaller step failed. Make sure you ran: pip install pyinstaller
    pause
    exit /b 1
)

echo [2/5] Compressing --onedir worker folder into worker_pkg.zip...
tar.exe -a -cf worker_pkg.zip -C dist recognize_ui
if %ERRORLEVEL% NEQ 0 (
    echo Failed to create worker_pkg.zip using tar.exe.
    pause
    exit /b 1
)

echo [3/5] Packing script (101) and worker archive (102) into Windows RCDATA resources...
(
    echo 101 RCDATA "recognize_ui.py"
    echo 102 RCDATA "worker_pkg.zip"
) > resources.rc

windres resources.rc -O coff -o resources.o
if %ERRORLEVEL% NEQ 0 (
    echo windres step failed. Check that w64devkit\bin\windres.exe exists.
    pause
    exit /b 1
)

echo [4/5] Compiling single standalone song_finder.exe...
gcc host_tray.c resources.o -o song_finder.exe -O2 -s -mwindows -lshell32 -lole32 -luser32 -lgdi32
if %ERRORLEVEL% NEQ 0 (
    echo GCC compilation failed. Make sure song_finder.exe is not currently running in the tray.
    pause
    exit /b 1
)

echo [5/5] Cleaning up temporary build artifacts...
rmdir /s /q build dist 2>nul
del /q recognize_ui.spec worker_pkg.zip resources.rc resources.o 2>nul

echo.
echo Build succeeded! Launching song_finder.exe...
start "" song_finder.exe
