#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
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
#define ID_TRAY_HISTORY     1003

#define IDR_PYTHON_SCRIPT   101
#define IDR_WORKER_ARCHIVE  102

#define HOTKEY_ID           1
#define LOADER_TIMER_ID     2

#define RECORD_SHORT_SEC    5
#define RECORD_DEEP_SEC     10

#define LOADER_W            146
#define LOADER_H_COMPACT    38
#define LOADER_H_WITH_BTN   64

typedef struct {
    CRITICAL_SECTION cs;
    ma_encoder encShort;
    ma_encoder encDeep;
    bool shortOpen;
    bool deepOpen;
} DualCaptureContext;

static NOTIFYICONDATAA g_nid = {0};
static HWND g_hMainWnd   = NULL;
static HWND g_hLoaderWnd = NULL;
static volatile bool g_isBusy          = false;
static volatile bool g_cancelRequested = false;
static bool g_useNativePython          = false;
static int g_loaderPhase = 0; /* 0=Hidden, 1=Listening, 2=Searching, 3=Loading, 4=Deep Listen */
static int g_animTick    = 0;
static bool g_cancelHover = false;

/* Paths inside %TEMP%\SongFinder */
static char g_workDir[MAX_PATH]       = {0};
static char g_wavPath[MAX_PATH]       = {0};
static char g_deepWritePath[MAX_PATH] = {0};
static char g_deepReadyPath[MAX_PATH] = {0};
static char g_scriptPath[MAX_PATH]    = {0};
static char g_archivePath[MAX_PATH]   = {0};
static char g_workerExePath[MAX_PATH] = {0};
static char g_modeCfgPath[MAX_PATH]   = {0};

static const int kBarWave[8] = { 6, 12, 18, 22, 16, 10, 14, 8 };
static const RECT kCancelBtnRect = { 12, 36, LOADER_W - 12, 57 };

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

/* Checks if the PC already has Python + all required packages */
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

/* WASAPI callback: writes 16kHz Mono PCM frames to both 5s and 10s encoders simultaneously */
void dual_data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    DualCaptureContext* ctx = (DualCaptureContext*)pDevice->pUserData;
    if (ctx != NULL && pInput != NULL) {
        EnterCriticalSection(&ctx->cs);
        if (ctx->shortOpen) {
            ma_encoder_write_pcm_frames(&ctx->encShort, pInput, frameCount, NULL);
        }
        if (ctx->deepOpen) {
            ma_encoder_write_pcm_frames(&ctx->encDeep, pInput, frameCount, NULL);
        }
        LeaveCriticalSection(&ctx->cs);
    }
    (void)pOutput;
}

/* Spawns Python worker and passes both the 5s WAV path and the 10s deep WAV target path */
HANDLE launch_python_worker(const char* shortWav, const char* deepReadyWav) {
    char cmdLine[MAX_PATH * 3 + 32];
    if (g_useNativePython) {
        snprintf(cmdLine, sizeof(cmdLine), "python \"%s\" \"%s\" \"%s\"", g_scriptPath, shortWav, deepReadyWav);
    } else {
        snprintf(cmdLine, sizeof(cmdLine), "\"%s\" \"%s\" \"%s\"", g_workerExePath, shortWav, deepReadyWav);
    }

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};

    if (CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }
    return NULL;
}

