// ============================================================================
//  SteamOverlayFix.cpp
//  One-click app to fix the Destiny 2 / Steam overlay bug.
//
//  Two modes in a single .exe:
//    * GUI mode  (double-click)         -> window with Enable / Revert buttons
//    * Watch mode ("SteamOverlayFix.exe --watch") -> background loop that
//      deletes the Steam overlay files every time Steam launches.
//
//  "Enable Fix" copies this exe to %ProgramData%\SteamOverlayFix\, registers a
//  Scheduled Task that runs it as SYSTEM at boot with --watch, and starts it.
//  "Revert" deletes the task, kills the running watcher, and cleans up.
//
//  Build: build.bat
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
// Win7+ for QueryFullProcessImageNameW / PROCESS_QUERY_LIMITED_INFORMATION.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#endif

// ---------------------------------------------------------------------------
static const wchar_t* TASK_NAME   = L"SteamOverlayFix";
static const wchar_t* WATCHER_EXE = L"SteamOverlayFix.exe";

static const wchar_t* kOverlay[] = {
    L"GameOverlayRenderer.dll",   L"GameOverlayRenderer64.dll",
    L"gameoverlayui.exe",         L"gameoverlayui64.exe",
    L"SteamOverlayVulkanLayer.dll",   L"SteamOverlayVulkanLayer64.dll",
    L"SteamOverlayVulkanLayer.json",  L"SteamOverlayVulkanLayer64.json"
};
static const int kOverlayCount = 8;

// ------------------------------- helpers -----------------------------------
static std::wstring EnvVar(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)(sizeof(buf) / sizeof(wchar_t)));
    return (n > 0 && n < sizeof(buf) / sizeof(wchar_t)) ? std::wstring(buf, n) : std::wstring();
}

static std::wstring SelfPath() {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(NULL, buf, (DWORD)(sizeof(buf) / sizeof(wchar_t)));
    return std::wstring(buf);
}

static std::wstring SelfName() {
    std::wstring p = SelfPath();
    size_t s = p.find_last_of(L"\\/");
    return (s == std::wstring::npos) ? p : p.substr(s + 1);
}

static std::wstring ProgramDataDir() {
    std::wstring pd = EnvVar(L"ProgramData");
    if (pd.empty()) pd = L"C:\\ProgramData";
    return pd + L"\\SteamOverlayFix";
}

