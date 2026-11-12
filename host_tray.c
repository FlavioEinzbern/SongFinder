#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdbool.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define WM_TRAYICON         (WM_USER + 1)
#define WM_APP_LOADER_STATE (WM_USER + 2) /* wParam: 0=Hide, 1=Listening, 2=Searching */

#define ID_TRAY_IDENTIFY  1001
#define ID_TRAY_EXIT      1002
#define IDR_PYTHON_WORKER 101
#define HOTKEY_ID         1
#define LOADER_TIMER_ID   2

#define RECORD_SECONDS    4
#define LOADER_W          136
#define LOADER_H          38

static NOTIFYICONDATAA g_nid = {0};
static HWND g_hMainWnd   = NULL;
static HWND g_hLoaderWnd = NULL;
static volatile bool g_isRecording = false;
static int g_loaderPhase = 0; /* 0 = Hidden, 1 = Listening, 2 = Searching */
static int g_animTick    = 0;

/* Paths inside %TEMP%\SongFinder so the single .exe leaves zero clutter */
static char g_workDir[MAX_PATH]       = {0};
static char g_wavPath[MAX_PATH]       = {0};
static char g_workerExePath[MAX_PATH] = {0};

static const int kBarWave[8] = { 6, 12, 18, 22, 16, 10, 14, 8 };

/* Initializes %TEMP%\SongFinder paths and extracts the embedded worker .exe if needed */
bool init_portable_environment(void) {
    char tempBase[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, tempBase);
    if (len == 0 || len > MAX_PATH - 32) {
        return false;
    }

    snprintf(g_workDir, sizeof(g_workDir), "%sSongFinder", tempBase);
    CreateDirectoryA(g_workDir, NULL);

    snprintf(g_wavPath, sizeof(g_wavPath), "%s\\temp.wav", g_workDir);
    snprintf(g_workerExePath, sizeof(g_workerExePath), "%s\\recognize_ui.exe", g_workDir);

    /* Locate the embedded Python worker inside our own .exe resources */
    HRSRC hRes = FindResourceA(NULL, MAKEINTRESOURCEA(IDR_PYTHON_WORKER), (LPCSTR)RT_RCDATA);
    if (!hRes) {
        return false;
    }

    DWORD resSize = SizeofResource(NULL, hRes);
    HGLOBAL hData = LoadResource(NULL, hRes);
    if (!hData || resSize == 0) {
        return false;
    }

    const void* pBytes = LockResource(hData);
    if (!pBytes) {
        return false;
    }

    /* Skip extraction if recognize_ui.exe already exists in %TEMP% with the exact same size */
    WIN32_FILE_ATTRIBUTE_DATA fileAttr;
    if (GetFileAttributesExA(g_workerExePath, GetFileExInfoStandard, &fileAttr)) {
        if (fileAttr.nFileSizeHigh == 0 && fileAttr.nFileSizeLow == resSize) {
            return true;
        }
    }

    /* Extract the embedded worker to %TEMP%\SongFinder\recognize_ui.exe */
    HANDLE hFile = CreateFileA(
        g_workerExePath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(hFile, pBytes, resSize, &written, NULL);
    CloseHandle(hFile);

    return (ok && written == resSize);
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

    /* 16,000 Hz, 1-Channel (Mono), 16-bit PCM — eliminates the need for ffmpeg */
    encoderConfig = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, 16000);
    result = ma_encoder_init_file(filename, &encoderConfig, &encoder);
    if (result != MA_SUCCESS) {
        return false;
    }

    /* miniaudio automatically downmixes and resamples WASAPI output to match encoderConfig */
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

/* Spawns the extracted standalone worker .exe */
HANDLE launch_python_worker(const char* wavPath) {
    char cmdLine[MAX_PATH * 2 + 16];
    snprintf(cmdLine, sizeof(cmdLine), "\"%s\" \"%s\"", g_workerExePath, wavPath);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};

    if (CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }
    return NULL;
}

/* Background thread for recording and monitoring the lookup phase */
DWORD WINAPI capture_thread(LPVOID lpParam) {
    (void)lpParam;

    /* Phase 1: Show "Listening..." overlay and capture 4s of audio */
    PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 1, 0);

    if (record_system_audio(g_wavPath, RECORD_SECONDS)) {
        /* Phase 2: Switch overlay to "Searching..." and run extracted worker */
        PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 2, 0);
        HANDLE hProc = launch_python_worker(g_wavPath);

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
    g_isRecording = false;
    return 0;
}

void trigger_identification(void) {
    if (g_isRecording) return;
    g_isRecording = true;
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

            COLORREF barColor = (g_loaderPhase == 1) ? RGB(239, 68, 68) : RGB(59, 130, 246);
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
            const char* label = (g_loaderPhase == 1) ? "Listening..." : "Searching...";
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
    (void)hPrevInstance; (void)lpCmdLine; (void)nCmdShow;

    if (!init_portable_environment()) {
        MessageBoxA(NULL, "Failed to extract embedded recognition worker to %TEMP%.", "Song Finder Error", MB_ICONERROR);
        return 1;
    }

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

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterHotKey(g_hMainWnd, HOTKEY_ID);
    return 0;
}
