Project Structure & Prerequisites
Create a project folder (for example, C:\SongFinder\) with these four files:

SongFinder/
├── w64devkit/         <-- Extracted portable compiler folder
│   └── bin/
│       └── gcc.exe
├── miniaudio.h        (Single-header C audio library)
├── host_tray.c        (Always-on C System Tray + WASAPI Loopback Recorder)
├── recognize_ui.py    (On-demand Python Shazam + YouTube Music + Popup UI)
└── build.bat          (One-click compiler script for Windows)

download the latest "w64devkit-x64-xxxx.7z.exe" from the following link, extract in the folder just like the tree above:
https://github.com/skeeto/w64devkit/releases

Open the command prompt (cmd) in the target folder and execute the following commands:
"curl -L -o miniaudio.h https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h"
"pip install shazamio ytmusicapi pillow"
"pip install pyinstaller"
"pip install SpeechRecognition"

Now run the "build.bat" and it should create a file "song_finder.exe"
