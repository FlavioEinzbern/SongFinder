#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define WM_TRAYICON         (WM_USER + 1)
#define WM_APP_LOADER_STATE (WM_USER + 2) /* wParam: 0=Hide, 1=Listening, 2=Searching, 3=Loading, 4=Deep Listen */

#define ID_TRAY_IDENTIFY    1001
#define ID_TRAY_EXIT        1002

#define IDR_PYTHON_SCRIPT   101
#define IDR_WORKER_ARCHIVE  102

#define HOTKEY_ID           1
#define LOADER_TIMER_ID     2

#define RECORD_SECONDS      4
#define LOADER_W            142
#define LOADER_H            38

static NOTIFYICONDATAA g_nid = {0};
static HWND g_hMainWnd   = NULL;
static HWND g_hLoaderWnd = NULL;
static volatile bool g_isBusy = false;
static bool g_useNativePython = false;
static int g_loaderPhase = 0; /* 0=Hidden, 1=Listening, 2=Searching, 3=Loading, 4=Deep Listen */
static int g_animTick    = 0;

/* Paths inside %TEMP%\SongFinder */
static char g_selfExePath[MAX_PATH]   = {0};
static char g_workDir[MAX_PATH]       = {0};
static char g_wavPath[MAX_PATH]       = {0};
static char g_scriptPath[MAX_PATH]    = {0};
static char g_archivePath[MAX_PATH]   = {0};
static char g_workerExePath[MAX_PATH] = {0};
static char g_modeCfgPath[MAX_PATH]   = {0};

static const int kBarWave[8] = { 6, 12, 18, 22, 16, 10, 14, 8 };

/* Helper: Checks if a file exists on disk */
static bool file_exists(const char* path) {
    DWORD attr = GetFileAttributesA(path);
    return (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY));
}

/* Helper: Returns the byte size of an embedded RCDATA resource */
static DWORD get_resource_size(int resId) {
    HRSRC hRes = FindResourceA(NULL, MAKEINTRESOURCEA(resId), (LPCSTR)RT_RCDATA);
    if (!hRes) return 0;
    return SizeofResource(NULL, hRes);
}

/* Helper: Extracts an embedded RCDATA resource to a file on disk */
static bool extract_resource_to_file(int resId, const char* destPath) {
    HRSRC hRes = FindResourceA(NULL, MAKEINTRESOURCEA(resId), (LPCSTR)RT_RCDATA);
    if (!hRes) return false;

    DWORD resSize = SizeofResource(NULL, hRes);
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData || resSize == 0) return false;

    const void* pBytes = LockResource(hData);
    if (!pBytes) return false;

    HANDLE hFile = CreateFileA(
        destPath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL
    );
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, pBytes, resSize, &written, NULL);
    CloseHandle(hFile);

    return (ok && written == resSize);
}

/* Helper: Runs a command silently and returns true if it exits with code 0 */
static bool run_silent_command(const char* cmdLine) {
    char mutableCmd[1024];
    strncpy(mutableCmd, cmdLine, sizeof(mutableCmd) - 1);
    mutableCmd[sizeof(mutableCmd) - 1] = '\0';

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessA(NULL, mutableCmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return false;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (exitCode == 0);
}

/* Checks if the PC already has Python + all required packages including speech_recognition */
static bool check_native_python_deps(void) {
    return run_silent_command("python -c \"import shazamio, ytmusicapi, PIL, tkinter, speech_recognition\"");
}

/* Extracts and unpacks the --onedir worker archive into %TEMP%\SongFinder\recognize_ui\ */
static bool extract_onedir_worker(void) {
    if (!extract_resource_to_file(IDR_WORKER_ARCHIVE, g_archivePath)) {
        return false;
    }

    char tarCmd[MAX_PATH * 2 + 64];
    snprintf(tarCmd, sizeof(tarCmd), "tar.exe -xf \"%s\" -C \"%s\"", g_archivePath, g_workDir);
    bool ok = run_silent_command(tarCmd);

    DeleteFileA(g_archivePath);
    return (ok && file_exists(g_workerExePath));
}

/* Saves the active mode (1=Native Python, 2=Standalone Worker) + build signature */
static void save_mode_cache(int mode, DWORD scriptSize, DWORD archiveSize) {
    char buf[128];
    int len = snprintf(buf, sizeof(buf), "%d:%lu:%lu", mode, (unsigned long)scriptSize, (unsigned long)archiveSize);
    HANDLE hFile = CreateFileA(g_modeCfgPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, buf, (DWORD)len, &written, NULL);
        CloseHandle(hFile);
    }
}