static bool IEquals(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

static std::wstring RegRead(HKEY root, const wchar_t* sub, const wchar_t* val) {
    HKEY k; std::wstring res;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        wchar_t buf[1024]; DWORD sz = sizeof(buf) - sizeof(wchar_t), type = 0;
        if (RegQueryValueExW(k, val, NULL, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            // Registry strings are not guaranteed NUL-terminated; use the
            // reported byte count and trim any terminator that was written.
            size_t chars = sz / sizeof(wchar_t);
            if (chars > 1023) chars = 1023;
            res.assign(buf, chars);
            size_t z = res.find(L'\0');
            if (z != std::wstring::npos) res.resize(z);
        }
        RegCloseKey(k);
    }
    return res;
}

// Resolve System32 tools by full path - a bare name would let CreateProcess
// search our own directory first and pick up a planted binary.
static std::wstring SystemExe(const wchar_t* name) {
    wchar_t dir[MAX_PATH];
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring(L"C:\\Windows\\System32\\") + name;
    return std::wstring(dir, n) + L"\\" + name;
}

static std::wstring GetSteamPath() {
    std::wstring p;
    p = RegRead(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    p = RegRead(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    p = RegRead(HKEY_CURRENT_USER, L"SOFTWARE\\Valve\\Steam", L"SteamPath");
    if (!p.empty()) { for (auto& c : p) if (c == L'/') c = L'\\'; if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p; }
    std::wstring def = L"C:\\Program Files (x86)\\Steam";
    if (GetFileAttributesW(def.c_str()) != INVALID_FILE_ATTRIBUTES) return def;
    return std::wstring();
}

static int DeleteOverlayFiles() {
    std::wstring steam = GetSteamPath();
    if (steam.empty()) return 0;
    int removed = 0;
    for (int i = 0; i < kOverlayCount; ++i) {
        std::wstring full = steam + L"\\" + kOverlay[i];
        for (int t = 0; t < 4; ++t) {
            DWORD at = GetFileAttributesW(full.c_str());
            if (at == INVALID_FILE_ATTRIBUTES) break;
            if (at & FILE_ATTRIBUTE_READONLY)
                SetFileAttributesW(full.c_str(), at & ~FILE_ATTRIBUTE_READONLY);
            if (DeleteFileW(full.c_str())) { ++removed; break; }
            Sleep(250); // may be briefly locked
        }
    }
    return removed;
}

static bool IsSteamRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// --------------------------- watcher (--watch) -----------------------------
static void WatchLog(const std::wstring& msg) {
    std::wstring dir = ProgramDataDir();
    CreateDirectoryW(dir.c_str(), NULL);
    std::wstring path = dir + L"\\watcher.log";
    // FILE_READ_ATTRIBUTES is required for the GetFileSizeEx below.
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t stamp[64];
    wsprintfW(stamp, L"%04d-%02d-%02d %02d:%02d:%02d  ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring line = std::wstring(stamp) + msg + L"\r\n";
    // UTF-16LE, with a BOM if the file is new.
    DWORD w;
    LARGE_INTEGER size; size.QuadPart = 0;
    GetFileSizeEx(h, &size);
    SetFilePointer(h, 0, NULL, FILE_END);
    if (size.QuadPart == 0) {
        unsigned char bom[2] = { 0xFF, 0xFE };
        WriteFile(h, bom, 2, &w, NULL);
    }
    WriteFile(h, line.data(), (DWORD)(line.size() * sizeof(wchar_t)), &w, NULL);
    CloseHandle(h);
}

// The watcher runs only while this marker file exists: Enable creates it,
// Revert deletes it, the watcher polls it and quits once it is gone. A plain
// file sidesteps the cross-session permission issues a kernel event has
// between the SYSTEM watcher and the elevated GUI.
static std::wstring MarkerPath() { return ProgramDataDir() + L"\\watcher.run"; }

static bool MarkerExists() {
    return GetFileAttributesW(MarkerPath().c_str()) != INVALID_FILE_ATTRIBUTES;
}

static void WriteMarker() {
    std::wstring dir = ProgramDataDir();
    CreateDirectoryW(dir.c_str(), NULL);
    HANDLE h = CreateFileW(MarkerPath().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static void DeleteMarker() {
    DeleteFileW(MarkerPath().c_str());
}

static int RunWatcher() {
    WatchLog(L"Watcher started.");
    if (!MarkerExists()) { WatchLog(L"No run marker present. Exiting."); return 0; }
    DeleteOverlayFiles();               // clean once at start
    bool wasRunning = IsSteamRunning();
    for (;;) {
        Sleep(2000);                    // light poll, negligible CPU
        if (!MarkerExists()) { WatchLog(L"Run marker removed. Exiting."); break; }
        bool now = IsSteamRunning();
        if (now && !wasRunning) {       // Steam just launched
            WatchLog(L"Steam launch detected.");
            for (int pass = 0; pass < 3; ++pass) {
                Sleep(3000);            // let Steam recreate the files
                if (!MarkerExists()) { WatchLog(L"Run marker removed. Exiting."); return 0; }
                int n = DeleteOverlayFiles();
                if (n > 0) { wchar_t b[64]; wsprintfW(b, L"Deleted %d overlay file(s).", n); WatchLog(b); }
            }
        }
        wasRunning = now;
    }
    return 0;
}

// ---------------------------- task management ------------------------------
static bool IsElevated();   // defined below

static std::wstring TempDir() {
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    return (n > 0) ? std::wstring(buf, n) : std::wstring(L".\\");
}

static std::wstring XmlEscape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        switch (c) {
            case L'&':  o += L"&amp;";  break;
            case L'<':  o += L"&lt;";   break;
            case L'>':  o += L"&gt;";   break;
            case L'"':  o += L"&quot;"; break;
            case L'\'': o += L"&apos;"; break;
            default:    o += c;         break;
        }
    }
    return o;
}

static std::wstring BuildTaskXml(const std::wstring& exePath) {
    std::wstring x;
    x += L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n";
    x += L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n";
    x += L"  <RegistrationInfo>\r\n";
    x += L"    <Author>SteamOverlayFix</Author>\r\n";
    x += L"    <Description>Deletes Steam overlay files when Steam launches (Destiny 2 overlay fix).</Description>\r\n";
    x += L"  </RegistrationInfo>\r\n";
    x += L"  <Triggers>\r\n";
    x += L"    <BootTrigger><Enabled>true</Enabled></BootTrigger>\r\n";
    x += L"  </Triggers>\r\n";
    x += L"  <Principals>\r\n";
    x += L"    <Principal id=\"Author\">\r\n";
    x += L"      <UserId>S-1-5-18</UserId>\r\n";           // LocalSystem
    x += L"      <RunLevel>HighestAvailable</RunLevel>\r\n";
    x += L"    </Principal>\r\n";
    x += L"  </Principals>\r\n";
    x += L"  <Settings>\r\n";
    x += L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n";
    x += L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n";
    x += L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n";
    x += L"    <AllowHardTerminate>true</AllowHardTerminate>\r\n";
    x += L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n";
    x += L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n";
    x += L"    <IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>\r\n";
    x += L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n";
    x += L"    <Enabled>true</Enabled>\r\n";
    x += L"    <Hidden>false</Hidden>\r\n";
    x += L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n";
    x += L"    <WakeToRun>false</WakeToRun>\r\n";
    x += L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n"; // no time limit
    x += L"    <Priority>7</Priority>\r\n";
    x += L"  </Settings>\r\n";
    x += L"  <Actions Context=\"Author\">\r\n";
    x += L"    <Exec>\r\n";
    x += L"      <Command>" + XmlEscape(exePath) + L"</Command>\r\n";
    x += L"      <Arguments>--watch</Arguments>\r\n";
    x += L"    </Exec>\r\n";
    x += L"  </Actions>\r\n";
    x += L"</Task>\r\n";
    return x;
}

static bool WriteUtf16File(const std::wstring& path, const std::wstring& content) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    unsigned char bom[2] = { 0xFF, 0xFE };
    DWORD w;
    WriteFile(h, bom, 2, &w, NULL);
    WriteFile(h, content.data(), (DWORD)(content.size() * sizeof(wchar_t)), &w, NULL);
    CloseHandle(h);
    return true;
}

static const DWORD RUN_FAILED  = 0xFFFFFFFF; // could not launch
static const DWORD RUN_TIMEOUT = 0xFFFFFFFE; // still running after the wait

// 'exe' is passed as lpApplicationName so the PATH/CWD search is never used.
static DWORD RunHidden(const std::wstring& exe, const std::wstring& args, DWORD waitMs = 20000) {
    STARTUPINFOW si; ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; ZeroMemory(&pi, sizeof(pi));
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.push_back(0);
    DWORD code = RUN_FAILED;
    if (CreateProcessW(exe.c_str(), buf.data(), NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        // Only read the exit code if the process actually exited; after a
        // timeout GetExitCodeProcess returns STILL_ACTIVE (259).
        if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_OBJECT_0)
            GetExitCodeProcess(pi.hProcess, &code);
        else
            code = RUN_TIMEOUT;
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    return code;
}

static DWORD SchTasks(const std::wstring& args, DWORD waitMs = 20000) {
    return RunHidden(SystemExe(L"schtasks.exe"), args, waitMs);
}

static bool TaskExists() {
    return SchTasks(std::wstring(L"/Query /TN \"") + TASK_NAME + L"\"", 10000) == 0;
}

static bool CreateTask(const std::wstring& exePath, std::wstring& err) {
    std::wstring xmlPath = TempDir() + L"SteamOverlayFix_task.xml";
    if (!WriteUtf16File(xmlPath, BuildTaskXml(exePath))) { err = L"Could not write task definition."; return false; }
    std::wstring args = std::wstring(L"/Create /TN \"") + TASK_NAME +
                        L"\" /XML \"" + xmlPath + L"\" /F";
    DWORD rc = SchTasks(args);
    DeleteFileW(xmlPath.c_str());
    if (rc != 0) {
        err = L"schtasks /Create failed (code " + std::to_wstring((int)rc) + L").";
        if (!IsElevated()) err += L" Run this app as Administrator.";
        return false;
    }
    SchTasks(std::wstring(L"/Run /TN \"") + TASK_NAME + L"\""); // start now
    return true;
}

static void EndTask() {
    SchTasks(std::wstring(L"/End /TN \"") + TASK_NAME + L"\"");
}

static int DeleteTask() {
    int rc = (int)SchTasks(std::wstring(L"/Delete /TN \"") + TASK_NAME + L"\" /F");
    if (rc != 0) // retry once with an explicit root path
        rc = (int)SchTasks(std::wstring(L"/Delete /TN \"\\") + TASK_NAME + L"\" /F");
    return rc;
}

static bool IsElevated() {
    HANDLE tok; TOKEN_ELEVATION el; DWORD sz = sizeof(el);
    bool r = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &sz))
            r = (el.TokenIsElevated != 0);
        CloseHandle(tok);
    }
    return r;
}

static void EnableDebugPrivilege() {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
    LUID luid;
    if (LookupPrivilegeValueW(NULL, SE_DEBUG_NAME, &luid)) {
        TOKEN_PRIVILEGES tp; tp.PrivilegeCount = 1; tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    }
    CloseHandle(tok);
}

// A watcher is a process named either WATCHER_EXE (what Enable installs as) or
// like this exe (the "copy failed, ran in place" fallback), running in session 0
// as the SYSTEM task or from the installed ProgramData path. Both names matter:
// the GUI may well have been launched as "SteamOverlayFix (1).exe".
static bool IsWatcherEntry(const PROCESSENTRY32W& pe, const std::wstring& installed,
                           const std::wstring& myName, DWORD myPid, DWORD* sessOut) {
    if (pe.th32ProcessID == myPid) return false;
    if (_wcsicmp(pe.szExeFile, WATCHER_EXE) != 0 &&
        _wcsicmp(pe.szExeFile, myName.c_str()) != 0) return false;

    DWORD sess = 999; ProcessIdToSessionId(pe.th32ProcessID, &sess);
    if (sessOut) *sessOut = sess;
    if (sess == 0) return true;             // the SYSTEM scheduled task

    HANDLE hq = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
    if (!hq) return false;
    bool match = false;
    wchar_t path[MAX_PATH * 2]; DWORD sz = MAX_PATH * 2;
    if (QueryFullProcessImageNameW(hq, 0, path, &sz))
        match = IEquals(std::wstring(path, sz), installed);
    CloseHandle(hq);
    return match;
}

static bool FindWatcher() {
    std::wstring installed = ProgramDataDir() + L"\\" + WATCHER_EXE;
    std::wstring myName = SelfName();
    DWORD myPid = GetCurrentProcessId();
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (IsWatcherEntry(pe, installed, myName, myPid, NULL)) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Kill the running watcher(s). taskkill /F handles the SYSTEM process cleanly;
// a manual TerminateProcess pass is a backup. We never touch our own PID.
// Appends human-readable diagnostics to 'diag'.
static int KillWatchers(std::wstring& diag) {
    EnableDebugPrivilege();
    std::wstring myName = SelfName();
    std::wstring taskkill = SystemExe(L"taskkill.exe");
    wchar_t me[32]; wsprintfW(me, L"%lu", GetCurrentProcessId());

    DWORD tkrc = RunHidden(taskkill, std::wstring(L"/F /FI \"IMAGENAME eq ") + WATCHER_EXE +
                           L"\" /FI \"PID ne " + me + L"\"", 10000);
    { wchar_t b[64]; wsprintfW(b, L"taskkill rc=%lu; ", tkrc); diag += b; }
    if (!IEquals(myName, WATCHER_EXE)) {   // running under a different name
        DWORD rc2 = RunHidden(taskkill, std::wstring(L"/F /FI \"IMAGENAME eq ") + myName +
                              L"\" /FI \"PID ne " + me + L"\"", 10000);
        wchar_t b[64]; wsprintfW(b, L"taskkill(self) rc=%lu; ", rc2); diag += b;
    }

    std::wstring installed = ProgramDataDir() + L"\\" + WATCHER_EXE;
    DWORD myPid = GetCurrentProcessId();
    int killed = 0, seen = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe; pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                DWORD sess = 999;
                if (!IsWatcherEntry(pe, installed, myName, myPid, &sess)) continue;
                ++seen;
                HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (h) {
                    if (TerminateProcess(h, 0)) { ++killed; wchar_t b[80]; wsprintfW(b, L"killed pid %lu (sess %lu); ", pe.th32ProcessID, sess); diag += b; }
                    else { wchar_t b[96]; wsprintfW(b, L"TerminateProcess pid %lu failed err %lu; ", pe.th32ProcessID, GetLastError()); diag += b; }
                    CloseHandle(h);
                } else {
                    wchar_t b[96]; wsprintfW(b, L"OpenProcess pid %lu failed err %lu; ", pe.th32ProcessID, GetLastError()); diag += b;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (seen == 0) diag += L"no matching watcher found; ";
    return killed;
}

// ------------------------------- GUI ---------------------------------------
enum { IDC_ENABLE = 1001, IDC_DISABLE = 1002, IDC_STATUS = 1003, IDC_LOG = 1004, IDC_LINK = 1005 };
#define IDI_APPICON 101

static HWND  g_status = NULL;
static HWND  g_log = NULL;
static bool  g_enabled = false;
static bool  g_busy = false;   // an Enable/Revert is in progress

// ---- dark theme palette ----
static const COLORREF CLR_BG     = RGB(24, 26, 32);
static const COLORREF CLR_PANEL  = RGB(16, 18, 24);
static const COLORREF CLR_TEXT   = RGB(228, 230, 236);
static const COLORREF CLR_MUTED  = RGB(150, 156, 168);
static const COLORREF CLR_GOOD   = RGB(70, 205, 125);
static const COLORREF CLR_BAD    = RGB(240, 100, 88);
static const COLORREF CLR_ENFILL = RGB(38, 140, 78);
static const COLORREF CLR_ENDOWN = RGB(30, 112, 62);
static const COLORREF CLR_BTN    = RGB(44, 48, 60);
static const COLORREF CLR_BTNDN  = RGB(58, 63, 78);
static const COLORREF CLR_BORDER = RGB(64, 70, 84);
static const COLORREF CLR_LINK   = RGB(96, 168, 255);

static HBRUSH  g_brBg = NULL, g_brPanel = NULL;
static HFONT   g_fBig = NULL, g_fBold = NULL, g_fUI = NULL, g_fMono = NULL;
static HCURSOR g_curHand = NULL, g_curWait = NULL;

// The layout is written in 96-DPI pixels. We are manifested system-DPI-aware,
// so Windows does not scale us; every coordinate and font height goes through
// Sc() to stay correct on scaled displays.
static int g_dpi = 96;
static int Sc(int v) { return MulDiv(v, g_dpi, 96); }

// Enable/Revert block the UI thread on schtasks, taskkill and retry sleeps.
// Drain the queue so the window keeps repainting and the log stays live;
// g_busy guards against the reentry this allows.
static void Pump() {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) { PostQuitMessage((int)m.wParam); break; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

static void AppendLog(const std::wstring& s) {
    if (!g_log) return;
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    std::wstring line = s + L"\r\n";
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line.c_str());
    if (g_busy) Pump();
}

static void RefreshStatus() {
    g_enabled = TaskExists();
    if (g_status) {
        SetWindowTextW(g_status, g_enabled
            ? L"Status:  FIX ENABLED  \x2014  overlay is being removed"
            : L"Status:  DISABLED  \x2014  overlay is active");
        InvalidateRect(g_status, NULL, TRUE);
    }
}

static void DoEnable() {
    AppendLog(L"Enabling fix...");
    if (!IsElevated())
        AppendLog(L"  WARNING: not running as Administrator - this will fail.");
    std::wstring destDir = ProgramDataDir();
    CreateDirectoryW(destDir.c_str(), NULL);
    std::wstring dest = destDir + L"\\" + WATCHER_EXE;
    std::wstring self = SelfPath();
    if (!IEquals(self, dest)) {
        if (!CopyFileW(self.c_str(), dest.c_str(), FALSE)) {
            AppendLog(L"  (note: could not copy exe; using current location)");
            dest = self;
        }
    }
    WriteMarker();  // watcher runs only while this file exists
    std::wstring err;
    if (!CreateTask(dest, err)) { AppendLog(L"  ERROR: " + err); RefreshStatus(); return; }
    int n = DeleteOverlayFiles();
    AppendLog(L"  Scheduled task created and started.");
    if (n > 0) { wchar_t b[64]; wsprintfW(b, L"  Removed %d overlay file(s) now.", n); AppendLog(b); }
    AppendLog(L"  Done. Launch Steam and Destiny 2 normally.");
    RefreshStatus();
}

static void DoDisable() {
    AppendLog(L"Reverting...");
    AppendLog(IsElevated() ? L"  (running elevated)" : L"  (NOT elevated - kill may fail; re-run as admin)");
    DeleteMarker();               // watcher self-exits within ~2s when this is gone
    EndTask();                    // ask the scheduler to stop the running instance
    int rc = DeleteTask();        // remove the task registration

    int k = 0;
    std::wstring diag;
    for (int i = 0; i < 5; ++i) { // kill and confirm it stays dead
        k += KillWatchers(diag);
        Sleep(400);
        Pump();
        if (!FindWatcher()) break;
    }
    bool watcherGone = !FindWatcher();
    (void)k;

    { wchar_t b[80]; wsprintfW(b, L"  schtasks delete returned code %d.", rc); AppendLog(b); }
    if (!diag.empty()) AppendLog(L"  kill: " + diag);

    bool taskGone = !TaskExists();
    AppendLog(taskGone ? L"  Scheduled task removed."
                       : L"  WARNING: task still present - delete \"SteamOverlayFix\" in Task Scheduler.");
    AppendLog(watcherGone ? L"  Watcher stopped - files are no longer being deleted."
                          : L"  Watcher still detected - it will also self-exit within ~2s.");

    for (int i = 0; i < 5; ++i) { Sleep(500); Pump(); } // let the marker-based self-exit land
    if (!FindWatcher()) AppendLog(L"  Confirmed: no watcher running.");
    else AppendLog(L"  NOTE: a watcher still shows - please send me the 'kill:' line above.");

    DeleteFileW((ProgramDataDir() + L"\\" + WATCHER_EXE).c_str()); // best effort
    AppendLog(L"  Done. Steam rebuilds the overlay files on its next launch.");
    RefreshStatus();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hi = ((LPCREATESTRUCTW)lp)->hInstance;
        g_fUI   = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        g_fBig  = CreateFontW(Sc(22), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_fBold = CreateFontW(Sc(16), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        g_fMono = CreateFontW(Sc(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");

        HWND title = CreateWindowW(L"STATIC", L"Destiny 2  \x2014  Steam Overlay Fix",
            WS_CHILD | WS_VISIBLE, Sc(20), Sc(16), Sc(440), Sc(26), hwnd, NULL, hi, NULL);
        SendMessageW(title, WM_SETFONT, (WPARAM)g_fBig, TRUE);

        g_status = CreateWindowW(L"STATIC", L"Status:",
            WS_CHILD | WS_VISIBLE, Sc(20), Sc(50), Sc(440), Sc(22), hwnd, (HMENU)IDC_STATUS, hi, NULL);
        SendMessageW(g_status, WM_SETFONT, (WPARAM)g_fBold, TRUE);

        HWND bEn = CreateWindowW(L"BUTTON", L"Enable Fix",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, Sc(20), Sc(84), Sc(210), Sc(46),
            hwnd, (HMENU)IDC_ENABLE, hi, NULL);
        SendMessageW(bEn, WM_SETFONT, (WPARAM)g_fBold, TRUE);

        HWND bDis = CreateWindowW(L"BUTTON", L"Revert / Disable",
            WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, Sc(250), Sc(84), Sc(210), Sc(46),
            hwnd, (HMENU)IDC_DISABLE, hi, NULL);
        SendMessageW(bDis, WM_SETFONT, (WPARAM)g_fBold, TRUE);

        g_log = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            Sc(20), Sc(146), Sc(440), Sc(180), hwnd, (HMENU)IDC_LOG, hi, NULL);
        SendMessageW(g_log, WM_SETFONT, (WPARAM)g_fMono, TRUE);

        // SysLink needs the comctl32 v6 manifest; fall back to a static if the
        // class is not registered.
        HWND link = CreateWindowExW(0, L"SysLink",
            L"<a href=\"https://ayresiv.com/\">Developed by AyresIV</a>",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | 0x0001 /*LWS_TRANSPARENT*/,
            Sc(20), Sc(336), Sc(300), Sc(20), hwnd, (HMENU)IDC_LINK, hi, NULL);
        if (!link)
            link = CreateWindowW(L"STATIC", L"Developed by AyresIV",
                WS_CHILD | WS_VISIBLE, Sc(20), Sc(336), Sc(300), Sc(20),
                hwnd, (HMENU)IDC_LINK, hi, NULL);
        if (link) SendMessageW(link, WM_SETFONT, (WPARAM)g_fUI, TRUE);

        RefreshStatus();
        AppendLog(L"Ready.  Enable Fix installs the background cleaner as a");
        AppendLog(L"Scheduled Task (runs as SYSTEM at boot).  Revert removes it");
        AppendLog(L"and stops it in memory.");
        std::wstring sp = GetSteamPath();
        AppendLog(sp.empty() ? L"WARNING: Steam folder not detected."
                             : (L"Steam: " + sp));
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp; HWND h = (HWND)lp;
        SetBkMode(dc, TRANSPARENT);
        if (h == g_status)      SetTextColor(dc, g_enabled ? CLR_GOOD : CLR_BAD);
        else if (h == g_log)  { SetTextColor(dc, CLR_TEXT); SetBkColor(dc, CLR_PANEL); return (LRESULT)g_brPanel; }
        else                    SetTextColor(dc, CLR_TEXT);
        return (LRESULT)g_brBg;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, CLR_TEXT); SetBkColor(dc, CLR_PANEL);
        return (LRESULT)g_brPanel;
    }
    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlType != ODT_BUTTON) break;   // let DefWindowProc have it
        bool down = (dis->itemState & ODS_SELECTED) != 0;
        bool off  = (dis->itemState & ODS_DISABLED) != 0;
        COLORREF fill, txt;
        if (dis->CtlID == IDC_ENABLE) { fill = down ? CLR_ENDOWN : CLR_ENFILL; txt = RGB(255,255,255); }
        else                          { fill = down ? CLR_BTNDN  : CLR_BTN;    txt = CLR_TEXT; }
        if (off) txt = CLR_MUTED;
        FillRect(dis->hDC, &dis->rcItem, g_brBg);           // corners match window
        HBRUSH b  = CreateSolidBrush(fill);
        HPEN   pn = CreatePen(PS_SOLID, 1, CLR_BORDER);
        HGDIOBJ ob = SelectObject(dis->hDC, b);
        HGDIOBJ op = SelectObject(dis->hDC, pn);
        RoundRect(dis->hDC, dis->rcItem.left, dis->rcItem.top,
                  dis->rcItem.right, dis->rcItem.bottom, Sc(14), Sc(14));
        SelectObject(dis->hDC, ob); SelectObject(dis->hDC, op);
        DeleteObject(b); DeleteObject(pn);
        wchar_t buf[64]; GetWindowTextW(dis->hwndItem, buf, 64);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, txt);
        HGDIOBJ of = SelectObject(dis->hDC, g_fBold);
        DrawTextW(dis->hDC, buf, -1, &dis->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dis->hDC, of);
        return TRUE;
    }
    case WM_NOTIFY: {
        LPNMHDR nm = (LPNMHDR)lp;
        if (nm->idFrom == IDC_LINK) {
            if (nm->code == NM_CLICK || nm->code == NM_RETURN) {
                PNMLINK pl = (PNMLINK)lp;
                ShellExecuteW(NULL, L"open", pl->item.szUrl, NULL, NULL, SW_SHOWNORMAL);
                return 0;
            }
            if (nm->code == NM_CUSTOMDRAW) {
                // SysLink applies its own colour at item-draw time, so the
                // override has to happen in CDDS_ITEMPREPAINT.
                LPNMCUSTOMDRAW cd = (LPNMCUSTOMDRAW)lp;
                if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                    SetTextColor(cd->hdc, CLR_LINK);
                    SetBkMode(cd->hdc, TRANSPARENT);
                }
                return CDRF_DODEFAULT;
            }
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id != IDC_ENABLE && id != IDC_DISABLE) return 0;
        if (g_busy) return 0;   // Pump() re-enters here; ignore clicks while working
        g_busy = true;
        HWND bEn = GetDlgItem(hwnd, IDC_ENABLE), bDis = GetDlgItem(hwnd, IDC_DISABLE);
        EnableWindow(bEn, FALSE); EnableWindow(bDis, FALSE);
        HCURSOR oldCur = SetCursor(g_curWait);
        if (id == IDC_ENABLE) DoEnable(); else DoDisable();
        SetCursor(oldCur);
        EnableWindow(bEn, TRUE); EnableWindow(bDis, TRUE);
        g_busy = false;
        return 0;
    }
    case WM_SETCURSOR: {
        if (g_busy) { SetCursor(g_curWait); return TRUE; }
        // Owner-draw replaces a button's painting but not its class cursor. A
        // child forwards WM_SETCURSOR to its parent first, so set the hand here
        // (wParam is the window under the cursor).
        HWND over = (HWND)wp;
        int id = GetDlgCtrlID(over);
        if ((id == IDC_ENABLE || id == IDC_DISABLE) && IsWindowEnabled(over)) {
            SetCursor(g_curHand);
            return TRUE;
        }
        break;
    }
    case WM_CLOSE:
        if (g_busy) return 0;   // don't close the window mid-operation
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_status = NULL; g_log = NULL;
        if (g_fBig)  { DeleteObject(g_fBig);  g_fBig  = NULL; }
        if (g_fBold) { DeleteObject(g_fBold); g_fBold = NULL; }
        if (g_fMono) { DeleteObject(g_fMono); g_fMono = NULL; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int RunGui(HINSTANCE hi) {
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);

    HDC screen = GetDC(NULL);
    if (screen) { g_dpi = GetDeviceCaps(screen, LOGPIXELSX); ReleaseDC(NULL, screen); }
    if (g_dpi < 96) g_dpi = 96;

    g_curHand = LoadCursorW(NULL, IDC_HAND);
    g_curWait = LoadCursorW(NULL, IDC_WAIT);
    if (!g_curHand) g_curHand = LoadCursorW(NULL, IDC_ARROW);

    g_brBg    = CreateSolidBrush(CLR_BG);
    g_brPanel = CreateSolidBrush(CLR_PANEL);

    WNDCLASSW wc; ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = g_brBg;
    wc.lpszClassName = L"SteamOverlayFixWnd";
    HICON appIcon = LoadIconW(hi, MAKEINTRESOURCEW(IDI_APPICON));
    if (!appIcon) appIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hIcon = appIcon;
    RegisterClassW(&wc);

    const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX) | WS_VISIBLE;

    RECT r = { 0, 0, Sc(484), Sc(372) };
    AdjustWindowRect(&r, style & ~WS_VISIBLE, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;

    // Centre on the work area (excludes the taskbar) of whichever monitor the
    // cursor is on.
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    POINT cur;
    if (!GetCursorPos(&cur)) { cur.x = 0; cur.y = 0; }
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi; mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) {
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2;
        y = mi.rcWork.top  + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top)  y = mi.rcWork.top;
    }

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Steam Overlay Fix",
        style, x, y, w, h, NULL, NULL, hi, NULL);
    if (!hwnd) return 1;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark)); // DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 2004+/11)
    DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark)); // older Win10 builds
    if (appIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)appIcon);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)appIcon);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

// ------------------------------- entry -------------------------------------
// The manifest already requests elevation; this is the backstop for a build
// made without it. The relaunched copy is elevated so this cannot recurse,
// and --elevated guards the case where UAC is disabled entirely.
static bool RelaunchElevated() {
    std::wstring self = SelfPath();
    SHELLEXECUTEINFOW ei; ZeroMemory(&ei, sizeof(ei));
    ei.cbSize       = sizeof(ei);
    ei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    ei.lpVerb       = L"runas";
    ei.lpFile       = self.c_str();
    ei.lpParameters = L"--elevated";
    ei.nShow        = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&ei)) return false;   // user declined the UAC prompt
    if (ei.hProcess) CloseHandle(ei.hProcess);
    return true;
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool watch = false, relaunched = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"--watch") == 0)    watch = true;
        if (_wcsicmp(argv[i], L"--elevated") == 0) relaunched = true;
    }
    if (argv) LocalFree(argv);

    if (watch) return RunWatcher();

    if (!relaunched && !IsElevated() && RelaunchElevated())
        return 0;   // the elevated instance owns the UI from here

    return RunGui(hi);
}
