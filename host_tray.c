#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdbool.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_IDENTIFY 1001
#define ID_TRAY_EXIT     1002
#define HOTKEY_ID        1
#define RECORD_SECONDS   4
#define WAV_FILENAME     "temp.wav"

static NOTIFYICONDATAA g_nid = {0};
static bool g_isRecording = false;

/* Callback fired by Windows WASAPI whenever system audio plays */
void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    ma_encoder* pEncoder = (ma_encoder*)pDevice->pUserData;
    if (pEncoder != NULL && pInput != NULL) {
        ma_encoder_write_pcm_frames(pEncoder, pInput, frameCount, NULL);
    }
    (void)pOutput;
}

/* Captures N seconds of Windows desktop audio into a WAV file */
bool record_system_audio(const char* filename, int seconds) {
    ma_result result;
    ma_encoder_config encoderConfig;
    ma_encoder encoder;
    ma_device_config deviceConfig;
    ma_device device;

    /* 44.1kHz, 16-bit stereo WAV */
    encoderConfig = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 2, 44100);
    result = ma_encoder_init_file(filename, &encoderConfig, &encoder);
    if (result != MA_SUCCESS) {
        return false;
    }

    /* Configure WASAPI Loopback (captures default playback device output) */
    deviceConfig = ma_device_config_init(ma_device_type_loopback);
    deviceConfig.capture.pDeviceID = NULL; /* Default Windows playback device */
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

    /* Record for the specified duration */
    Sleep(seconds * 1000);

    ma_device_uninit(&device);
    ma_encoder_uninit(&encoder);
    return true;
}

/* Spawns Python in the background without freezing the C tray host */
void launch_python_worker(const char* wavPath) {
    char cmdLine[512];
    snprintf(cmdLine, sizeof(cmdLine), "python recognize_ui.py \"%s\"", wavPath);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};

    /* CREATE_NO_WINDOW prevents a black cmd console from flashing */
    if (CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    } else {
        MessageBoxA(NULL, "Failed to launch Python worker. Make sure python is in your PATH.", "Error", MB_ICONERROR);
    }
}

/* Shows a subtle Windows tray balloon notification */
void show_tray_balloon(const char* title, const char* msg) {
    g_nid.uFlags = NIF_INFO;
    strncpy(g_nid.szInfoTitle, title, sizeof(g_nid.szInfoTitle) - 1);
    strncpy(g_nid.szInfo, msg, sizeof(g_nid.szInfo) - 1);
    g_nid.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconA(NIM_MODIFY, &g_nid);
}

/* Worker thread so the tray UI remains responsive during the 4-second capture */
DWORD WINAPI capture_thread(LPVOID lpParam) {
    (void)lpParam;
    show_tray_balloon("Listening...", "Capturing 4 seconds of system audio...");

    if (record_system_audio(WAV_FILENAME, RECORD_SECONDS)) {
        launch_python_worker(WAV_FILENAME);
    } else {
        show_tray_balloon("Audio Error", "Could not initialize WASAPI loopback capture.");
    }

    g_isRecording = false;
    return 0;
}

void trigger_identification(void) {
    if (g_isRecording) return;
    g_isRecording = true;
    HANDLE hThread = CreateThread(NULL, 0, capture_thread, NULL, 0, NULL);
    if (hThread) CloseHandle(hThread);
}

/* Win32 Window Procedure for Tray Icon & Global Hotkey events */
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_HOTKEY:
            if (wParam == HOTKEY_ID) {
                trigger_identification();
            }
            break;

        case WM_TRAYICON:
            if (lParam == WM_LBUTTONUP) {
                /* Left-clicking the tray icon starts recognition immediately */
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

    const char CLASS_NAME[] = "SongFinderTrayClass";
    WNDCLASSA wc = {0};
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassA(&wc);

    /* Message-only hidden window to receive tray and hotkey events */
    HWND hwnd = CreateWindowExA(0, CLASS_NAME, "SongFinderHost", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    /* Register Ctrl + Alt + S global shortcut */
    RegisterHotKey(hwnd, HOTKEY_ID, MOD_CONTROL | MOD_ALT, 'S');

    /* Create System Tray Icon */
    g_nid.cbSize           = sizeof(NOTIFYICONDATAA);
    g_nid.hWnd             = hwnd;
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

    UnregisterHotKey(hwnd, HOTKEY_ID);
    return 0;
}