/* Checks if %TEMP%\SongFinder\mode.cfg matches current build and files exist */
static bool load_valid_mode_cache(DWORD scriptSize, DWORD archiveSize) {
    HANDLE hFile = CreateFileA(g_modeCfgPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    char buf[128] = {0};
    DWORD bytesRead = 0;
    ReadFile(hFile, buf, sizeof(buf) - 1, &bytesRead, NULL);
    CloseHandle(hFile);

    int mode = 0;
    unsigned long cachedScript = 0, cachedArchive = 0;
    if (sscanf(buf, "%d:%lu:%lu", &mode, &cachedScript, &cachedArchive) != 3) {
        return false;
    }

    if (cachedScript != scriptSize || cachedArchive != archiveSize) {
        return false;
    }

    if (mode == 1 && file_exists(g_scriptPath)) {
        g_useNativePython = true;
        return true;
    }
    if (mode == 2 && file_exists(g_workerExePath)) {
        g_useNativePython = false;
        return true;
    }
    return false;
}

/* Background startup thread: checks cache or shows "Loading..." while setting up */
DWORD WINAPI startup_init_thread(LPVOID lpParam) {
    (void)lpParam;

    DWORD scriptSize  = get_resource_size(IDR_PYTHON_SCRIPT);
    DWORD archiveSize = get_resource_size(IDR_WORKER_ARCHIVE);

    /* Always keep recognize_ui.py updated in %TEMP% if size changed */
    if (load_valid_mode_cache(scriptSize, archiveSize)) {
        g_isBusy = false;
        return 0;
    }

    PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 3, 0);
    extract_resource_to_file(IDR_PYTHON_SCRIPT, g_scriptPath);

    if (check_native_python_deps()) {
        g_useNativePython = true;
        save_mode_cache(1, scriptSize, archiveSize);
        Sleep(300);
    } else {
        if (extract_onedir_worker()) {
            g_useNativePython = false;
            save_mode_cache(2, scriptSize, archiveSize);
        } else {
            MessageBoxA(NULL, "Failed to unpack standalone worker to %TEMP%.", "Song Finder Error", MB_ICONERROR);
        }
    }

    PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 0, 0);
    g_isBusy = false;
    return 0;
}

/* WASAPI callback: writes converted 16kHz Mono PCM frames into the WAV file */
void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    ma_encoder* pEncoder = (ma_encoder*)pDevice->pUserData;
    if (pEncoder != NULL && pInput != NULL) {
        ma_encoder_write_pcm_frames(pEncoder, pInput, frameCount, NULL);
    }
    (void)pOutput;
}

/* Records N seconds of Windows desktop audio directly in 16kHz Mono 16-bit PCM */
bool record_system_audio(const char* filename, int seconds) {
    ma_result result;
    ma_encoder_config encoderConfig;
    ma_encoder encoder;
    ma_device_config deviceConfig;
    ma_device device;

    encoderConfig = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, 16000);
    result = ma_encoder_init_file(filename, &encoderConfig, &encoder);
    if (result != MA_SUCCESS) {
        return false;
    }

    deviceConfig = ma_device_config_init(ma_device_type_loopback);
    deviceConfig.capture.pDeviceID = NULL;
    deviceConfig.capture.format    = encoder.config.format;
    deviceConfig.capture.channels  = encoder.config.channels;
    deviceConfig.sampleRate        = encoder.config.sampleRate;
    deviceConfig.dataCallback      = data_callback;
    deviceConfig.pUserData         = &encoder;

    result = ma_device_init(NULL, &deviceConfig, &device);
    if (result != MA_SUCCESS) {
        ma_encoder_uninit(&encoder);
        return false;
    }

    result = ma_device_start(&device);
    if (result != MA_SUCCESS) {
        ma_device_uninit(&device);
        ma_encoder_uninit(&encoder);
        return false;
    }

    Sleep(seconds * 1000);

    ma_device_uninit(&device);
    ma_encoder_uninit(&encoder);
    return true;
}

/* Spawns Python worker and passes both temp.wav and song_finder.exe's path (for Backup 1 CLI capture) */
HANDLE launch_python_worker(const char* wavPath) {
    char cmdLine[MAX_PATH * 3 + 32];
    if (g_useNativePython) {
        snprintf(cmdLine, sizeof(cmdLine), "python \"%s\" \"%s\" \"%s\"", g_scriptPath, wavPath, g_selfExePath);
    } else {
        snprintf(cmdLine, sizeof(cmdLine), "\"%s\" \"%s\" \"%s\"", g_workerExePath, wavPath, g_selfExePath);
    }

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};

    if (CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }
    return NULL;
}