/* Launches the Found Songs History window (--history) */
void launch_history_viewer(void) {
    if (g_loaderPhase == 3) return; /* Wait if first-run extraction is still active */

    char cmdLine[MAX_PATH * 2 + 32];
    if (g_useNativePython) {
        snprintf(cmdLine, sizeof(cmdLine), "python \"%s\" --history", g_scriptPath);
    } else {
        snprintf(cmdLine, sizeof(cmdLine), "\"%s\" --history", g_workerExePath);
    }

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};

    if (CreateProcessA(NULL, cmdLine, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* Pipelined capture thread: records 0-5s and 0-10s simultaneously while Python searches at t=5s */
DWORD WINAPI capture_thread(LPVOID lpParam) {
    (void)lpParam;
    g_cancelRequested = false;

    DeleteFileA(g_wavPath);
    DeleteFileA(g_deepWritePath);
    DeleteFileA(g_deepReadyPath);

    DualCaptureContext ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    InitializeCriticalSection(&ctx.cs);

    ma_encoder_config encCfg = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, 1, 16000);
    if (ma_encoder_init_file(g_wavPath, &encCfg, &ctx.encShort) != MA_SUCCESS) {
        DeleteCriticalSection(&ctx.cs);
        g_isBusy = false;
        return 0;
    }
    ctx.shortOpen = true;

    if (ma_encoder_init_file(g_deepWritePath, &encCfg, &ctx.encDeep) == MA_SUCCESS) {
        ctx.deepOpen = true;
    }

    ma_device_config devCfg = ma_device_config_init(ma_device_type_loopback);
    devCfg.capture.pDeviceID = NULL;
    devCfg.capture.format    = ma_format_s16;
    devCfg.capture.channels  = 1;
    devCfg.sampleRate        = 16000;
    devCfg.dataCallback      = dual_data_callback;
    devCfg.pUserData         = &ctx;

    ma_device device;
    if (ma_device_init(NULL, &devCfg, &device) != MA_SUCCESS || ma_device_start(&device) != MA_SUCCESS) {
        if (ctx.shortOpen) ma_encoder_uninit(&ctx.encShort);
        if (ctx.deepOpen)  ma_encoder_uninit(&ctx.encDeep);
        DeleteCriticalSection(&ctx.cs);
        g_isBusy = false;
        return 0;
    }

    /* Phase 1 (0s -> 5s): Show "Listening..." while recording both encoders */
    PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 1, 0);

    int elapsedMs = 0;
    const int shortTargetMs = RECORD_SHORT_SEC * 1000;
    while (elapsedMs < shortTargetMs && !g_cancelRequested) {
        Sleep(50);
        elapsedMs += 50;
    }

    /* Finalize 5-second clip so Python can read it immediately */
    EnterCriticalSection(&ctx.cs);
    if (ctx.shortOpen) {
        ctx.shortOpen = false;
        ma_encoder_uninit(&ctx.encShort);
    }
    LeaveCriticalSection(&ctx.cs);

    HANDLE hProc = NULL;

    if (!g_cancelRequested) {
        /* Phase 2 (5s -> 10s): Launch Python on 5s clip while continuing 10s recording in background */
        PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 2, 0);
        hProc = launch_python_worker(g_wavPath, g_deepReadyPath);

        /* Safety Fallback if native Python was uninstalled */
        if (hProc == NULL && g_useNativePython) {
            PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 3, 0);
            if (extract_onedir_worker()) {
                g_useNativePython = false;
                save_mode_cache(2, get_resource_size(IDR_PYTHON_SCRIPT), get_resource_size(IDR_WORKER_ARCHIVE));
                PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 2, 0);
                hProc = launch_python_worker(g_wavPath, g_deepReadyPath);
            }
        }

        const int deepTargetMs = RECORD_DEEP_SEC * 1000;
        while (elapsedMs < deepTargetMs && !g_cancelRequested && g_loaderPhase != 0) {
            if (hProc != NULL && WaitForSingleObject(hProc, 50) == WAIT_OBJECT_0) {
                break;
            }
            Sleep(50);
            elapsedMs += 50;
        }
    }

    /* Stop WASAPI capture and finalize the 10-second deep WAV file */
    ma_device_uninit(&device);

    EnterCriticalSection(&ctx.cs);
    if (ctx.deepOpen) {
        ctx.deepOpen = false;
        ma_encoder_uninit(&ctx.encDeep);
    }
    LeaveCriticalSection(&ctx.cs);
    DeleteCriticalSection(&ctx.cs);

    if (!g_cancelRequested && g_loaderPhase != 0 && file_exists(g_deepWritePath)) {
        /* Atomic rename signals to Python that the 10s file is complete and ready */
        MoveFileExA(g_deepWritePath, g_deepReadyPath, MOVEFILE_REPLACE_EXISTING);
    } else {
        DeleteFileA(g_deepWritePath);
    }

    /* Wait for Python worker to finish or terminate it if Cancel was clicked */
    if (hProc != NULL) {
        while (!g_cancelRequested && g_loaderPhase != 0) {
            if (WaitForSingleObject(hProc, 50) == WAIT_OBJECT_0) {
                break;
            }
        }
        if (g_cancelRequested) {
            TerminateProcess(hProc, 1);
            WaitForSingleObject(hProc, 500);
        }
        CloseHandle(hProc);
    }

    if (g_cancelRequested) {
        DeleteFileA(g_wavPath);
        DeleteFileA(g_deepWritePath);
        DeleteFileA(g_deepReadyPath);
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

/* Window Procedure for the bottom-right animated equalizer + Cancel button overlay */
LRESULT CALLBACK LoaderWndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_TIMER:
            if (wParam == LOADER_TIMER_ID) {
                g_animTick++;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_MOUSEMOVE: {
            if (g_loaderPhase == 3) return 0;
            POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            bool hover = PtInRect(&kCancelBtnRect, pt);
            if (hover != g_cancelHover) {
                g_cancelHover = hover;
                InvalidateRect(hwnd, NULL, FALSE);
                if (hover) {
                    TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                    TrackMouseEvent(&tme);
                }
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_cancelHover) {
                g_cancelHover = false;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_LBUTTONUP: {
            if (g_loaderPhase != 3) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                if (PtInRect(&kCancelBtnRect, pt)) {
                    g_cancelRequested = true;
                    g_cancelHover = false;
                    PostMessageA(g_hMainWnd, WM_APP_LOADER_STATE, 0, 0);
                }
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);

            int curH = (g_loaderPhase == 3) ? LOADER_H_COMPACT : LOADER_H_WITH_BTN;

            HDC memDC = CreateCompatibleDC(hdc);
            HBITMAP memBmp = CreateCompatibleBitmap(hdc, LOADER_W, curH);
            HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

            HBRUSH bgBrush = CreateSolidBrush(RGB(24, 24, 27));
            HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(63, 63, 70));
            HBRUSH oldBrush = (HBRUSH)SelectObject(memDC, bgBrush);
            HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
            Rectangle(memDC, 0, 0, LOADER_W, curH);

            /* Top Row: 4 Equalizer Bars (1=Red, 2=Blue, 3=Amber, 4=Purple) */
            COLORREF barColor = RGB(59, 130, 246);
            if (g_loaderPhase == 1) barColor = RGB(239, 68, 68);
            else if (g_loaderPhase == 3) barColor = RGB(245, 158, 11);
            else if (g_loaderPhase == 4) barColor = RGB(168, 85, 247);

            HBRUSH barBrush = CreateSolidBrush(barColor);
            for (int i = 0; i < 4; i++) {
                int h = kBarWave[(g_animTick + i * 2) % 8];
                int x = 14 + i * 6;
                int y = (LOADER_H_COMPACT - h) / 2;
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

            RECT textRect = { 44, 0, LOADER_W - 6, LOADER_H_COMPACT };
            const char* label = "Searching...";
            if (g_loaderPhase == 1) label = "Listening...";
            else if (g_loaderPhase == 3) label = "Loading...";
            else if (g_loaderPhase == 4) label = "Deep Listen...";

            DrawTextA(memDC, label, -1, &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            /* Bottom Row: Clickable "Cancel" Button during active scans */
            if (g_loaderPhase != 3) {
                COLORREF btnBg = g_cancelHover ? RGB(220, 38, 38) : RGB(39, 39, 42);
                COLORREF btnBorder = g_cancelHover ? RGB(239, 68, 68) : RGB(82, 82, 91);
                HBRUSH btnBrush = CreateSolidBrush(btnBg);
                HPEN btnPen = CreatePen(PS_SOLID, 1, btnBorder);

                SelectObject(memDC, btnBrush);
                SelectObject(memDC, btnPen);
                RoundRect(memDC, kCancelBtnRect.left, kCancelBtnRect.top, kCancelBtnRect.right, kCancelBtnRect.bottom, 6, 6);

                HFONT hBtnFont = CreateFontA(
                    -11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, "Segoe UI"
                );
                SelectObject(memDC, hBtnFont);
                SetTextColor(memDC, g_cancelHover ? RGB(255, 255, 255) : RGB(212, 212, 216));

                RECT cancelTextRect = kCancelBtnRect;
                DrawTextA(memDC, "Cancel", -1, &cancelTextRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

                SelectObject(memDC, hFont);
                DeleteObject(hBtnFont);
                DeleteObject(btnBrush);
                DeleteObject(btnPen);
            }

            BitBlt(hdc, 0, 0, LOADER_W, curH, memDC, 0, 0, SRCCOPY);

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
                g_cancelHover = false;
                KillTimer(g_hLoaderWnd, LOADER_TIMER_ID);
                ShowWindow(g_hLoaderWnd, SW_HIDE);
            } else {
                int curH = (newState == 3) ? LOADER_H_COMPACT : LOADER_H_WITH_BTN;
                RECT workArea;
                SystemParametersInfoA(SPI_GETWORKAREA, 0, &workArea, 0);
                int x = workArea.right - LOADER_W - 16;
                int y = workArea.bottom - curH - 16;

                SetWindowPos(g_hLoaderWnd, HWND_TOPMOST, x, y, LOADER_W, curH, SWP_NOACTIVATE);
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
                AppendMenuA(hMenu, MF_STRING, ID_TRAY_HISTORY,  "Found Songs History");
                AppendMenuA(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenuA(hMenu, MF_STRING, ID_TRAY_EXIT,     "Exit");

                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            break;

        case WM_COMMAND:
            if (LOWORD(wParam) == ID_TRAY_IDENTIFY) {
                trigger_identification();
            } else if (LOWORD(wParam) == ID_TRAY_HISTORY) {
                launch_history_viewer();
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

    char tempBase[MAX_PATH];
    DWORD len = GetTempPathA(MAX_PATH, tempBase);
    if (len == 0 || len > MAX_PATH - 48) {
        return 1;
    }
    snprintf(g_workDir, sizeof(g_workDir), "%sSongFinder", tempBase);
    CreateDirectoryA(g_workDir, NULL);

    snprintf(g_wavPath, sizeof(g_wavPath), "%s\\temp.wav", g_workDir);
    snprintf(g_deepWritePath, sizeof(g_deepWritePath), "%s\\deep_writing.wav", g_workDir);
    snprintf(g_deepReadyPath, sizeof(g_deepReadyPath), "%s\\deep_temp.wav", g_workDir);
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
    lwc.hCursor       = LoadCursor(NULL, IDC_HAND);
    RegisterClassA(&lwc);

    g_hMainWnd = CreateWindowExA(0, HOST_CLASS, "SongFinderHost", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    g_hLoaderWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        LOADER_CLASS, "", WS_POPUP,
        0, 0, LOADER_W, LOADER_H_WITH_BTN,
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