/* Background thread for recording and monitoring the full lookup/fallback chain */
DWORD WINAPI capture_thread(LPVOID lpParam) {
    (void)lpParam;

    /* Phase 1: Show "Listening..." overlay and capture initial 4s of audio */
    PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 1, 0);

    if (record_system_audio(g_wavPath, RECORD_SECONDS)) {
        /* Phase 2: Switch overlay to "Searching..." and launch worker */
        PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 2, 0);
        HANDLE hProc = launch_python_worker(g_wavPath);

        /* Safety Fallback: If native Python was uninstalled after caching, unpack worker now */
        if (hProc == NULL && g_useNativePython) {
            PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 3, 0);
            if (extract_onedir_worker()) {
                g_useNativePython = false;
                save_mode_cache(2, get_resource_size(IDR_PYTHON_SCRIPT), get_resource_size(IDR_WORKER_ARCHIVE));
                PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 2, 0);
                hProc = launch_python_worker(g_wavPath);
            }
        }

        if (hProc != NULL) {
            while (g_loaderPhase != 0) {
                if (WaitForSingleObject(hProc, 50) == WAIT_OBJECT_0) {
                    break;
                }
            }
            CloseHandle(hProc);
        }
    }

    PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 0, 0);
    g_isBusy = false;
    return 0;
}

void trigger_identification(void) {
    if (g_isBusy) return;
    g_isBusy = true;
    HANDLE hThread = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    if (hThread) CloseHandle(hThread);
}

/* Window Procedure for the bottom-right animated equalizer overlay */
LRESULT CALLBACK LoaderWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_TIMER:
            if (wParam == LOADER_TIMER_ID) {
                g_animTick++;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, LOADER_W, LOADER_H);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            HBRUSH bgBrush = CreateSolidBrush(RGB(24, 24, 27));
            HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(63, 63, 70));
            HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, bgBrush);
            HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
            Rectangle(memDC, 0, 0, LOADER_W, LOADER_H);

            /* Bar colors: 1=Red (Listening), 2=Blue (Searching), 3=Amber (Loading), 4=Purple (Deep Listen) */
            COLORREF barColor = RGB(59, 130, 246);
            if (g_loaderPhase == 1) barColor = RGB(239, 68, 68);
            else if (g_loaderPhase == 3) barColor = RGB(245, 158, 11);
            else if (g_loaderPhase == 4) barColor = RGB(168, 85, 247);

            HBRUSH barBrush = CreateSolidBrush(barColor);
            for (int i = 0; i < 4; i++) {
                int h = kBarWave[(g_animTick + i * 2) % 8];
                int x = 14 + i * 6;
                int y = (LOADER_H - h) / 2;
                RECT barRect = { x, y, x + 4, y + h };
                FillRect(memDC, &barRect, barBrush);
            }
            DeleteObject(barBrush);

            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(228, 228, 231));
            HFONT hFont = CreateFontA(
                -13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI"
            );
            HFONT oldFont = (HFONT)SelectObject(memDC, hFont);

            RECT textRect = { 44, 0, LOADER_W - 6, LOADER_H };
            const char* label = "Searching...";
            if (g_loaderPhase == 1) label = "Listening...";
            else if (g_loaderPhase == 3) label = "Loading...";
            else if (g_loaderPhase == 4) label = "Deep Listen...";

            DrawTextA(memDC, label, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            BitBlt(hdc, 0, 0, LOADER_W, LOADER_H, memDC, 0, 0, SRCCOPY);

            SelectObject(memDC, oldFont);
            DeleteObject(hFont);
            SelectObject(memDC, oldBrush);
            SelectObject(memDC, oldPen);
            DeleteObject(bgBrush);
            DeleteObject(borderPen);
            SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            DeleteDC(memDC);

            EndPaint(hwnd, &ps);
            return 0;
        }
    }
    return DefWindowProcA(hwnd, uMsg, wParam, lParam);
}

/* Main hidden window procedure for Tray, Hotkey, and Worker IPC messages */
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_APP_LOADER_STATE: {
            int newState = (int)wParam;
            g_loaderPhase = newState;

            if (newState == 0) {
                KillTimer(g_hLoaderWnd, LOADER_TIMER_ID);
                ShowWindow(g_hLoaderWnd, SW_HIDE);
            } else {
                RECT workArea;
                SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0);
                int x = workArea.right - LOADER_W - 16;
                int y = workArea.bottom - LOADER_H - 16;

                SetWindowPos(g_hLoaderWnd, HWND_TOPMOST, x, y, LOADER_W, LOADER_H, SWP_NOACTIVATE);
                if (!IsWindowVisible(g_hLoaderWnd)) {
                    ShowWindow(g_hLoaderWnd, SW_SHOWNOACTIVATE);
                    SetTimer(g_hLoaderWnd, LOADER_TIMER_ID, 40, NULL);
                }
                InvalidateRect(g_hLoaderWnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_HOTKEY:
            if (wParam == HOTKEY_ID) {
                trigger_identification();
            }
            break;

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP) {
                trigger_identification();
            } else if (lParam == WM_RBUTTONUP) {
                POINT pt;
                GetCursorPos(&pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuA(hMenu, MF_STRING, ID_TRAY_IDENTIFY, "Identify Song Now (Ctrl+Alt+S)");
                AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hMenu, MF_STRING, ID_TRAY_EXIT, "Exit");

                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_IDENTIFY) {
                trigger_identification();
            } else if (LOWORD(wParam) == ID_TRAY_EXIT) {
                PostQuitMessage(0);
            }
            break;

        case WM_DESTROY:
            Shell_NotifyIconA(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            break;

        default:
            return DefWindowProcA(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)nCmdShow;

    /* Backup 1 CLI Audio Engine Mode: song_finder.exe --capture <seconds> "<wav_path>" */
    if (lpCmdLine && strncmp(lpCmdLine, "--capture", 9) == 0) {
        int sec = 8;
        char outWav[MAX_PATH] = {0};
        const char* p = lpCmdLine + 9;
        while (*p == ' ') p++;
        if (*p >= '0' && *p <= '9') {
            sec = atoi(p);
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
        }
        if (*p == '"') {
            p++;
            strncpy(outWav, p, MAX_PATH - 1);
            char* q = strchr(outWav, '"');
            if (q) *q = '\0';
        } else if (*p) {
            strncpy(outWav, p, MAX_PATH - 1);
        }
        if (outWav[0] != '\0' && sec > 0 && sec <= 30) {
            return record_system_audio(outWav, sec) ? 0 : 1;
        }
        return 1;
    }

    GetModuleFileNameA(NULL, g_selfExePath, MAX_PATH);

    /* Initialize %TEMP%\SongFinder file paths */
    char tempBase[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, tempBase);
    if (len == 0 || len > MAX_PATH - 48) {
        return 1;
    }
    snprintf(g_workDir, sizeof(g_workDir), "%sSongFinder", tempBase);
    CreateDirectoryA(g_workDir, NULL);

    snprintf(g_wavPath, sizeof(g_wavPath), "%s\\temp.wav", g_workDir);
    snprintf(g_scriptPath, sizeof(g_scriptPath), "%s\\recognize_ui.py", g_workDir);
    snprintf(g_archivePath, sizeof(g_archivePath), "%s\\worker_pkg.zip", g_workDir);
    snprintf(g_workerExePath, sizeof(g_workerExePath), "%s\\recognize_ui\\recognize_ui.exe", g_workDir);
    snprintf(g_modeCfgPath, sizeof(g_modeCfgPath), "%s\\mode.cfg", g_workDir);

    const char HOST_CLASS[] = "SongFinderTrayClass";
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = HOST_CLASS;
    RegisterClassA(&wc);

    const char LOADER_CLASS[] = "SongFinderLoaderClass";
    WNDCLASSA lwc = {0};
    lwc.lpfnWndProc   = LoaderWndProc;
    lwc.hInstance     = hInstance;
    lwc.lpszClassName = LOADER_CLASS;
    lwc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&lwc);

    g_hMainWnd = CreateWindowExA(0, HOST_CLASS, "SongFinderHost", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    g_hLoaderWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        LOADER_CLASS, "", WS_POPUP,
        0, 0, LOADER_W, LOADER_H,
        NULL, NULL, hInstance, NULL
    );

    RegisterHotKey(g_hMainWnd, HOTKEY_ID, MOD_CONTROL | MOD_ALT, 'S');

    g_nid.cbSize           = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd             = g_hMainWnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIcon(NULL, IDI_APPLICATION);
    strncpy(g_nid.szTip, "Song Finder (Click or press Ctrl+Alt+S)", sizeof(g_nid.szTip) - 1);
    Shell_NotifyIconA(NIM_ADD, &g_nid);

    g_isBusy = true;
    HANDLE hInitThread = CreateThread(NULL, 0, startup_init_thread, NULL, 0, NULL);
    if (hInitThread) CloseHandle(hInitThread);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterHotKey(g_hMainWnd, HOTKEY_ID);
    return 0;
}
