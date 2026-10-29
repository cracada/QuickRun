/*
 * QuickRun - 快速启动器
 * ------------------------------------------------------------
 * 编译（TDM-GCC，64 位）：
 *     @windres app.rc -o app_res.o --codepage=65001
 *     gcc QuickRun.c app_res.o -o QuickRun.exe -Os -mwindows -s -lshell32 -ldwmapi -lmsimg32
 *         -Wall -Wextra -Wshadow -Wformat=2 
 */

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN

#ifndef WINVER
#define WINVER 0x0601
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE    ((HANDLE)-3)
#define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE         ((HANDLE)-2)
#endif

#define QR_VERSION L"1.2"

/* GetProcAddress 返回 FARPROC（签名未知），直接强转具体函数指针会触发
   GCC -Wcast-function-type。先经 void(*)(void) 中转可消除该警告。 */
#define QR_GETPROC(h, name, type) (type)(void (*)(void))GetProcAddress((h), (name))

/* ---- 控件 ID ---- */
#define ID_EDIT      1001
#define ID_LIST      1003
#define ID_DATA      1004
#define ID_HELP      1005
#define ID_RESETPOS  1007
#define ID_OPEN_FOLDER  1008

/* ---- 托盘菜单 ID ---- */
#define ID_TRAY_SHOW      2001
#define ID_TRAY_CONFIG    2002
#define ID_TRAY_RELOAD    2003
#define ID_TRAY_DIAG      2004
#define ID_TRAY_AUTOSTART 2005
#define ID_TRAY_EXIT      2006

/* ---- 消息/定时器/热键 ---- */
#define WM_QR_ACTIVATE   (WM_APP + 1)
#define WM_APP_TRAY      (WM_APP + 2)
#define WM_APP_RELOAD    (WM_APP + 3)

#define IDT_EVDEBOUNCE   1
#define HOTKEY_ID        1
#define QR_IPC_MAGIC  0x5152414E

/* ---- 尺寸限制 ---- */
#define MAX_INPUT   512
#define MAX_ARGS    32
#define MAX_ENTRIES 2048
#define MAX_RESULTS_CAP 50

#define EV_SORT_RUN_COUNT_DESC  20

/* =========================================================
 *  Everything SDK 动态加载
 * ========================================================= */
typedef void  (WINAPI *PFN_SetSearchW)(LPCWSTR);
typedef BOOL  (WINAPI *PFN_QueryW)(BOOL);
typedef DWORD (WINAPI *PFN_GetNumResults)(void);
typedef DWORD (WINAPI *PFN_GetResultFullPathNameW)(DWORD, LPWSTR, DWORD);
typedef void  (WINAPI *PFN_SetSort)(DWORD);
typedef void  (WINAPI *PFN_SetMax)(DWORD);

static HMODULE g_hEv = NULL;
static PFN_SetSearchW              g_evSetSearch   = NULL;
static PFN_QueryW                  g_evQuery       = NULL;
static PFN_GetNumResults           g_evGetNum      = NULL;
static PFN_GetResultFullPathNameW  g_evGetPath     = NULL;
static PFN_SetSort                 g_evSetSort     = NULL;
static PFN_SetMax                  g_evSetMax      = NULL;

static void EvLoad(void)
{
    g_hEv = LoadLibraryW(L"Everything64.dll");
    if (!g_hEv) return;
    g_evSetSearch = QR_GETPROC(g_hEv, "Everything_SetSearchW",             PFN_SetSearchW);
    g_evQuery     = QR_GETPROC(g_hEv, "Everything_QueryW",                 PFN_QueryW);
    g_evGetNum    = QR_GETPROC(g_hEv, "Everything_GetNumResults",          PFN_GetNumResults);
    g_evGetPath   = QR_GETPROC(g_hEv, "Everything_GetResultFullPathNameW", PFN_GetResultFullPathNameW);
    g_evSetSort   = QR_GETPROC(g_hEv, "Everything_SetSort",                PFN_SetSort);
    g_evSetMax    = QR_GETPROC(g_hEv, "Everything_SetMax",                 PFN_SetMax);
}

static void EvUnload(void)
{
    if (g_hEv) { FreeLibrary(g_hEv); g_hEv = NULL; }
}

/* =========================================================
 *  全局状态
 * ========================================================= */
static WCHAR g_input[MAX_INPUT];
static WCHAR g_prefill[MAX_INPUT] = { 0 };
static WCHAR g_args[MAX_ARGS][MAX_INPUT];
static int   g_argCount = 0;
static BOOL  g_noFallback = FALSE;
static WCHAR g_dataFile[MAX_PATH * 2];
static WCHAR g_iniPath[MAX_PATH * 2];
static WCHAR g_statePath[MAX_PATH * 2];

static int   g_maxResults = 9;
static int   g_debounceMs = 80;
static int   g_toastDuration = 1800;
static BOOL  g_enableEverything = TRUE;
/* 新增：UI 字体与行高配置 */
static int   g_fontSizeUI = 16;
static int   g_fontSizeEdit = 22;
static int   g_listItemHeight = 32;
/* 新增：高级搜索与行为配置 */
static WCHAR g_extensions[MAX_INPUT] = L"exe;lnk";
static WCHAR g_excludes[MAX_INPUT * 2] = L"C:\\Windows\\SysWOW64;C:\\Windows\\WinSxS;C:\\Windows\\SystemApps;C:\\Windows\\servicing;C:\\Windows\\Microsoft.NET";
static BOOL  g_autoSelectFirst = TRUE;

typedef struct {
    WCHAR key[MAX_INPUT];
    WCHAR cmd[MAX_INPUT];
    WCHAR desc[MAX_INPUT];
    BOOL  useCmd;
    BOOL  hidden;
} Entry;

static Entry g_entries[MAX_ENTRIES];
static int   g_entryCount = 0;
static BOOL  g_dataFileTruncated = FALSE; /* [FIX 12] 数据文件是否因超限被截断 */

/* ---- 数据校验（新增） ---- */
static WCHAR g_parseErrMsg[8192];
static int   g_parseErrCount = 0;

typedef struct {
    WCHAR key[MAX_INPUT];
    int   lineNo;
} SeenKey;
static SeenKey g_seen[MAX_ENTRIES];
static int     g_seenCount = 0;

static void AppendParseErr(int lineNo, const WCHAR *fmt, ...)
{
    WCHAR buf[1024];
    WCHAR line[1100];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 1023, fmt, ap);
    va_end(ap);
    buf[1023] = 0;

    g_parseErrCount++;
    if (g_parseErrCount <= 30) {
        _snwprintf(line, 1099, L"第 %d 行: %s\n", lineNo, buf);
        line[1099] = 0;
        wcsncat(g_parseErrMsg, line,
                (sizeof(g_parseErrMsg) / sizeof(WCHAR)) - 1 - wcslen(g_parseErrMsg));
    } else if (g_parseErrCount == 31) {
        wcsncat(g_parseErrMsg, L"……（其余错误省略）\n",
                (sizeof(g_parseErrMsg) / sizeof(WCHAR)) - 1 - wcslen(g_parseErrMsg));
    }
}

static int FindSeenKey(const WCHAR *key)
{
    int i;
    for (i = 0; i < g_seenCount; i++) {
        if (_wcsicmp(g_seen[i].key, key) == 0) return i;
    }
    return -1;
}

static WCHAR g_matches[MAX_RESULTS_CAP][MAX_INPUT];
static WCHAR g_matchFullPaths[MAX_RESULTS_CAP][MAX_INPUT];
static WCHAR g_matchCmds[MAX_RESULTS_CAP][MAX_INPUT];
static WCHAR g_matchDescs[MAX_RESULTS_CAP][MAX_INPUT];
static BOOL  g_matchUseCmds[MAX_RESULTS_CAP];
static int   g_matchCount = 0;
static int   g_selected = -1;

static WNDPROC g_oldEditProc = NULL;
static BOOL    g_listVisible = FALSE;
static int     g_initW = 0, g_initH = 0, g_expandedH = 0;

static HFONT   g_hFontUI = NULL;
static HFONT   g_hFontEdit = NULL;
static RECT    g_rcEdit = { 0 };
static UINT    g_initDpi = 96;          /* 新增：初始 DPI */
static int     g_extraInitH = 0;        /* 新增：初始 DPI 下的展开增量 */
static RECT    g_rcList = { 0 };          /* 新增：LIST 初始像素布局 */
static RECT    g_originalRcEdit = { 0 };  /* 新增：初始 Edit 布局 */
static WCHAR   g_statusText[MAX_INPUT] = { 0 };
static BOOL    g_inModal = FALSE;
static BOOL    g_activating = FALSE;

static HICON   g_hIconSmall = NULL;
static HICON   g_hIconBig   = NULL;
static HBRUSH  g_hSelBrush  = NULL;
static BOOL    g_quitting = FALSE;
static UINT    g_taskbarCreated = 0;

static WCHAR   g_hotkeyStr[MAX_INPUT] = L"Ctrl+Alt+R";       /* ini 配置值 */
static WCHAR   g_activeHotkeyStr[MAX_INPUT] = { 0 };          /* 实际生效值 */
static BOOL    g_hotkeyRegistered = FALSE;
static UINT    g_hotkeyMods = 0;
static UINT    g_hotkeyVk   = 0;

typedef enum {
    HOTKEY_DISABLED = 0,
    HOTKEY_OK,
    HOTKEY_PARSE_FAIL,
    HOTKEY_REGISTER_FAIL
} HotkeyStatus;
static HotkeyStatus g_hotkeyStatus = HOTKEY_DISABLED;
static BOOL    g_reloadHotkeyWarning = FALSE;

static BOOL    g_guideShown = FALSE;
static BOOL    g_showTrayIcon  = TRUE;
static BOOL    g_fromSilent = FALSE;
static BOOL    g_temporaryMode = FALSE;
static BOOL    g_forceNoTray = FALSE;

static BOOL    g_tplOverflow = FALSE;

static double  g_dpiScale = 1.0;

typedef struct {
    WCHAR input[MAX_INPUT];
} QR_IPC;

/* 前置声明 */
static void ForceForeground(HWND hWnd);
static void ExecuteInput(HWND hDlg, BOOL forceFallback);
static void UpdateSearch(HWND hDlg);
static void ShowList(HWND hDlg, BOOL show);
static void RunCommandEx(const WCHAR *cmd, const WCHAR *desc, BOOL useCmd);
static void UpdateStatusBar(HWND hDlg);
static BOOL LoadWindowPos(int *x, int *y);
static void ResetUI(HWND hDlg, BOOL doShow);
static void AddTrayIcon(HWND hDlg);
static void RemoveTrayIcon(HWND hDlg);
static void LoadConfig(void);
static void LoadState(void);
static void SaveState(HWND hDlg);
static void RegisterHotkeyFromIni(HWND hDlg);
static void ReloadAll(HWND hDlg);
static void ShowQuickToast(HWND hDlg, const WCHAR *text, int level);

/* =========================================================
 *  DPI 支持
 * ========================================================= */
static void EnableDpiAwareness(void)
{
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
        PFN_SetProcessDpiAwarenessContext pfn =
            QR_GETPROC(hUser32, "SetProcessDpiAwarenessContext", PFN_SetProcessDpiAwarenessContext);
        if (pfn) {
            if (pfn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
            if (pfn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))    return;
            if (pfn(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))         return;
            return; /* 已设置或失败，不再尝试 */
        }
    }
    {
        HMODULE hShcore = LoadLibraryW(L"shcore.dll");
        if (hShcore) {
            typedef HRESULT (WINAPI *PFN_SetProcessDpiAwareness)(int);
            PFN_SetProcessDpiAwareness pfn =
                QR_GETPROC(hShcore, "SetProcessDpiAwareness", PFN_SetProcessDpiAwareness);
            if (pfn) { pfn(2); FreeLibrary(hShcore); return; }
            FreeLibrary(hShcore);
        }
    }
    SetProcessDPIAware();
}

static UINT GetDpiForWindowSafe(HWND hWnd)
{
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
        PFN_GetDpiForWindow pfn = QR_GETPROC(hUser32, "GetDpiForWindow", PFN_GetDpiForWindow);
        if (pfn) {
            UINT dpi = pfn(hWnd);
            if (dpi) return dpi;
        }
    }
    HDC hdc = GetDC(NULL);
    UINT dpi = hdc ? (UINT)GetDeviceCaps(hdc, LOGPIXELSX) : 96;
    if (hdc) ReleaseDC(NULL, hdc);
    return dpi ? dpi : 96;
}

/* =========================================================
 *  工具
 * ========================================================= */
static void TrimW(WCHAR *s)
{
    WCHAR *p = s;
    size_t n;
    while (*p == L' ' || *p == L'\t') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(WCHAR));
    n = wcslen(s);
    while (n > 0 && (s[n - 1] == L' '  || s[n - 1] == L'\t' ||
                     s[n - 1] == L'\r' || s[n - 1] == L'\n'))
        s[--n] = 0;
}

static BOOL ContainsIgnoreCase(const WCHAR *hay, const WCHAR *needle, size_t nl)
{
    if (nl == 0) return TRUE;
    for (const WCHAR *p = hay; *p; p++) {
        if (_wcsnicmp(p, needle, nl) == 0) return TRUE;
    }
    return FALSE;
}

/* 按 CommandLineToArgvW 规则转义并追加一个带引号的参数 */
static void AppendQuotedArg(WCHAR *dst, size_t cap, size_t *pos, const WCHAR *src)
{
    size_t p = *pos;
    if (p + 2 >= cap) return;
    dst[p++] = L'"';
    while (*src) {
        size_t numBs = 0;
        while (src[numBs] == L'\\') numBs++;
        if (src[numBs] == L'"') {
            size_t total = numBs * 2 + 1;
            size_t i;
            for (i = 0; i < total && p + 2 < cap; i++) dst[p++] = L'\\';
            if (p + 2 >= cap) break;
            dst[p++] = L'"';
            src += numBs + 1;
        } else if (src[numBs] == 0) {
            size_t total = numBs * 2;
            size_t i;
            for (i = 0; i < total && p + 2 < cap; i++) dst[p++] = L'\\';
            src += numBs;
        } else {
            size_t i;
            for (i = 0; i < numBs && p + 2 < cap; i++) dst[p++] = L'\\';
            if (p + 2 >= cap) break;
            dst[p++] = src[numBs];
            src += numBs + 1;
        }
    }
    if (p + 2 < cap) dst[p++] = L'"';
    dst[p] = 0;
    *pos = p;
}

static BOOL GetClipboardText(WCHAR *buf, size_t bufSize)
{
    if (!buf || bufSize == 0) return FALSE;
    buf[0] = 0;
    if (!OpenClipboard(NULL)) return FALSE;
    BOOL ok = FALSE;
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData) {
        WCHAR *p = (WCHAR *)GlobalLock(hData);
        if (p) {
            wcsncpy(buf, p, bufSize - 1);
            buf[bufSize - 1] = 0;
            GlobalUnlock(hData);
            ok = TRUE;
        }
    }
    CloseClipboard();
    return ok;
}

static void SplitArgs(const WCHAR *s, int *outCount)
{
    int n = 0;
    const WCHAR *p = s;
    while (*p && n < MAX_ARGS) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        const WCHAR *start;
        size_t len;
        if (*p == L'"') {
            p++;
            start = p;
            while (*p && *p != L'"') p++;
            len = (size_t)(p - start);
            if (*p == L'"') p++;
        } else {
            start = p;
            while (*p && *p != L' ' && *p != L'\t') p++;
            len = (size_t)(p - start);
        }
        if (len >= MAX_INPUT) len = MAX_INPUT - 1;
        wcsncpy(g_args[n], start, len);
        g_args[n][len] = 0;
        n++;
    }
    *outCount = n;
}

static void SubstituteArgs(const WCHAR *src, WCHAR *dst, size_t dstSize)
{
    size_t di = 0;
    const WCHAR *p = src;
    while (*p && di + 1 < dstSize) {
        if (wcsncmp(p, L"!clip!", 6) == 0) {
            WCHAR clip[MAX_INPUT * 4];
            if (GetClipboardText(clip, MAX_INPUT * 4)) {
                size_t clen = wcslen(clip);
                if (di + clen < dstSize) {
                    memcpy(dst + di, clip, clen * sizeof(WCHAR));
                    di += clen;
                }
            }
            p += 6;
            continue;
        }
        if (*p == L'{') {
            const WCHAR *close = wcschr(p + 1, L'}');
            if (close && close - p >= 2 && close - p <= 4) {
                WCHAR inner[4] = { 0 };
                size_t n = (size_t)(close - p - 1);
                if (n < 4) wcsncpy(inner, p + 1, n);
                if (inner[0] == L'*' && inner[1] == 0) {
                    for (int i = 0; i < g_argCount; i++) {
                        if (i > 0 && di + 1 < dstSize) dst[di++] = L' ';
                        size_t alen = wcslen(g_args[i]);
                        if (di + alen < dstSize) {
                            memcpy(dst + di, g_args[i], alen * sizeof(WCHAR));
                            di += alen;
                        }
                    }
                    p = close + 1;
                    continue;
                }
                BOOL isNum = (n > 0);
                for (size_t k = 0; k < n; k++) {
                    if (inner[k] < L'0' || inner[k] > L'9') { isNum = FALSE; break; }
                }
                if (isNum) {
                    int idx = _wtoi(inner);
                    const WCHAR *val = NULL;
                    if (idx == 0) val = g_input;
                    else if (idx >= 1 && idx <= g_argCount) val = g_args[idx - 1];
                    if (val) {
                        size_t vlen = wcslen(val);
                        if (di + vlen < dstSize) {
                            memcpy(dst + di, val, vlen * sizeof(WCHAR));
                            di += vlen;
                        }
                    }
                    p = close + 1;
                    continue;
                }
            }
        }
        dst[di++] = *p++;
    }
    dst[di] = 0;
}

static BOOL ParseHotkey(const WCHAR *s, UINT *outMods, UINT *outVk)
{
    if (!s || !*s) return FALSE;
    WCHAR buf[MAX_INPUT];
    wcsncpy(buf, s, MAX_INPUT - 1);
    buf[MAX_INPUT - 1] = 0;
    WCHAR *parts[16];
    int nParts = 0;
    WCHAR *start = buf;
    WCHAR *p = buf;
    while (*p && nParts < 15) {
        if (*p == L'+') { *p = 0; parts[nParts++] = start; start = p + 1; }
        p++;
    }
    if (*start || nParts > 0) parts[nParts++] = start;
    if (nParts == 0) return FALSE;
    WCHAR *main = parts[nParts - 1];
    while (*main == L' ') main++;
    WCHAR *me = main + wcslen(main);
    while (me > main && *(me - 1) == L' ') *--me = 0;
    UINT vk = 0;
    if (wcslen(main) == 1) {
        WCHAR c = main[0];
        if (c >= L'a' && c <= L'z')      vk = c - L'a' + 'A';
        else if (c >= L'A' && c <= L'Z') vk = c;
        else if (c >= L'0' && c <= L'9') vk = c;
        else return FALSE;
    } else if (main[0] == L'F' || main[0] == L'f') {
        int fn = _wtoi(main + 1);
        if (fn >= 1 && fn <= 24) vk = VK_F1 + fn - 1;
        else return FALSE;
    } else return FALSE;
    UINT mods = 0;
    for (int i = 0; i < nParts - 1; i++) {
        WCHAR *m = parts[i];
        while (*m == L' ') m++;
        WCHAR *mend = m + wcslen(m);
        while (mend > m && *(mend - 1) == L' ') *--mend = 0;
        if (_wcsicmp(m, L"Ctrl") == 0 || _wcsicmp(m, L"Control") == 0) mods |= MOD_CONTROL;
        else if (_wcsicmp(m, L"Alt") == 0) mods |= MOD_ALT;
        else if (_wcsicmp(m, L"Shift") == 0) mods |= MOD_SHIFT;
        else if (_wcsicmp(m, L"Win") == 0) mods |= MOD_WIN;
        else return FALSE;
    }
    *outMods = mods;
    *outVk = vk;
    return vk != 0;
}

static void RegisterHotkeyFromIni(HWND hDlg)
{
    g_hotkeyStatus = HOTKEY_DISABLED;
    g_reloadHotkeyWarning = FALSE;

    if (!g_hotkeyStr[0]) {
        if (g_hotkeyRegistered) {
            UnregisterHotKey(hDlg, HOTKEY_ID);
            g_hotkeyRegistered = FALSE;
        }
        g_hotkeyStatus = HOTKEY_DISABLED;
        g_activeHotkeyStr[0] = 0;
        g_hotkeyMods = 0;   /* 新增：清空上次成功热键的修饰符，防止后续冲突时误恢复 */
        g_hotkeyVk   = 0;   /* 新增：清空上次成功热键的主键 */        
        return;
    }

    UINT mods = 0, vk = 0;
    if (!ParseHotkey(g_hotkeyStr, &mods, &vk)) {
        if (g_hotkeyRegistered) {
            /* 旧热键仍生效：状态保持 OK，抑制 ReloadAll 末尾的成功 toast，
               避免“⚠️ 格式错误”被“✅ 重载成功”覆盖。 */
            g_hotkeyStatus = HOTKEY_OK;
            g_reloadHotkeyWarning = TRUE;
            ShowQuickToast(hDlg, L"⚠️ 热键格式错误，已保留原热键", TRUE);
        } else {
            /* 没有可回退的热键：状态为格式错误。 */
            g_hotkeyStatus = HOTKEY_PARSE_FAIL;
            ShowQuickToast(hDlg, L"⚠️ 热键格式错误，请检查 ini", TRUE);
        }
        return;
    }

    if (g_hotkeyRegistered && mods == g_hotkeyMods && vk == g_hotkeyVk) {
        g_hotkeyStatus = HOTKEY_OK;
        /* g_activeHotkeyStr 已是上次成功注册时对应的字符串，无需变化 */
        return;
    }

    if (g_hotkeyRegistered) {
        UnregisterHotKey(hDlg, HOTKEY_ID);
        g_hotkeyRegistered = FALSE;
    }

    if (RegisterHotKey(hDlg, HOTKEY_ID, mods | MOD_NOREPEAT, vk)) {
        g_hotkeyRegistered = TRUE;
        g_hotkeyMods = mods;
        g_hotkeyVk   = vk;
        g_hotkeyStatus = HOTKEY_OK;
        wcsncpy(g_activeHotkeyStr, g_hotkeyStr, MAX_INPUT - 1);
        g_activeHotkeyStr[MAX_INPUT - 1] = 0;
        return;
    }

    g_hotkeyStatus = HOTKEY_REGISTER_FAIL;

    /* 尝试恢复上一次成功的热键 */
    if (g_hotkeyMods && g_hotkeyVk) {
        if (RegisterHotKey(hDlg, HOTKEY_ID, g_hotkeyMods | MOD_NOREPEAT, g_hotkeyVk)) {
            g_hotkeyRegistered = TRUE;
            g_hotkeyStatus = HOTKEY_OK;
            g_reloadHotkeyWarning = TRUE;  /* 抑制 ReloadAll 的成功 toast，避免覆盖警告 */
            ShowQuickToast(hDlg, L"⚠️ 新热键冲突，已自动恢复原热键", TRUE);
            return;
        }
    }

    ShowQuickToast(hDlg, L"❌ 热键冲突，请在 ini 中修改", TRUE);
}

static BOOL IsAutoStartEnabled(void)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS) return FALSE;
    WCHAR val[MAX_PATH * 2];
    DWORD sz = sizeof(val);
    DWORD type = 0;
    BOOL ok = FALSE;
    if (RegQueryValueExW(hKey, L"QuickRun", NULL, &type,
                         (BYTE *)val, &sz) == ERROR_SUCCESS &&
        type == REG_SZ) ok = TRUE;
    RegCloseKey(hKey);
    return ok;
}

static void SetAutoStart(BOOL enable)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) return;
    if (enable) {
        WCHAR exePath[MAX_PATH];
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        WCHAR quoted[MAX_PATH + 32];
        wsprintfW(quoted, L"\"%s\" --silent", exePath);
        RegSetValueExW(hKey, L"QuickRun", 0, REG_SZ,
                       (BYTE *)quoted,
                       (DWORD)((wcslen(quoted) + 1) * sizeof(WCHAR)));
    } else {
        RegDeleteValueW(hKey, L"QuickRun");
    }
    RegCloseKey(hKey);
}

static void BuildPaths(void)
{
    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR *slash = wcsrchr(exe, L'\\');
    if (slash) slash[1] = 0;
    wsprintfW(g_iniPath,   L"%sQuickRun.ini",   exe);
    wsprintfW(g_statePath, L"%sQuickRun.qrs", exe);
}

static void EnsureConfigFileExists(void)
{
    if (GetFileAttributesW(g_iniPath) != INVALID_FILE_ATTRIBUTES) return;
    FILE *fp = _wfopen(g_iniPath, L"wb");
    if (!fp) return;
    fputc(0xFF, fp);
    fputc(0xFE, fp);
    static const WCHAR tmpl[] =
        L"; QuickRun 配置文件\r\n"
        L"; 修改后请在托盘菜单中执行\"重载\"生效（不需要重启程序）\r\n"
        L"\r\n"
        L"[Hotkey]\r\n"
        L"; 全局热键。留空禁用（此时只能用托盘或 --show 唤起）\r\n"
        L"; 格式：修饰符+修饰符+主键\r\n"
        L"; 修饰符：Ctrl / Alt / Shift / Win（可组合）\r\n"
        L"; 主键：A-Z, 0-9, F1-F24\r\n"
        L"Key=Ctrl+Alt+R\r\n"
        L"\r\n"
        L"[Tray]\r\n"
        L"; 是否在通知区域（托盘）显示 QuickRun 图标（1=显示，0=隐藏）\r\n"
        L"; 隐藏后：仍可用热键唤起窗口，唤起后按 Ctrl+Q 退出程序\r\n"
        L"ShowTrayIcon=1\r\n"
        L"\r\n"
        L"[UI]\r\n"
        L"; 搜索出结果时，默认是否选中第一项（1=选中，0=不选）\r\n"
        L"AutoSelectFirst=1\r\n"
        L"; 列表最大显示结果条数（1-50，默认 9）\r\n"
        L"MaxResults=9\r\n"
        L"; Toast 提示框停留时间毫秒（默认 1800）。0 = 彻底关闭提示\r\n"
        L"ToastDuration=1800\r\n"
        L"; 列表和状态栏基础字体大小（默认 16）\r\n"
        L"FontSizeUI=16\r\n"
        L"; 输入框字体大小（默认 22）\r\n"
        L"FontSizeEdit=22\r\n"
        L"; 列表每一行的高度（默认 32）\r\n"
        L"ListItemHeight=32\r\n"
        L"\r\n"
        L"[Search]\r\n"
        L"; 是否启用 Everything 文件搜索（1=启用，0=禁用）\r\n"
        L"EnableEverything=1\r\n"
        L"; 输入防抖延迟（毫秒）。越小打字越灵敏，但 Everything 负担越重（推荐 40-150）\r\n"
        L"DebounceMs=80\r\n"
        L"; 允许搜索的文件扩展名（分号分隔，留空代表不限）\r\n"
        L"Extensions=exe;lnk\r\n"
        L"; 需要排除的搜索目录（分号分隔，留空代表不排除）\r\n"
        L"Excludes=C:\\Windows\\SysWOW64; C:\\Windows\\WinSxS; C:\\Windows\\SystemApps; C:\\Windows\\servicing; C:\\Windows\\Microsoft.NET\r\n";
    fwrite(tmpl, sizeof(WCHAR), wcslen(tmpl), fp);
    fclose(fp);
}

static void EnsureDataFileExists(void)
{
    if (GetFileAttributesW(g_dataFile) != INVALID_FILE_ATTRIBUTES) return;
    FILE *fp = _wfopen(g_dataFile, L"wb");
    if (!fp) return;

    static const WCHAR tmpl[] =
        L"# QuickRun 命令数据文件\r\n"
        L"# 格式：key,命令,描述\r\n"
        L"#   key    触发键，可用 | 分隔多个（如 calc|jsq）\r\n"
        L"#   命令   要执行的程序/路径；以 !cmd! 开头表示 cmd /k 执行命令\r\n"
        L"#   描述   显示在列表右侧的说明\r\n"
        L"#   # 或 ; 开头的行是注释\r\n"
        L"#   key 前加 @ 表示隐藏条目（不参与搜索，仍可精确匹配执行）\r\n"
        L"#\r\n"
        L"# 占位符：\r\n"
        L"#   {1} {2} ...   命令行第 N 个参数（{0} 为命令 key 自身）\r\n"
        L"#   {*}           全部参数\r\n"
        L"#   !clip!        当前剪贴板文本\r\n"
        L"#\r\n"
        L"# 示例：\r\n"
        L"notepad,notepad.exe,打开记事本\r\n"
        L"calc,calc.exe,打开计算器\r\n"
        /* [FIX 9] !cmd! 本身即等价 cmd /k，命令体不要再写 cmd /k；search 不需要 !cmd! */
        L"cmd,!cmd!echo QuickRun 命令示例,命令示例（cmd /k）\r\n"
        L"search,explorer \"https://www.bing.com/search?q={*}\",Bing 搜索\r\n"
        L"@hide,notepad.exe,隐藏示例（精确输入 hide 才可执行）\r\n"
        L"echo,cmd.exe /c echo {*},回显参数\r\n";

    int len = WideCharToMultiByte(CP_UTF8, 0, tmpl, -1, NULL, 0, NULL, NULL);
    if (len > 1) {
        char *buf = (char *)malloc(len);
        if (buf) {
            WideCharToMultiByte(CP_UTF8, 0, tmpl, -1, buf, len, NULL, NULL);
            fputc(0xEF, fp); fputc(0xBB, fp); fputc(0xBF, fp);  /* UTF-8 BOM */
            fwrite(buf, 1, (size_t)(len - 1), fp);
            free(buf);
        }
    }
    fclose(fp);
}

static void LoadConfig(void)
{
    WCHAR buf[MAX_INPUT];
    GetPrivateProfileStringW(L"Hotkey", L"Key", L"Ctrl+Alt+R", buf, MAX_INPUT, g_iniPath);
    TrimW(buf);
    wcsncpy(g_hotkeyStr, buf, MAX_INPUT - 1);
    g_hotkeyStr[MAX_INPUT - 1] = 0;

    g_showTrayIcon  = GetPrivateProfileIntW(L"Tray", L"ShowTrayIcon", 1, g_iniPath) != 0;

    g_maxResults = GetPrivateProfileIntW(L"UI", L"MaxResults", 9, g_iniPath);
    if (g_maxResults < 1) g_maxResults = 1;
    if (g_maxResults > MAX_RESULTS_CAP) g_maxResults = MAX_RESULTS_CAP;

    g_toastDuration = GetPrivateProfileIntW(L"UI", L"ToastDuration", 1800, g_iniPath);
    if (g_toastDuration < 0) g_toastDuration = 0;

    g_enableEverything = GetPrivateProfileIntW(L"Search", L"EnableEverything", 1, g_iniPath) != 0;

    g_debounceMs = GetPrivateProfileIntW(L"Search", L"DebounceMs", 80, g_iniPath);
    if (g_debounceMs < 10) g_debounceMs = 10;
    if (g_debounceMs > 1000) g_debounceMs = 1000;

    g_fontSizeUI = GetPrivateProfileIntW(L"UI", L"FontSizeUI", 16, g_iniPath);
    g_fontSizeEdit = GetPrivateProfileIntW(L"UI", L"FontSizeEdit", 22, g_iniPath);
    g_listItemHeight = GetPrivateProfileIntW(L"UI", L"ListItemHeight", 32, g_iniPath);
    
    g_autoSelectFirst = GetPrivateProfileIntW(L"UI", L"AutoSelectFirst", 1, g_iniPath) != 0;

    GetPrivateProfileStringW(L"Search", L"Extensions", L"exe;lnk", g_extensions, MAX_INPUT, g_iniPath);
    TrimW(g_extensions);
    GetPrivateProfileStringW(L"Search", L"Excludes", 
        L"C:\\Windows\\SysWOW64;C:\\Windows\\WinSxS;C:\\Windows\\SystemApps;C:\\Windows\\servicing;C:\\Windows\\Microsoft.NET", 
        g_excludes, MAX_INPUT * 2, g_iniPath);
    TrimW(g_excludes);

    if (g_fontSizeUI < 10) g_fontSizeUI = 10;
    if (g_fontSizeUI > 72) g_fontSizeUI = 72;
    if (g_fontSizeEdit < 10) g_fontSizeEdit = 10;
    if (g_fontSizeEdit > 72) g_fontSizeEdit = 72;
    if (g_listItemHeight < 10) g_listItemHeight = 10;
    if (g_listItemHeight > 200) g_listItemHeight = 200;
}

static void LoadState(void)
{
    g_guideShown = (GetFileAttributesW(g_statePath) != INVALID_FILE_ATTRIBUTES);
}

static void SaveState(HWND hDlg)
{
    if (!IsWindow(hDlg)) return;
    RECT rc;
    GetWindowRect(hDlg, &rc);
    WCHAR buf[32];
    wsprintfW(buf, L"%d", rc.left);
    WritePrivateProfileStringW(L"Window", L"X", buf, g_statePath);
    wsprintfW(buf, L"%d", rc.top);
    WritePrivateProfileStringW(L"Window", L"Y", buf, g_statePath);
}

static BOOL LoadWindowPos(int *x, int *y)
{
    int px = GetPrivateProfileIntW(L"Window", L"X", -9999, g_statePath);
    int py = GetPrivateProfileIntW(L"Window", L"Y", -9999, g_statePath);
    if (px == -9999 || py == -9999) return FALSE;
    POINT pt = { px + 30, py + 15 };
    if (MonitorFromPoint(pt, MONITOR_DEFAULTTONULL) == NULL) return FALSE;
    *x = px; *y = py;
    return TRUE;
}

static void RunCommandEx(const WCHAR *cmd, const WCHAR *desc, BOOL useCmd)
{
    WCHAR substituted[4096];
    WCHAR expanded[4096];
    WCHAR line[4096];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    size_t n;
    BOOL ok = FALSE;
    DWORD err = 0;

    SubstituteArgs(cmd, substituted, 4096);
    if (ExpandEnvironmentStringsW(substituted, expanded, 4096) == 0) {
        wcsncpy(expanded, substituted, 4095);
        expanded[4095] = 0;
    }

    if (useCmd) {
        WCHAR cmdLine[4400];
        wsprintfW(cmdLine, L"cmd /k %s", expanded);
        ZeroMemory(&si, sizeof(si));
        si.cb = sizeof(si);
        si.lpTitle = (LPWSTR)((desc && *desc) ? desc : L"QuickRun");
        ZeroMemory(&pi, sizeof(pi));
        if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            DWORD e = GetLastError();
            WCHAR msg[8192];
            wsprintfW(msg, L"命令执行失败：\n\n%s\n\n错误码：%lu", expanded, e);
            g_inModal = TRUE;
            MessageBoxW(NULL, msg, L"错误", MB_ICONERROR);
            g_inModal = FALSE;
            return;
        }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        return;
    }

    n = wcslen(expanded);
    if (n > 4095) n = 4095;
    memcpy(line, expanded, n * sizeof(WCHAR));
    line[n] = 0;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (CreateProcessW(NULL, line, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        ok = TRUE;
    } else {
        err = GetLastError();
        HINSTANCE hi = ShellExecuteW(NULL, L"open", expanded, NULL, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)hi > 32) ok = TRUE;
        else                  err = (DWORD)(INT_PTR)hi;
    }

    if (!ok) {
        /* 最大长度：cmd 512 + substituted 4096 + expanded 4096 + desc 512 + 格式串/错误码约 52 ≈ 9268 */
        WCHAR msg[12288];
        wsprintfW(msg, L"命令执行失败：\n\n原始：%s\n\n替换参数后：%s\n\n展开环境变量后：%s\n\n说明：%s\n\n错误码：%lu",
            cmd, substituted, expanded, desc, err);
        g_inModal = TRUE;
        MessageBoxW(NULL, msg, L"错误", MB_ICONERROR);
        g_inModal = FALSE;
    }
}

static BOOL LoadDataFile(const WCHAR *file)
{
    FILE  *fp;
    long   sz;
    size_t got;
    char  *raw;
    WCHAR *wbuf;
    int    wlen;
    UINT   cp = CP_UTF8;

    g_dataFileTruncated = FALSE; /* [FIX 12] 每次加载重置截断标志 */
    g_parseErrCount = 0;
    g_parseErrMsg[0] = 0;
    g_seenCount = 0;

    fp = _wfopen(file, L"rb");
    if (!fp) return FALSE;

    g_entryCount = 0;
    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0)  { fclose(fp); return FALSE; }   /* ftell 失败 = 真正的读取错误 */
    if (sz == 0) { fclose(fp); return TRUE;  }   /* 空文件 = 合法的 0 条命令 */
    long origSz = sz;
    if (sz > 4 * 1024 * 1024) sz = 4 * 1024 * 1024;

    raw = (char *)malloc((size_t)sz + 1);
    if (!raw) { fclose(fp); return FALSE; }

    got = fread(raw, 1, (size_t)sz, fp);
    fclose(fp);
    raw[got] = 0;

    /* 文件被 4MB 上限截断时，把末尾回退到完整的 UTF-8 字符边界；
       否则 MB_ERR_INVALID_CHARS 严格解码会失败，导致整文件回退 ANSI 而全部乱码。 */
    if (got == (size_t)sz && origSz > 4 * 1024 * 1024) {
        size_t k = got;
        while (k > 0 && (((unsigned char)raw[k - 1]) & 0xC0) == 0x80) k--;   /* 跳过续字节 */
        if (k > 0) {
            unsigned char c = (unsigned char)raw[k - 1];
            int need = (c & 0x80) == 0x00 ? 1 :
                       (c & 0xE0) == 0xC0 ? 2 :
                       (c & 0xF0) == 0xE0 ? 3 :
                       (c & 0xF8) == 0xF0 ? 4 : 0;
            if (need > 0 && got - (k - 1) < (size_t)need) {
                got = k - 1;
            }
        }
        /* [FIX 12] 再回退到最后一个完整换行符，丢弃可能不完整的最后一行；
           若找不到换行，则保留字符边界截断。 */
        {
            size_t m = got;
            while (m > 0 && raw[m - 1] != '\n') m--;
            if (m > 0) got = m;
        }
        raw[got] = 0;
        g_dataFileTruncated = TRUE;
    }

    wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, raw, (int)got, NULL, 0);
    if (wlen <= 0) {
        cp   = CP_ACP;
        wlen = MultiByteToWideChar(CP_ACP, 0, raw, (int)got, NULL, 0);
    }
    if (wlen <= 0) { free(raw); return FALSE; }

    wbuf = (WCHAR *)malloc(((size_t)wlen + 1) * sizeof(WCHAR));
    if (!wbuf) { free(raw); return FALSE; }
    MultiByteToWideChar(cp, 0, raw, (int)got, wbuf, wlen);
    wbuf[wlen] = 0;
    free(raw);

    if (wbuf[0] == 0xFEFF) memmove(wbuf, wbuf + 1, ((size_t)wlen) * sizeof(WCHAR));

    WCHAR *p = wbuf;
    int lineNo = 1;
    while (*p && g_entryCount < MAX_ENTRIES) {
        WCHAR *eol  = wcschr(p, L'\n');
        WCHAR *line = p;
        size_t ln;
        if (!eol && g_dataFileTruncated) break; /* 截断的不完整最后一行，静默跳过 */
        if (eol) *eol = 0;
        ln = wcslen(line);
        while (ln > 0 && (line[ln - 1] == L'\r' || line[ln - 1] == L' ' || line[ln - 1] == L'\t'))
            line[--ln] = 0;

        WCHAR *s = line;
        while (*s == L' ' || *s == L'\t') s++;

        if (*s && *s != L'#' && *s != L';') {
            WCHAR *c1 = wcschr(s, L',');
            if (!c1) {
                AppendParseErr(lineNo, L"缺少逗号（格式：key,命令[,描述]）");
            } else {
                WCHAR *f2, *c2, *desc;
                WCHAR emptyDesc[1] = { 0 };
                *c1 = 0;
                f2  = c1 + 1;
                c2  = wcschr(f2, L',');
                if (c2) { *c2 = 0; desc = c2 + 1; }
                else    { desc = emptyDesc; }

                TrimW(s); TrimW(f2); TrimW(desc);

                BOOL hidden = FALSE;
                if (*s == L'@') { hidden = TRUE; s++; TrimW(s); }

                if (wcslen(s) > MAX_INPUT - 1)
                    AppendParseErr(lineNo, L"key 过长（%d 字符），将截断到 %d",
                                   (int)wcslen(s), MAX_INPUT - 1);
                if (wcslen(f2) > MAX_INPUT - 1)
                    AppendParseErr(lineNo, L"命令过长（%d 字符），将截断到 %d",
                                   (int)wcslen(f2), MAX_INPUT - 1);
                if (wcslen(desc) > MAX_INPUT - 1)
                    AppendParseErr(lineNo, L"描述过长（%d 字符），将截断到 %d",
                                   (int)wcslen(desc), MAX_INPUT - 1);

                BOOL useCmd = FALSE;
                if (wcsncmp(f2, L"!cmd!", 5) == 0) {
                    useCmd = TRUE; f2 += 5; TrimW(f2);
                } else if (f2[0] == L'!' && _wcsnicmp(f2, L"!cmd", 4) == 0) {
                    AppendParseErr(lineNo, L"!cmd! 前缀疑似拼错（应为全小写 !cmd!）");
                }

                if (!*s) {
                    AppendParseErr(lineNo, L"key 为空");
                } else if (!*f2) {
                    AppendParseErr(lineNo, L"命令为空");
                } else {
                    WCHAR *tok = s;
                    while (*tok && g_entryCount < MAX_ENTRIES) {
                        WCHAR *bar  = wcschr(tok, L'|');
                        WCHAR *next = NULL;
                        if (bar) { *bar = 0; next = bar + 1; }
                        TrimW(tok);
                        if (*tok) {
                            int dup = FindSeenKey(tok);
                            if (dup >= 0) {
                                AppendParseErr(lineNo,
                                    L"重复 key \"%s\"（首次在第 %d 行）",
                                    tok, g_seen[dup].lineNo);
                            } else if (g_seenCount < MAX_ENTRIES) {
                                wcsncpy(g_seen[g_seenCount].key, tok, MAX_INPUT - 1);
                                g_seen[g_seenCount].key[MAX_INPUT - 1] = 0;
                                g_seen[g_seenCount].lineNo = lineNo;
                                g_seenCount++;
                            }
                            Entry *e = &g_entries[g_entryCount];
                            wcsncpy(e->key,  tok,  MAX_INPUT - 1);
                            wcsncpy(e->cmd,  f2,   MAX_INPUT - 1);
                            wcsncpy(e->desc, desc, MAX_INPUT - 1);
                            e->key[MAX_INPUT - 1]  = 0;
                            e->cmd[MAX_INPUT - 1]  = 0;
                            e->desc[MAX_INPUT - 1] = 0;
                            e->useCmd = useCmd;
                            e->hidden = hidden;
                            g_entryCount++;
                        } else {
                            AppendParseErr(lineNo, L"key 列表中有空项（检查 | 分隔符）");
                        }
                        if (!next) break;
                        tok = next;
                    }
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
        lineNo++;
    }
    if (g_entryCount >= MAX_ENTRIES) {
        AppendParseErr(lineNo, L"已达 %d 条上限，后续内容未解析", MAX_ENTRIES);
    }
    free(wbuf);
    return TRUE;
}

static void FillMatch(int n, const Entry *e)
{
    wcsncpy(g_matches[n],       e->key,  MAX_INPUT - 1);
    wcsncpy(g_matchFullPaths[n],e->cmd,  MAX_INPUT - 1);
    wcsncpy(g_matchCmds[n],     e->cmd,  MAX_INPUT - 1);
    wcsncpy(g_matchDescs[n],    e->desc, MAX_INPUT - 1);
    g_matches[n][MAX_INPUT-1]       = 0;
    g_matchFullPaths[n][MAX_INPUT-1] = 0;
    g_matchCmds[n][MAX_INPUT-1]     = 0;
    g_matchDescs[n][MAX_INPUT-1]    = 0;
    g_matchUseCmds[n] = e->useCmd;
}

static void SearchCommandsOnly(const WCHAR *input, int *outCount)
{
    int n = 0;
    size_t inLen = wcslen(input);

    for (int i = 0; i < g_entryCount && n < g_maxResults; i++) {
        if (g_entries[i].hidden) continue;
        if (_wcsicmp(g_entries[i].key, input) == 0) { FillMatch(n, &g_entries[i]); n++; }
    }
    for (int i = 0; i < g_entryCount && n < g_maxResults; i++) {
        if (g_entries[i].hidden) continue;
        if (_wcsicmp(g_entries[i].key, input) == 0) continue;
        if (_wcsnicmp(g_entries[i].key, input, inLen) == 0) { FillMatch(n, &g_entries[i]); n++; }
    }
    for (int i = 0; i < g_entryCount && n < g_maxResults; i++) {
        if (g_entries[i].hidden) continue;
        if (_wcsicmp(g_entries[i].key, input) == 0) continue;
        if (_wcsnicmp(g_entries[i].key, input, inLen) == 0) continue;
        if (ContainsIgnoreCase(g_entries[i].key, input, inLen)) { FillMatch(n, &g_entries[i]); n++; }
    }
    *outCount = n;
}

static void AppendEverythingResults(HWND hDlg)
{
    if (!g_enableEverything) return;

    WCHAR buf[MAX_INPUT];
    GetDlgItemTextW(hDlg, ID_EDIT, buf, MAX_INPUT);
    TrimW(buf);
    if (!buf[0]) return;
    if (g_matchCount >= g_maxResults) return;
    if (!g_evSetSearch || !g_evQuery || !g_evGetNum || !g_evGetPath) return;

    /* '>' 前缀 = 整串直通 Everything，无任何二次加工 */
    const WCHAR *query = buf;
    BOOL forceQuery = FALSE;
    if (buf[0] == L'>') {
        query = buf + 1;
        while (*query == L' ' || *query == L'\t') query++;
        if (!*query) return;
        forceQuery = TRUE;
    }

    /* 非 '>' 透传且带空格：维持原行为，不搜文件，避免噪音 */
    if (!forceQuery && (wcschr(buf, L' ') || wcschr(buf, L'\t'))) return;

    WCHAR q[MAX_INPUT * 4] = { 0 };
    WCHAR extStr[MAX_INPUT + 16] = { 0 };
    WCHAR exQuery[MAX_INPUT * 2] = { 0 };

    if (!forceQuery && g_extensions[0]) {
        wsprintfW(extStr, L" ext:%s", g_extensions);
    }

    if (g_excludes[0]) {
        WCHAR temp[MAX_INPUT * 2];
        wcsncpy(temp, g_excludes, MAX_INPUT * 2 - 1);
        WCHAR *p = temp;
        while (p && *p) {
            WCHAR *semi = wcschr(p, L';');
            if (semi) *semi = 0;
            TrimW(p);
            if (*p) {
                wcsncat(exQuery, L" !\"", MAX_INPUT * 2 - wcslen(exQuery) - 1);
                wcsncat(exQuery, p, MAX_INPUT * 2 - wcslen(exQuery) - 1);
                if (p[wcslen(p) - 1] != L'\\') {
                    wcsncat(exQuery, L"\\", MAX_INPUT * 2 - wcslen(exQuery) - 1);
                }
                wcsncat(exQuery, L"\"", MAX_INPUT * 2 - wcslen(exQuery) - 1);
            }
            if (!semi) break;
            p = semi + 1;
        }
    }

    _snwprintf(q, MAX_INPUT * 4 - 1, L"%s%s%s", query, extStr, exQuery);
    q[MAX_INPUT * 4 - 1] = 0;

    g_evSetSearch(q);
    if (g_evSetSort) g_evSetSort(EV_SORT_RUN_COUNT_DESC);
    if (g_evSetMax)  g_evSetMax((DWORD)(g_maxResults - g_matchCount));
    if (!g_evQuery(TRUE)) return;

    DWORD cnt = g_evGetNum();
    if (cnt == 0) return;

    HWND hList = GetDlgItem(hDlg, ID_LIST);
    int oldCount = g_matchCount;
    int n = g_matchCount;

    for (DWORD i = 0; i < cnt && n < g_maxResults; i++) {
        WCHAR fullPath[MAX_INPUT];
        g_evGetPath(i, fullPath, MAX_INPUT);
        fullPath[MAX_INPUT - 1] = 0;
        wcsncpy(g_matchFullPaths[n], fullPath, MAX_INPUT - 1);
        g_matchFullPaths[n][MAX_INPUT - 1] = 0;

        WCHAR *fileName = wcsrchr(fullPath, L'\\');
        if (fileName) fileName++;
        else          fileName = fullPath;
        wcsncpy(g_matches[n], fileName, MAX_INPUT - 1);
        g_matches[n][MAX_INPUT - 1] = 0;

        g_matchCmds[n][0]  = 0;
        g_matchDescs[n][0] = 0;
        g_matchUseCmds[n]  = FALSE;
        n++;
    }
    g_matchCount = n;

    if (n > oldCount) {
        for (int i = oldCount; i < n; i++) {
            int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)L"");
            if (idx >= 0) SendMessageW(hList, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
        }
        if (g_autoSelectFirst && g_selected < 0) {
            g_selected = 0;
            SendMessageW(hList, LB_SETCURSEL, 0, 0);
        }
        ShowList(hDlg, TRUE);
        UpdateStatusBar(hDlg);
    }
}

static union { BYTE b[4096]; DWORD d; } g_tpl;
static BYTE *g_p;

static void PW(WORD v)
{
    if (g_p + 2 > g_tpl.b + sizeof(g_tpl.b)) { g_tplOverflow = TRUE; return; }
    memcpy(g_p, &v, 2); g_p += 2;
}
static void PD(DWORD v)
{
    if (g_p + 4 > g_tpl.b + sizeof(g_tpl.b)) { g_tplOverflow = TRUE; return; }
    memcpy(g_p, &v, 4); g_p += 4;
}
static void PS(const WCHAR *s)
{
    size_t n = (wcslen(s) + 1) * sizeof(WCHAR);
    if (g_p + n > g_tpl.b + sizeof(g_tpl.b)) { g_tplOverflow = TRUE; return; }
    memcpy(g_p, s, n); g_p += n;
}
static void PA(void)
{
    while (((ULONG_PTR)g_p) & 3) {
        if (g_p + 1 > g_tpl.b + sizeof(g_tpl.b)) { g_tplOverflow = TRUE; return; }
        *g_p++ = 0;
    }
}

static void BuildTemplate(void)
{
    g_tplOverflow = FALSE;
    g_p = g_tpl.b;
    PD(WS_POPUP | DS_CENTER | DS_SETFONT);
    PD(WS_EX_TOOLWINDOW);
    PW(3);
    PW(0); PW(0);
    PW(206); PW(26);
    PW(0);
    PW(0);
    PS(L"QuickRun");
    PW(9);
    PS(L"Microsoft YaHei");

    PA();
    PD(WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL);
    PD(0);
    PW(5); PW(4); PW(196); PW(18);
    PW(ID_EDIT);
    PW(0xFFFF); PW(0x0081);
    PS(L"");
    PW(0);

    PA();
    PD(WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_HASSTRINGS);
    PD(WS_EX_CLIENTEDGE);
    PW(0); PW(22); PW(206); PW(128);
    PW(ID_LIST);
    PW(0xFFFF); PW(0x0083);
    PS(L"");
    PW(0);

    PA();
    PD(WS_CHILD | BS_DEFPUSHBUTTON);
    PD(0);
    PW(0); PW(0); PW(1); PW(1);
    PW(IDOK);
    PW(0xFFFF); PW(0x0080);
    PS(L"OK");
    PW(0);
}

static void ShowList(HWND hDlg, BOOL show)
{
    if (g_listVisible == show) return;
    g_listVisible = show;

    HWND hList = GetDlgItem(hDlg, ID_LIST);
    RECT rc;
    GetWindowRect(hDlg, &rc);
    int newH = show ? g_expandedH : g_initH;

    SetWindowPos(hDlg, NULL, 0, 0, rc.right - rc.left, newH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    HRGN hRgn = CreateRoundRectRgn(0, 0, rc.right - rc.left + 1, newH + 1, 5, 5);
    SetWindowRgn(hDlg, hRgn, TRUE);

    ShowWindow(hList, show ? SW_SHOW : SW_HIDE);
    RedrawWindow(hDlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void UpdateStatusBar(HWND hDlg)
{
    if (g_selected >= 0 && g_selected < g_matchCount) {
        wcsncpy(g_statusText, g_matchFullPaths[g_selected], MAX_INPUT - 1);
    } else {
        wcsncpy(g_statusText, L"↑↓ 选择 | Enter 执行 | Alt+Enter 原样执行 | F1 帮助", MAX_INPUT - 1);
    }
    g_statusText[MAX_INPUT - 1] = 0;
    InvalidateRect(hDlg, NULL, TRUE);
}

static void ResetSearchState(HWND hDlg)
{
    HWND hList = GetDlgItem(hDlg, ID_LIST);
    KillTimer(hDlg, IDT_EVDEBOUNCE);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    g_matchCount = 0; g_selected = -1;
    ShowList(hDlg, FALSE);
    UpdateStatusBar(hDlg);
}

static void UpdateSearch(HWND hDlg)
{
    WCHAR buf[MAX_INPUT];
    GetDlgItemTextW(hDlg, ID_EDIT, buf, MAX_INPUT);
    HWND hList = GetDlgItem(hDlg, ID_LIST);

    TrimW(buf);
    if (!buf[0]) {
        ResetSearchState(hDlg);
        return;
    }

    /* 截断空格及后面的参数，只用前面的词去搜索列表 */
    WCHAR searchKey[MAX_INPUT];
    WCHAR *sp = wcschr(buf, L' ');
    if (!sp) sp = wcschr(buf, L'\t');
    if (sp) {
        size_t klen = (size_t)(sp - buf);
        if (klen >= MAX_INPUT) klen = MAX_INPUT - 1;
        wcsncpy(searchKey, buf, klen);
        searchKey[klen] = 0;
        TrimW(searchKey);
    } else {
        wcscpy(searchKey, buf);
    }

    /* 如果只剩下空格（比如只输入了一个空格），直接重置 */
    if (!searchKey[0]) {
        ResetSearchState(hDlg);
        return;
    }

    SearchCommandsOnly(searchKey, &g_matchCount);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_matchCount; i++) {
        int idx = (int)SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)L"");
        if (idx >= 0) SendMessageW(hList, LB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);
    }
    ShowList(hDlg, g_matchCount > 0);
    if (g_matchCount > 0) {
        if (g_autoSelectFirst) {
            g_selected = 0;
            SendMessageW(hList, LB_SETCURSEL, 0, 0);
        } else {
            g_selected = -1;
            SendMessageW(hList, LB_SETCURSEL, (WPARAM)-1, 0);
        }
    } else {
        g_selected = -1;
    }
    UpdateStatusBar(hDlg);

    KillTimer(hDlg, IDT_EVDEBOUNCE);
    SetTimer(hDlg, IDT_EVDEBOUNCE, g_debounceMs, NULL);
}

static void ExecuteInput(HWND hDlg, BOOL forceFallback)
{
    WCHAR buf[MAX_INPUT];
    GetDlgItemTextW(hDlg, ID_EDIT, buf, MAX_INPUT);
    TrimW(buf);
    if (!buf[0]) return;

    if (!forceFallback) {
        /* 只要选中了列表，就执行列表项（带空格则切分参数传入） */
        if (g_selected >= 0 && g_selected < g_matchCount) {
            WCHAR *sp = wcschr(buf, L' ');
            if (!sp) sp = wcschr(buf, L'\t');
            g_argCount = 0;
            if (sp) {
                SplitArgs(sp + 1, &g_argCount);
            }

            if (g_matchCmds[g_selected][0] == 0) {
                HINSTANCE hi = ShellExecuteW(NULL, L"open", g_matchFullPaths[g_selected], NULL, NULL, SW_SHOWNORMAL);
                if ((INT_PTR)hi <= 32) {
                    WCHAR msg[800];
                    wsprintfW(msg, L"打开失败：\n%s\n错误码：%d", g_matchFullPaths[g_selected], (int)(INT_PTR)hi);
                    g_inModal = TRUE;
                    MessageBoxW(hDlg, msg, L"错误", MB_ICONERROR);
                    g_inModal = FALSE;
                }
            } else {
                wcsncpy(g_input, g_matches[g_selected], MAX_INPUT - 1);
                g_input[MAX_INPUT - 1] = 0;
                RunCommandEx(g_matchCmds[g_selected], g_matchDescs[g_selected], g_matchUseCmds[g_selected]);
            }
            return;
        }
        
        /* 没选中列表：不管是带不带空格，绝对不查配置，去执行原文 */
    }

    /* '>' 透传查询在未选中列表项（通常无结果）时，
       不 fallback 到 cmd，避免 > 被 cmd 当成重定向创建空文件 */
    if (!forceFallback && buf[0] == L'>' &&
        (g_selected < 0 || g_selected >= g_matchCount)) {
        ShowQuickToast(hDlg, L"⚠️ 未找到匹配文件", 2);
        return;
    }

    if (g_noFallback) {
        WCHAR msg[600];
        wsprintfW(msg, L"没有找到与“%s”匹配的命令。", buf);
        g_inModal = TRUE;
        MessageBoxW(hDlg, msg, L"提示", MB_ICONINFORMATION);
        g_inModal = FALSE;
        return;
    }

    HINSTANCE hi = ShellExecuteW(NULL, L"open", buf, NULL, NULL, SW_SHOWNORMAL);
    if ((INT_PTR)hi <= 32) {
        WCHAR cmdLine[2048];
        wsprintfW(cmdLine, L"cmd /c %s || timeout /t 2 >nul", buf);
        STARTUPINFOW si = { 0 };
        PROCESS_INFORMATION pi = { 0 };
        si.cb = sizeof(si);
        if (!CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            WCHAR msg[600];
            wsprintfW(msg, L"无法执行：%s\n错误码：%d", buf, (int)(INT_PTR)hi);
            g_inModal = TRUE;
            MessageBoxW(hDlg, msg, L"提示", MB_ICONINFORMATION);
            g_inModal = FALSE;
        } else {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
    }
}

static void ForceForeground(HWND hWnd)
{
    if (!hWnd) return;
    g_activating = TRUE;
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);

    HWND hCurFg = GetForegroundWindow();
    if (hCurFg == hWnd) {
        HWND hEdit = GetDlgItem(hWnd, ID_EDIT);
        if (hEdit) { SetFocus(hEdit); SendMessageW(hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1); }
        g_activating = FALSE;
        return;
    }

    DWORD fgThread = GetWindowThreadProcessId(hCurFg, NULL);
    DWORD myThread = GetCurrentThreadId();
    BOOL attached = FALSE;

    if (fgThread && fgThread != myThread) attached = AttachThreadInput(fgThread, myThread, TRUE);
    SetForegroundWindow(hWnd);
    BringWindowToTop(hWnd);
    if (attached) AttachThreadInput(fgThread, myThread, FALSE);

    HWND hEdit = GetDlgItem(hWnd, ID_EDIT);
    if (hEdit) { SetFocus(hEdit); SendMessageW(hEdit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1); }
    g_activating = FALSE;
}

static void FormatTrayTip(WCHAR *tip, int cap)
{
    WCHAR dataStr[64], hkStr[MAX_INPUT + 16], evStr[64];

    /* 数据状态 */
    if (!g_entryCount && g_parseErrCount == 0) {
        wcsncpy(dataStr, L"数据: 0 条", 63);
    } else if (g_parseErrCount > 0) {
        wsprintfW(dataStr, L"数据: %d 条 (%d处错误)", g_entryCount, g_parseErrCount);
    } else {
        wsprintfW(dataStr, L"数据: %d 条 (√)", g_entryCount);
    }

    /* 热键状态 */
    if (g_hotkeyRegistered) {
        wsprintfW(hkStr, L"热键: %s", g_activeHotkeyStr);
    } else if (g_hotkeyStr[0]) {
        wcsncpy(hkStr, L"热键: ⚠️ 注册失败", 63);
    } else {
        wcsncpy(hkStr, L"热键: 未设置", 63);
    }

    /* Everything 状态 */
    if (!g_enableEverything) {
        wcsncpy(evStr, L"Everything: 已禁用", 63);
    } else if (g_evQuery) {
        wcsncpy(evStr, L"Everything: 已连接", 63);
    } else {
        wcsncpy(evStr, L"Everything: 未运行", 63);
    }

    _snwprintf(tip, cap - 1, L"QuickRun v" QR_VERSION L"\n%s\n%s\n%s", hkStr, dataStr, evStr);
    tip[cap - 1] = 0;
}

static void UpdateTrayTip(HWND hDlg)
{
    if (!g_showTrayIcon) return;
    NOTIFYICONDATAW nid = { 0 };
    nid.cbSize = sizeof(nid);
    nid.hWnd = hDlg;
    nid.uID = 1;
    nid.uFlags = NIF_TIP;

    FormatTrayTip(nid.szTip, sizeof(nid.szTip) / sizeof(WCHAR));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void AddTrayIcon(HWND hDlg)
{
    if (!g_showTrayIcon) return;
    NOTIFYICONDATAW nid = { 0 };
    nid.cbSize = sizeof(nid);
    nid.hWnd = hDlg;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = g_hIconSmall ? g_hIconSmall : LoadIconW(NULL, IDI_APPLICATION);
    FormatTrayTip(nid.szTip, sizeof(nid.szTip) / sizeof(WCHAR));
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTrayIcon(HWND hDlg)
{
    NOTIFYICONDATAW nid = { 0 };
    nid.cbSize = sizeof(nid);
    nid.hWnd = hDlg;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ResetUI(HWND hDlg, BOOL doShow)
{
    HWND hEdit = GetDlgItem(hDlg, ID_EDIT);
    HWND hList = GetDlgItem(hDlg, ID_LIST);

    KillTimer(hDlg, IDT_EVDEBOUNCE);

    g_matchCount = 0; g_selected = -1; g_argCount = 0; g_input[0] = 0; g_statusText[0] = 0;
    SendMessageW(hList, LB_SETCURSEL, (WPARAM)-1, 0);
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);
    SetDlgItemTextW(hDlg, ID_EDIT, L"");

    ShowList(hDlg, FALSE);
    UpdateStatusBar(hDlg);

    if (!doShow) { ShowWindow(hDlg, SW_HIDE); return; }

    ForceForeground(hDlg);
    if (g_prefill[0]) {
        SetDlgItemTextW(hDlg, ID_EDIT, g_prefill);
        SendMessageW(hEdit, EM_SETSEL, 0, 0);
        g_prefill[0] = 0;
        UpdateSearch(hDlg);
    }
}

static HWND   g_hQuickToast = NULL;
static WCHAR  g_quickToastText[256] = {0};
static int    g_toastLevel = 0;

static LRESULT CALLBACK QuickToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hwnd, &rc);
        COLORREF bg = (g_toastLevel == 1) ? RGB(190, 40, 40) :
                      (g_toastLevel == 2) ? RGB(230, 150, 30) :
                                            RGB(30, 160, 60);
        HBRUSH hbr = CreateSolidBrush(bg);
        FillRect(hdc, &rc, hbr);
        DeleteObject(hbr);
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        HFONT f = CreateFontW(-(int)(18 * g_dpiScale), 0, 0, 0, FW_BOLD, 0, 0, 0, GB2312_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei");
        HFONT old = (HFONT)SelectObject(hdc, f);
        rc.left += 10; rc.right -= 10;
        DrawTextW(hdc, g_quickToastText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(hdc, old); DeleteObject(f);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_hQuickToast = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* 帮助窗口相关 */
#define IDR_HELP_TEXT 1005
static HWND g_hHelpWnd = NULL;
static HWND g_hMainDlg = NULL;
static HFONT g_hFontHelp = NULL;
static BOOL g_helpStandalone = FALSE;

static WNDPROC g_oldHelpEditProc = NULL;

static LRESULT CALLBACK HelpEditSubclassProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        /* 按下 Esc 时，向父窗口发送关闭消息 */
        PostMessageW(GetParent(hWnd), WM_CLOSE, 0, 0);
        return 0;
    }
    return CallWindowProcW(g_oldHelpEditProc, hWnd, msg, wp, lp);
}

static HBRUSH g_hHelpBgBrush = NULL; // 帮助窗口背景画刷

static LRESULT CALLBACK HelpWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static HWND hEdit = NULL;
    switch (msg) {
    case WM_CREATE: {
        /* 创建只读多行文本控件，带垂直滚动条 */
        hEdit = CreateWindowExW(0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            15, 15, 100, 100, hwnd, (HMENU)1, GetModuleHandleW(NULL), NULL);
        
        if (g_hFontHelp) SendMessageW(hEdit, WM_SETFONT, (WPARAM)g_hFontHelp, TRUE);
        
        /* 拦截 Edit 控件的按键，实现 ESC 关闭 */
        g_oldHelpEditProc = (WNDPROC)SetWindowLongPtrW(hEdit, GWLP_WNDPROC, (LONG_PTR)HelpEditSubclassProc);
        return 0;
    }
    case WM_CTLCOLORSTATIC: { // 只读 Edit 控件会发送此消息
        HDC hdc = (HDC)wp;
        /* 设置非常淡的米黄色：RGB(253, 250, 240) */ 
        SetBkColor(hdc, RGB(245, 245, 233));
        SetTextColor(hdc, RGB(40, 40, 40)); // 字体用深灰色，比纯黑柔和
        if (!g_hHelpBgBrush) {
            g_hHelpBgBrush = CreateSolidBrush(RGB(245, 245, 233));
        }
        return (LRESULT)g_hHelpBgBrush;
    }
    case WM_SIZE: {
        if (hEdit) {
            MoveWindow(hEdit, 15, 15, LOWORD(lp) - 30, HIWORD(lp) - 30, TRUE);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = (LONG)(400 * g_dpiScale);
        mmi->ptMinTrackSize.y = (LONG)(300 * g_dpiScale);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_hHelpBgBrush) {
            DeleteObject(g_hHelpBgBrush);
            g_hHelpBgBrush = NULL;
        }
        if (g_hFontHelp) {
            DeleteObject(g_hFontHelp);
            g_hFontHelp = NULL;
        }
        g_hHelpWnd = NULL;
        g_inModal = FALSE;
        if (g_helpStandalone) {
            PostQuitMessage(0);
        } else if (g_hMainDlg && IsWindowVisible(g_hMainDlg)) {
            SetForegroundWindow(g_hMainDlg);
            SetFocus(GetDlgItem(g_hMainDlg, ID_EDIT));
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ShowHelpWindow(HWND hParent, BOOL standalone)
{
    g_helpStandalone = standalone;
    if (g_hHelpWnd && IsWindow(g_hHelpWnd)) {
        ShowWindow(g_hHelpWnd, SW_SHOW);
        SetForegroundWindow(g_hHelpWnd);
        HWND hEdit = GetDlgItem(g_hHelpWnd, 1);
        if (hEdit) SetFocus(hEdit);
        return;
    }

    /* 1. 从 exe 资源中加载文本 */
    HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_HELP_TEXT), RT_RCDATA);
    if (!hRes) {
        MessageBoxW(hParent, L"未找到内置的帮助资源！\n请确保编译时包含了 app.rc。", L"错误", MB_ICONERROR);
        return;
    }
    
    HGLOBAL hMem = LoadResource(NULL, hRes);
    DWORD size = SizeofResource(NULL, hRes);
    const char *pData = (const char *)LockResource(hMem);
    if (!pData || size == 0) return;

    /* 2. 将 UTF-8 文本转为宽字符 */
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, pData, size, NULL, 0);
    WCHAR *wbuf = NULL;
    if (wlen > 0) {
        wbuf = (WCHAR *)malloc((wlen + 1) * sizeof(WCHAR));
        if (wbuf) {
            MultiByteToWideChar(CP_UTF8, 0, pData, size, wbuf, wlen);
            wbuf[wlen] = 0;
            if (wbuf[0] == 0xFEFF) memmove(wbuf, wbuf + 1, wlen * sizeof(WCHAR)); /* 去掉 BOM */
        }
    }

    /* 3. 注册窗口类（只注册一次，避免重复创建背景画刷） */
    static BOOL s_helpClsReg = FALSE;
    static HBRUSH s_helpClsBrush = NULL;
    if (!s_helpClsReg) {
        if (!s_helpClsBrush) s_helpClsBrush = CreateSolidBrush(RGB(245, 245, 233));
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = HelpWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = s_helpClsBrush;
        wc.lpszClassName = L"QRHelpClass";
        if (RegisterClassW(&wc)) {
            s_helpClsReg = TRUE;
        } else {
            if (s_helpClsBrush) { DeleteObject(s_helpClsBrush); s_helpClsBrush = NULL; }
        }
    }

    if (g_hFontHelp) DeleteObject(g_hFontHelp);
    g_hFontHelp = CreateFontW(-(int)(18 * g_dpiScale), 0, 0, 0, FW_NORMAL, 0, 0, 0, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");

    /* 4. 计算位置和大小（排除任务栏） */
    RECT rcWork;
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &rcWork, 0)) {
        rcWork.left = 0;
        rcWork.top = 0;
        rcWork.right = GetSystemMetrics(SM_CXSCREEN);
        rcWork.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    
    int screenW = rcWork.right - rcWork.left;
    int screenH = rcWork.bottom - rcWork.top;

    /* 设定基础窗口大小 1000x750（4:3 黄金阅读比例） 850*630*/
    int winW = (int)(700 * g_dpiScale);
    int winH = (int)(570 * g_dpiScale);

    /* 如果屏幕工作区不够大，则自动缩减，留出 50 像素边距 */
    if (winW > screenW - (int)(100 * g_dpiScale)) winW = screenW - (int)(100 * g_dpiScale);
    if (winH > screenH - (int)(100 * g_dpiScale)) winH = screenH - (int)(100 * g_dpiScale);

    int x = rcWork.left + (screenW - winW) / 2;
    int y = rcWork.top + (screenH - winH) / 2;

    /* 5. 创建窗口（拼接版本号） */
    WCHAR title[128];
    wsprintfW(title, L"QuickRun v%s 帮助文档", QR_VERSION);
    g_hHelpWnd = CreateWindowExW(WS_EX_APPWINDOW, L"QRHelpClass", title,
        WS_OVERLAPPEDWINDOW, x, y, winW, winH, NULL, NULL, GetModuleHandleW(NULL), NULL);

    if (g_hHelpWnd) {
        HWND hEdit = GetDlgItem(g_hHelpWnd, 1);
        if (hEdit && wbuf) {
            SetWindowTextW(hEdit, wbuf);
        }
        ShowWindow(g_hHelpWnd, SW_SHOW);
        
        /* 强行激活帮助窗口，并把焦点交给只读文本框 */
        SetForegroundWindow(g_hHelpWnd);
        if (hEdit) SetFocus(hEdit);
    }

    if (wbuf) free(wbuf);
}

static void ShowQuickToast(HWND hDlg, const WCHAR *text, int level)
{
    if (g_toastDuration <= 0) return;
    if (g_hQuickToast && IsWindow(g_hQuickToast)) {
        DestroyWindow(g_hQuickToast);
        g_hQuickToast = NULL;
    }

    wcsncpy(g_quickToastText, text, 255);
    g_quickToastText[255] = 0;
    g_toastLevel = level;

    static BOOL s_clsReg = FALSE;
    if (!s_clsReg) {
        WNDCLASSW wc = {0};
        wc.lpfnWndProc   = QuickToastProc;
        wc.hInstance     = GetModuleHandleW(NULL);
        wc.hbrBackground = NULL;
        wc.lpszClassName = L"QRQuickToast";
        RegisterClassW(&wc);
        s_clsReg = TRUE;
    }

    RECT rcParent;
    GetWindowRect(hDlg, &rcParent);
    int w = (int)(420 * g_dpiScale), h = (int)(44 * g_dpiScale);
    int x = rcParent.left + ((rcParent.right - rcParent.left) - w) / 2;
    int y = rcParent.bottom + (int)(8 * g_dpiScale);

    HMONITOR hMon = MonitorFromWindow(hDlg, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        if (y + h > mi.rcWork.bottom - (int)(40 * g_dpiScale))
            y = rcParent.top - h - (int)(8 * g_dpiScale);
    } else {
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (y + h > screenH - 40) y = rcParent.top - h - 8;
    }

    g_hQuickToast = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        L"QRQuickToast", L"", WS_POPUP,
        x, y, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);

    if (!g_hQuickToast) return;

    SetLayeredWindowAttributes(g_hQuickToast, 0, 235, LWA_ALPHA);
    ShowWindow(g_hQuickToast, SW_SHOWNOACTIVATE);
    InvalidateRect(g_hQuickToast, NULL, TRUE);
    SetTimer(g_hQuickToast, 1, g_toastDuration, NULL);
}

static const WCHAR *HotkeyStatusStr(void)
{
    switch (g_hotkeyStatus) {
    case HOTKEY_OK:            return L"已注册";
    case HOTKEY_DISABLED:      return L"未设置（禁用）";
    case HOTKEY_PARSE_FAIL:    return L"格式错误";
    case HOTKEY_REGISTER_FAIL: return L"注册失败（冲突）";
    }
    return L"未知";
}

static void ExportDiagnostics(HWND hDlg)
{
    WCHAR exe[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    SYSTEMTIME st;
    GetLocalTime(&st);

    WCHAR text[8192];
    int n = 0;
    n += wsprintfW(text + n,
        L"QuickRun " QR_VERSION L" 诊断信息\r\n"
        L"时间: %04d-%02d-%02d %02d:%02d:%02d\r\n"
        L"执行文件: %s\r\n"
        L"数据文件: %s\r\n"
        L"配置文件: %s\r\n"
        L"状态文件: %s\r\n"
        L"命令条目: %d\r\n"
        /* [FIX 12] 输出数据文件是否被截断 */
        L"数据文件截断: %s\r\n"
        L"热键: %s（%s，配置：%s）\r\n"
        L"开机自启: %s\r\n"
        L"Everything DLL: %s\r\n"
        L"Everything SDK: %s\r\n"
        L"窗口: %s\r\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        exe, g_dataFile, g_iniPath, g_statePath, g_entryCount,
        g_dataFileTruncated ? L"是（超过 4MB，已丢弃末尾不完整行）" : L"否",
        g_activeHotkeyStr[0] ? g_activeHotkeyStr : L"（无）",
        HotkeyStatusStr(),
        g_hotkeyStr[0] ? g_hotkeyStr : L"（空）",
        IsAutoStartEnabled() ? L"已启用" : L"未启用",
        g_hEv ? L"已加载" : L"未加载",
        g_evQuery ? L"已连接" : L"未连接（Everything 未运行或 DLL 缺失）",
        IsWindowVisible(hDlg) ? L"可见" : L"隐藏");

    /* 新增：数据格式校验明细 */
    if ((sizeof(text) / sizeof(WCHAR)) - n < 256) {
        n += wsprintfW(text + n, L"\r\n数据文件格式问题：%d 处（明细省略）\r\n",
                       g_parseErrCount);
    } else if (g_parseErrCount > 0) {
        n += wsprintfW(text + n, L"\r\n数据文件格式问题（共 %d 处）:\r\n",
                       g_parseErrCount);
        {
            size_t cap = (sizeof(text) / sizeof(WCHAR)) - n - 1;
            wcsncat(text + n, g_parseErrMsg, cap);
            n += (int)wcslen(text + n);
        }
    } else {
        n += wsprintfW(text + n, L"\r\n数据文件格式：正确\r\n");
    }

    BOOL copied = FALSE;
    if (OpenClipboard(hDlg)) {
        EmptyClipboard();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(WCHAR));
        if (h) {
            WCHAR *p = (WCHAR *)GlobalLock(h);
            if (p) {
                memcpy(p, text, n * sizeof(WCHAR));
                p[n] = 0;
                GlobalUnlock(h);
                if (SetClipboardData(CF_UNICODETEXT, h)) copied = TRUE;
                else GlobalFree(h);
            } else GlobalFree(h);
        }
        CloseClipboard();
    }

    if (copied) {
        ShowQuickToast(hDlg, L"✅ 诊断信息已复制到剪贴板", FALSE);
    } else {
        g_inModal = TRUE;
        MessageBoxW(hDlg, text, L"QuickRun 诊断（剪贴板失败）", MB_ICONINFORMATION);
        g_inModal = FALSE;
    }
}

/* A1：重载配置后按新字号/行高重算当前 DPI 下的像素布局。
 * 只更新当前像素布局与 g_initH/g_expandedH/g_extraInitH/g_rcEdit/g_rcList，
 * 不更新 g_original*（A2 才做）。因此重载后若立即跨 DPI 拖窗口，
 * WM_DPICHANGED 仍按冷启动时的旧字号基准缩放；重载后不跨 DPI 则布局与冷启动一致。 */
static void ApplyLayoutAfterConfigChange(HWND hDlg)
{
    HWND hEdit = GetDlgItem(hDlg, ID_EDIT);
    HWND hList = GetDlgItem(hDlg, ID_LIST);
    if (!hEdit || !hList) return;

    int margin = (int)(8 * g_dpiScale);

    /* 宽度沿用当前值：A1 不改宽度 */
    int baseEditW = g_rcEdit.right - g_rcEdit.left;
    if (baseEditW <= 0) return;
    
    /* 强制主窗口宽度 = 输入框宽度 + 左右边距，彻底杜绝任何右侧留白 */
    g_initW = baseEditW + 2 * margin;
    if (baseEditW <= 0) return;

    /* 编辑框高度：复刻 WM_INITDIALOG 的逻辑 */
    int newEditH = (int)(g_fontSizeEdit * g_dpiScale * 1.25);
    HDC hdc = GetDC(hDlg);
    if (hdc && g_hFontEdit) {
        HFONT hOld = (HFONT)SelectObject(hdc, g_hFontEdit);
        TEXTMETRICW tm;
        if (GetTextMetricsW(hdc, &tm)) {
            newEditH = tm.tmAscent + tm.tmDescent + (int)(2 * g_dpiScale);
        }
        SelectObject(hdc, hOld);
    }
    if (hdc) ReleaseDC(hDlg, hdc);
    if (newEditH < (int)(28 * g_dpiScale)) newEditH = (int)(28 * g_dpiScale);

    int boxExpandY = (int)(4 * g_dpiScale);
    int editTop = margin + boxExpandY;
    int gap = (int)(6 * g_dpiScale);
    int newListTop = margin + newEditH + (boxExpandY * 2) + gap;

    int targetListH = (int)(g_listItemHeight * 9 * g_dpiScale) + GetSystemMetrics(SM_CYEDGE) * 2;

    int statusBarH = (int)(g_fontSizeUI * g_dpiScale * 1.6);
    if (statusBarH < 24) statusBarH = 24;

    int newInitH     = editTop + newEditH + boxExpandY + margin;
    int newExtraH    = gap + targetListH + statusBarH;
    int newExpandedH = newInitH + newExtraH;

    SetWindowPos(hEdit, NULL, margin, editTop, baseEditW, newEditH,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(hList, NULL, (int)(1 * g_dpiScale), newListTop,
                 g_initW - (int)(2 * g_dpiScale), targetListH,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    int winH = g_listVisible ? newExpandedH : newInitH;
    SetWindowPos(hDlg, NULL, 0, 0, g_initW, winH,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    HRGN hRgn = CreateRoundRectRgn(0, 0, g_initW + 1, winH + 1, 5, 5);
    SetWindowRgn(hDlg, hRgn, TRUE);

    g_initH      = newInitH;
    g_extraInitH = newExtraH;
    g_expandedH  = newExpandedH;

    /* 刷新 g_rcEdit / g_rcList 供 WM_ERASEBKGND 画白框 */
    GetWindowRect(hEdit, &g_rcEdit);
    MapWindowPoints(HWND_DESKTOP, hDlg, (POINT*)&g_rcEdit, 2);
    GetWindowRect(hList, &g_rcList);
    MapWindowPoints(HWND_DESKTOP, hDlg, (POINT*)&g_rcList, 2);

    /* A2：重载视为一次全新的“冷启动”基准，直接重置 g_initDpi 并保存当前物理像素。
     * 这样彻底消除了基于历史 DPI 缩放带来的取整误差和状态栏保底丢失问题。 */
    UINT currentDpi = GetDpiForWindowSafe(hDlg);
    if (currentDpi) {
        g_initDpi = currentDpi;
        g_dpiScale = currentDpi / 96.0;
    }

    g_originalRcEdit = g_rcEdit;

    InvalidateRect(hDlg, NULL, TRUE);
    InvalidateRect(hEdit, NULL, TRUE);
    InvalidateRect(hList, NULL, TRUE);
}

static void ReloadAll(HWND hDlg)
{
    BOOL oldShowTray = g_showTrayIcon;

    LoadConfig();
    if (g_forceNoTray) g_showTrayIcon = FALSE;

    /* 重载时先同步一次当前所在显示器的 DPI，防止切屏后立即重载使用旧 DPI 字体 */
    UINT currentDpi = GetDpiForWindowSafe(hDlg);
    if (currentDpi) {
        g_dpiScale = currentDpi / 96.0;
    }

    /* 重载时立即应用新的字体和行高 */
    HWND hEdit = GetDlgItem(hDlg, ID_EDIT);
    HWND hList = GetDlgItem(hDlg, ID_LIST);
    if (hEdit && hList) {
        if (g_hFontUI)   DeleteObject(g_hFontUI);
        if (g_hFontEdit) DeleteObject(g_hFontEdit);
        g_hFontUI   = CreateFontW(-(int)(g_fontSizeUI * g_dpiScale), 0, 0, 0, FW_BOLD, 0, 0, 0, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
        g_hFontEdit = CreateFontW(-(int)(g_fontSizeEdit * g_dpiScale), 0, 0, 0, FW_BOLD, 0, 0, 0, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
        SendMessageW(hList, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(hEdit, WM_SETFONT, (WPARAM)g_hFontEdit, TRUE);
        SendMessageW(hList, LB_SETITEMHEIGHT, 0, (LPARAM)(int)(g_listItemHeight * g_dpiScale));

        /* 核心修复：无论是否修改了字号，重载都强制重新应用一次绝对布局，以修复跨 DPI 切换导致的残留误差 */
        ApplyLayoutAfterConfigChange(hDlg);
    }

    RegisterHotkeyFromIni(hDlg);

    /* 失联保护：热键未生效且托盘被隐藏时，强制保留托盘 */
    if (!g_forceNoTray && !g_showTrayIcon && !g_hotkeyRegistered) {
        g_showTrayIcon = TRUE;
        g_reloadHotkeyWarning = TRUE;
        if (g_hotkeyStatus == HOTKEY_PARSE_FAIL)
            ShowQuickToast(hDlg, L"⚠️ 热键格式错误且无可用热键，已强制保留托盘图标", TRUE);
        else
            ShowQuickToast(hDlg, L"⚠️ 热键未生效，已强制保留托盘图标", TRUE);
    }

    if (!g_hEv) EvLoad();

    if (oldShowTray != g_showTrayIcon) {
        if (g_showTrayIcon) AddTrayIcon(hDlg);
        else                RemoveTrayIcon(hDlg);
    }

    if (LoadDataFile(g_dataFile)) {
        if (IsWindowVisible(hDlg)) UpdateSearch(hDlg);

        /* 热键正常时才播报；热键异常时警告已在前面发出，这里不再覆盖。
           格式问题明细仍在诊断信息里，修好热键后重载会自然显示。 */
        if (g_hotkeyStatus == HOTKEY_OK && !g_reloadHotkeyWarning) {
            if (g_parseErrCount > 0) {
                WCHAR msg[128];
                wsprintfW(msg, L"⚠️ 数据文件有 %d 处格式问题，详见诊断信息", g_parseErrCount);
                ShowQuickToast(hDlg, msg, 2);
            } else {
                WCHAR msg[128 + MAX_INPUT];
                wsprintfW(msg, L"✅ 重载成功：热键 %s，共 %d 条命令",
                    g_activeHotkeyStr[0] ? g_activeHotkeyStr : L"无", g_entryCount);
                ShowQuickToast(hDlg, msg, FALSE);
            }
        }
    } else {
        const WCHAR *name = wcsrchr(g_dataFile, L'\\');
        name = name ? name + 1 : g_dataFile;
        WCHAR msg[MAX_PATH + 64];
        wsprintfW(msg, L"❌ 重载失败：无法读取 %s", name);
        ShowQuickToast(hDlg, msg, TRUE);
    }

    /* 新增：重载后强刷托盘 ToolTip，确保热键/数据/Everything 状态实时更新 */
    UpdateTrayTip(hDlg);

    /* --notray 模式下热键注册失败 → 失联保护：提示并退出 */
    if (g_forceNoTray && !g_hotkeyRegistered && g_hotkeyStr[0]) {
        g_inModal = TRUE;
        MessageBoxW(hDlg,
            L"QuickRun：--no-tray 模式下热键注册失败，程序将退出。\n\n"
            L"请检查 QuickRun.ini 的 [Hotkey] Key 设置。",
            L"QuickRun", MB_ICONERROR);
        g_inModal = FALSE;
        g_quitting = TRUE;
        PostMessageW(hDlg, WM_CLOSE, 0, 0);
    }
}

static LRESULT CALLBACK EditSubclassProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_GETDLGCODE) {
        LRESULT base = CallWindowProcW(g_oldEditProc, hWnd, msg, wp, lp);
        if (lp) {
            MSG *m = (MSG *)lp;
            BOOL isSpecialKey = FALSE;
            if (m->message == WM_SYSKEYDOWN) {
                WPARAM k = m->wParam;
                if ((k >= L'1' && k <= L'9') || (k >= VK_NUMPAD1 && k <= VK_NUMPAD9) || k == L'I' || k == L'K' || k == VK_RETURN) isSpecialKey = TRUE;
            } else if (m->message == WM_SYSCHAR) {
                if ((GetKeyState(VK_MENU) & 0x8000) && m->wParam != L' ') isSpecialKey = TRUE;
            } else if (m->message == WM_KEYDOWN) {
                /* 放行 F1-F5、F9，防止响铃 */
                if (m->wParam == VK_UP || m->wParam == VK_DOWN ||
                    m->wParam == VK_F1 || m->wParam == VK_F2 ||
                    m->wParam == VK_F3 || m->wParam == VK_F4 ||
                    m->wParam == VK_F5 || m->wParam == VK_F9) isSpecialKey = TRUE;
                /* 仅保留现有的 Ctrl+D (打开文件夹) 和 Ctrl+Q (退出) */
                if ((GetKeyState(VK_CONTROL) & 0x8000) && (m->wParam == L'D' || m->wParam == L'Q')) isSpecialKey = TRUE;
            }
            if (isSpecialKey) base |= DLGC_WANTALLKEYS;
        }
        return base;
    }

    if (msg == WM_CHAR) {
          /* 吞掉 Ctrl+@ / Ctrl+D / Ctrl+Q 的 WM_CHAR，避免 Edit 插入控制字符或响铃 */
         if (wp == 0 || wp == 4 || wp == 17) return 0;
    }

    if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
        HWND hList = GetDlgItem(GetParent(hWnd), ID_LIST);
        BOOL alt = (GetKeyState(VK_MENU) & 0x8000) ? TRUE : FALSE;

        if (msg == WM_KEYDOWN) {
            if (wp == VK_F1) { PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(ID_HELP, 0), 0); return 0; }
            if (wp == VK_F2) { PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(ID_DATA, 0), 0); return 0; }
            if (wp == VK_F3) { PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(ID_TRAY_CONFIG, 0), 0); return 0; } /* 新增 F3 */
            if (wp == VK_F4) { PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(ID_RESETPOS, 0), 0); return 0; }
            if (wp == VK_F5) { PostMessageW(GetParent(hWnd), WM_APP_RELOAD, 0, 0); return 0; } /* 新增 F5 */
            if (wp == VK_F9) { PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(ID_TRAY_DIAG, 0), 0); return 0; } /* 新增 F9 */
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == L'D') { PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(ID_OPEN_FOLDER, 0), 0); return 0; }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == L'Q') { g_quitting = TRUE; PostMessageW(GetParent(hWnd), WM_CLOSE, 0, 0); return 0; }
            /* 移除 Ctrl+S 和 Ctrl+R 的映射，交给系统触发响铃 */
        }

        if (alt) {
            if (wp == VK_RETURN) { PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, 1), 0); return 0; }
            int n = -1;
            if (wp >= L'1' && wp <= L'9') n = (int)(wp - L'1');
            else if (wp >= VK_NUMPAD1 && wp <= VK_NUMPAD9) n = (int)(wp - VK_NUMPAD1);
            if (n >= 0 && g_matchCount > 0 && n < g_matchCount) { g_selected = n; PostMessageW(GetParent(hWnd), WM_COMMAND, MAKEWPARAM(IDOK, 0), 0); return 0; }
            if (wp == L'I') wp = VK_UP;
            else if (wp == L'K') wp = VK_DOWN;
        }

        if (wp == VK_UP) {
            if (g_matchCount > 0) {
                int newSel = (g_selected <= 0) ? g_matchCount - 1 : g_selected - 1;
                if (newSel != g_selected) { g_selected = newSel; SendMessageW(hList, LB_SETCURSEL, (WPARAM)g_selected, 0); UpdateStatusBar(GetParent(hWnd)); }
            }
            return 0;
        }
        if (wp == VK_DOWN) {
            if (g_matchCount > 0) {
                int newSel = (g_selected < 0 || g_selected >= g_matchCount - 1) ? 0 : g_selected + 1;
                if (newSel != g_selected) { g_selected = newSel; SendMessageW(hList, LB_SETCURSEL, g_selected, 0); UpdateStatusBar(GetParent(hWnd)); }
            }
            return 0;
        }
    }

    if (msg == WM_SYSCHAR) {
        if (wp >= L'1' && wp <= L'9') { int n = (int)(wp - L'1'); if (g_matchCount > 0 && n < g_matchCount) return 0; }
        else if (wp == 0x0D) return 0;
        else if ((GetKeyState(VK_MENU) & 0x8000) && g_matchCount > 0 && wp != L' ') return 0;
    }
    return CallWindowProcW(g_oldEditProc, hWnd, msg, wp, lp);
}

static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_taskbarCreated && msg == g_taskbarCreated) { AddTrayIcon(hDlg); return TRUE; }

    switch (msg) {
    case WM_INITDIALOG: {
        BOOL forceShow = (BOOL)lp;
        g_hMainDlg = hDlg;

        #ifndef DWMWA_WINDOW_CORNER_PREFERENCE
        #define DWMWA_WINDOW_CORNER_PREFERENCE 33
        #endif
        int pref = 2;
        DwmSetWindowAttribute(hDlg, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));

        /* 原版代码： */
        // g_dpiScale = GetDpiForWindowSafe(hDlg) / 96.0;
        // if (g_dpiScale <= 0) g_dpiScale = 1.0;
        
        /* 替换为： */
        g_initDpi = GetDpiForWindowSafe(hDlg);
        if (!g_initDpi) g_initDpi = 96;
        g_dpiScale = g_initDpi / 96.0;

        HWND hEdit = GetDlgItem(hDlg, ID_EDIT);
        HWND hList = GetDlgItem(hDlg, ID_LIST);

        g_hFontUI = CreateFontW(-(int)(g_fontSizeUI * g_dpiScale), 0, 0, 0, FW_BOLD, 0, 0, 0, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
        g_hFontEdit = CreateFontW(-(int)(g_fontSizeEdit * g_dpiScale), 0, 0, 0, FW_BOLD, 0, 0, 0, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
        SendMessageW(hList, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(hEdit, WM_SETFONT, (WPARAM)g_hFontEdit, TRUE);
        SendMessageW(hList, LB_SETITEMHEIGHT, 0, (LPARAM)(int)(g_listItemHeight * g_dpiScale));

        /* 1. 定义统一的边距和尺寸计算 */
        int margin = (int)(8 * g_dpiScale);
        GetWindowRect(hEdit, &g_rcEdit);
        MapWindowPoints(HWND_DESKTOP, hDlg, (POINT*)&g_rcEdit, 2);
        int baseEditW = g_rcEdit.right - g_rcEdit.left; /* 记住输入框原本的宽度 */

        /* [FIX 14 修正3] 取字体实际最大高度，仅加 4 像素微小留白，保证比例紧凑且不切字 */
        int newEditH = (int)(g_fontSizeEdit * g_dpiScale * 1.25); /* 保底 */
        if (hEdit && g_hFontEdit) {
            HDC hdc = GetDC(hDlg);
            if (hdc) {
                HFONT hOld = (HFONT)SelectObject(hdc, g_hFontEdit);
                TEXTMETRICW tm;
                if (GetTextMetricsW(hdc, &tm)) {
                    /* tmAscent + tmDescent 刚好是包含降部（如 g,p,q）的物理高度，加 2 像素防切边 */
                    newEditH = tm.tmAscent + tm.tmDescent + (int)(2 * g_dpiScale);
                }
                SelectObject(hdc, hOld);
                ReleaseDC(hDlg, hdc);
            }
        }
        /* 兜底最小高度 */
        if (newEditH < (int)(28 * g_dpiScale)) newEditH = (int)(28 * g_dpiScale);

        int boxExpandY = (int)(4 * g_dpiScale);
        int editTop = margin + boxExpandY;

        int gap = (int)(6 * g_dpiScale);
        int newListTop = margin + newEditH + (boxExpandY * 2) + gap;

        GetWindowRect(hList, &g_rcList);
        MapWindowPoints(HWND_DESKTOP, hDlg, (POINT*)&g_rcList, 2);
        int targetListH = (int)(g_listItemHeight * 9 * g_dpiScale) + GetSystemMetrics(SM_CYEDGE) * 2;

        int statusBarH = (int)(g_fontSizeUI * g_dpiScale * 1.6);
        if (statusBarH < 24) statusBarH = 24;

        /* 提前计算主窗口宽度 */
        g_initW = baseEditW + 2 * margin;
        g_initH = editTop + newEditH + boxExpandY + margin;
        
        g_extraInitH = gap + targetListH + statusBarH;
        g_expandedH = g_initH + g_extraInitH;

        /* 2. 应用子控件位置 */
        /* 输入框：保留两边 margin */
        SetWindowPos(hEdit, NULL, margin, editTop, baseEditW, newEditH, SWP_NOZORDER | SWP_NOACTIVATE);
        /* 列表框：顶满左右（留 1 像素防裁剪） */
        SetWindowPos(hList, NULL, (int)(1 * g_dpiScale), newListTop, g_initW - (int)(2 * g_dpiScale), targetListH, SWP_NOZORDER | SWP_NOACTIVATE);

        /* 3. 重新获取基准并保存 */
        GetWindowRect(hEdit, &g_rcEdit);
        MapWindowPoints(HWND_DESKTOP, hDlg, (POINT*)&g_rcEdit, 2);
        g_originalRcEdit = g_rcEdit;

        GetWindowRect(hList, &g_rcList);
        MapWindowPoints(HWND_DESKTOP, hDlg, (POINT*)&g_rcList, 2);

        {
            HINSTANCE hInst = GetModuleHandleW(NULL);
            g_hIconBig = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED);
            g_hIconSmall = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
            if (g_hIconBig)   SendMessageW(hDlg, WM_SETICON, ICON_BIG,   (LPARAM)g_hIconBig);
            if (g_hIconSmall) SendMessageW(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)g_hIconSmall);
        }

        ShowWindow(hList, SW_HIDE);
        g_oldEditProc = (WNDPROC)SetWindowLongPtrW(hEdit, GWLP_WNDPROC, (LONG_PTR)EditSubclassProc);

        /* 4. 设置主窗口位置和大小 */
        int x, y;
        if (!LoadWindowPos(&x, &y)) {
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);
            x = (screenW - g_initW) / 2;
            y = (screenH - g_expandedH) / 2;
        }
        SetWindowPos(hDlg, NULL, x, y, g_initW, g_initH, SWP_NOZORDER | SWP_NOACTIVATE);

        /* 5. 重建圆角 Region */
        HRGN hRgn = CreateRoundRectRgn(0, 0, g_initW + 1, g_initH + 1, 5, 5);
        SetWindowRgn(hDlg, hRgn, TRUE);

        g_listVisible = FALSE; g_selected = -1; g_matchCount = 0;
        g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

        if (!g_temporaryMode) {
            AddTrayIcon(hDlg);
            RegisterHotkeyFromIni(hDlg);

            /* --notray 且热键未注册成功（配置了热键但失败）→ 警告并退出 */
            if (g_forceNoTray && !g_hotkeyRegistered && g_hotkeyStr[0]) {
                g_inModal = TRUE;
                MessageBoxW(hDlg,
                    L"QuickRun 启动失败：\n\n"
                    L"--no-tray 模式下热键未成功注册，且托盘已禁用。\n\n"
                    L"程序将退出。请检查 QuickRun.ini 的 [Hotkey] Key 设置。",
                    L"QuickRun", MB_ICONERROR);
                g_inModal = FALSE;
                g_quitting = TRUE;
                PostMessageW(hDlg, WM_CLOSE, 0, 0);
                return FALSE;
            }

            /* 失联保护：热键未生效且托盘被隐藏时，强制保留托盘 */
            if (!g_forceNoTray && !g_showTrayIcon && !g_hotkeyRegistered) {
                g_showTrayIcon = TRUE;
                AddTrayIcon(hDlg);
                if (g_hotkeyStatus == HOTKEY_PARSE_FAIL)
                    ShowQuickToast(hDlg, L"⚠️ 热键格式错误且无可用热键，已强制保留托盘图标", TRUE);
                else
                    ShowQuickToast(hDlg, L"⚠️ 热键未生效，已强制保留托盘图标", TRUE);
            }

            /* [FIX 7] --notray 且热键留空 → 仅启动时提示一次（开机自启时不打扰） */
            if (!g_fromSilent && g_forceNoTray && !g_hotkeyRegistered && !g_hotkeyStr[0]) {
                g_inModal = TRUE;
                MessageBoxW(hDlg,
                    L"当前使用 --no-tray 且未设置热键。\n\n"
                    L"窗口隐藏后，只能通过再次运行 QuickRun.exe --show 恢复。\n\n"
                    L"建议在 QuickRun.ini 的 [Hotkey] 中设置热键，或去掉 --no-tray 参数。",
                    L"QuickRun 提示", MB_ICONINFORMATION);
                g_inModal = FALSE;
            }
        }

        BOOL shouldShow = forceShow || !g_fromSilent;
        ResetUI(hDlg, shouldShow);

        if (shouldShow && !g_guideShown) {
            g_guideShown = TRUE;
            g_inModal = TRUE;
            MessageBoxW(hDlg,
                L"QuickRun 已常驻后台。\n\n"
                L"• 按热键唤起（默认 Ctrl+Alt+R，可在 QuickRun.ini 里改）\n"
                L"• 托盘图标右键可执行更多操作\n"
                L"• 托盘菜单可启用开机自启\n"
                L"• 按 Esc 或点击窗口外可隐藏（不退出）\n\n"
                L"真正退出请按 Ctrl+Q 或通过托盘菜单的\"退出\"。",
                L"QuickRun 首次启动", MB_ICONINFORMATION);
            g_inModal = FALSE;
        }

        /* 新增：数据校验提醒（手动启动、非 --once） */
        if (shouldShow && !g_temporaryMode && g_parseErrCount > 0) {
            WCHAR toastMsg[128];
            wsprintfW(toastMsg, L"⚠️ 数据文件有 %d 处格式问题，详见诊断信息", g_parseErrCount);
            ShowQuickToast(hDlg, toastMsg, 2);
        }

        /* 新增：初次创建后同步一次托盘 ToolTip */
        UpdateTrayTip(hDlg);

        return FALSE;
    }

    case WM_DPICHANGED: {
        UINT newDpi = HIWORD(wp);
        if (!newDpi) newDpi = 96;
        double newScale = newDpi / 96.0;
        double initScale = (g_initDpi > 0 ? g_initDpi / 96.0 : 1.0);
        double fromInit = newScale / initScale;
        
        g_dpiScale = newScale;
        g_initDpi = newDpi;

        /* 1. 统一计算边距与基础宽度 */
        int margin = (int)(8 * g_dpiScale);
        int boxExpandY = (int)(4 * g_dpiScale);
        int gap = (int)(6 * g_dpiScale);
        
        /* 核心修复：用初始输入框的物理基准乘以比例，推导出当前应该有的绝对宽度 */
        int baseEditW = (int)((g_originalRcEdit.right - g_originalRcEdit.left) * fromInit);
        if (baseEditW <= 0) baseEditW = (int)(196 * g_dpiScale);
        
        /* 主窗口宽度死死锁住 = 输入框宽度 + 2个边距 */
        g_initW = baseEditW + 2 * margin;

        /* 2. 精确计算高度：用历史基准按比例缩放，坚决避开跨屏瞬间 DC 不准导致的测量误差 */
        int newEditH = (int)((g_originalRcEdit.bottom - g_originalRcEdit.top) * fromInit);
        if (newEditH < (int)(28 * g_dpiScale)) newEditH = (int)(28 * g_dpiScale);
        
        HWND hEdit = GetDlgItem(hDlg, ID_EDIT);
        HWND hList = GetDlgItem(hDlg, ID_LIST);

        int editTop = margin + boxExpandY;
        int newListTop = margin + newEditH + (boxExpandY * 2) + gap;
        
        int targetListH = (int)(g_listItemHeight * 9 * g_dpiScale) + GetSystemMetrics(SM_CYEDGE) * 2;
        int statusBarH = (int)(g_fontSizeUI * g_dpiScale * 1.6);
        if (statusBarH < 24) statusBarH = 24;

        g_initH = editTop + newEditH + boxExpandY + margin;
        int extraH = gap + targetListH + statusBarH;
        g_expandedH = g_initH + extraH;

        /* 3. 更新字体 */
        if (g_hFontUI)   DeleteObject(g_hFontUI);
        if (g_hFontEdit) DeleteObject(g_hFontEdit);
        g_hFontUI   = CreateFontW(-(int)(g_fontSizeUI * g_dpiScale), 0, 0, 0, FW_BOLD, 0, 0, 0, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
        g_hFontEdit = CreateFontW(-(int)(g_fontSizeEdit * g_dpiScale), 0, 0, 0, FW_BOLD, 0, 0, 0, GB2312_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei");
        
        SendMessageW(hList, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(hEdit, WM_SETFONT, (WPARAM)g_hFontEdit, TRUE);
        SendMessageW(hList, LB_SETITEMHEIGHT, 0, (LPARAM)(int)(g_listItemHeight * g_dpiScale));

        /* 4. 精准应用子控件尺寸 */
        SetWindowPos(hEdit, NULL, margin, editTop, baseEditW, newEditH, SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(hList, NULL, (int)(1 * g_dpiScale), newListTop, g_initW - (int)(2 * g_dpiScale), targetListH, SWP_NOZORDER | SWP_NOACTIVATE);

        /* 5. 更新所有历史基准，确保后续一切操作无误差 */
        g_extraInitH = extraH;
        
        RECT rcEdit, rcList;
        GetWindowRect(hEdit, &rcEdit);
        MapWindowPoints(HWND_DESKTOP, hDlg, (POINT*)&rcEdit, 2);
        g_originalRcEdit = rcEdit;
        g_rcEdit = rcEdit; /* 让背景圆角框完美贴合 */

        GetWindowRect(hList, &rcList);
        MapWindowPoints(HWND_DESKTOP, hDlg, (POINT*)&rcList, 2);
        g_rcList = rcList;

        /* 6. 应用主窗口尺寸和圆角 */
        int winH = g_listVisible ? g_expandedH : g_initH;
        RECT *prc = (RECT *)lp;
        SetWindowPos(hDlg, NULL, prc->left, prc->top, g_initW, winH, SWP_NOZORDER | SWP_NOACTIVATE);
        HRGN hRgn = CreateRoundRectRgn(0, 0, g_initW + 1, winH + 1, 5, 5);
        SetWindowRgn(hDlg, hRgn, TRUE);

        InvalidateRect(hDlg, NULL, TRUE);
        return TRUE;
    }

    case WM_ERASEBKGND: {
        HDC hdc = (HDC)wp;
        RECT rc;
        GetClientRect(hDlg, &rc);
        int midY = rc.top + (rc.bottom - rc.top) / 2;

        TRIVERTEX vTop[2] = { { rc.left, rc.top, 0xbA00, 0xbA00, 0xbA00, 0x0000 }, { rc.right, midY,  0xFF00, 0xFF00, 0xFF00, 0x0000 } };
        GRADIENT_RECT gTop = { 0, 1 };
        GradientFill(hdc, vTop, 2, &gTop, 1, GRADIENT_FILL_RECT_V);

        TRIVERTEX vBottom[2] = { { rc.left, midY,   0xFF00, 0xFF00, 0xFF00, 0x0000 }, { rc.right, rc.bottom, 0xCA00, 0xCA00, 0xCA00, 0x0000 } };
        GRADIENT_RECT gBottom = { 0, 1 };
        GradientFill(hdc, vBottom, 2, &gBottom, 1, GRADIENT_FILL_RECT_V);

        HBRUSH hWhite = (HBRUSH)GetStockObject(WHITE_BRUSH);
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(200, 200, 200));
        HBRUSH hOldBrush = (HBRUSH)SelectObject(hdc, hWhite);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        int boxExpandX = (int)(4 * g_dpiScale);
        int boxExpandY = (int)(4 * g_dpiScale);
        RoundRect(hdc, g_rcEdit.left - boxExpandX, g_rcEdit.top - boxExpandY, g_rcEdit.right + boxExpandX, g_rcEdit.bottom + boxExpandY, 12, 12);
        SelectObject(hdc, hOldBrush); SelectObject(hdc, hOldPen); DeleteObject(hPen);

        if (g_listVisible) {
            HWND hList = GetDlgItem(hDlg, ID_LIST);
            RECT rcList;
            GetWindowRect(hList, &rcList);
            MapWindowPoints(HWND_DESKTOP, hDlg, (POINT*)&rcList, 2);
            RECT rcStatus = rc; rcStatus.top = rcList.bottom;
            SetBkMode(hdc, TRANSPARENT);
            HFONT hOldFont = (HFONT)SelectObject(hdc, g_hFontUI);
            SetTextColor(hdc, RGB(80, 80, 80));
            rcStatus.left += 10; rcStatus.right -= 10;
            DrawTextW(hdc, g_statusText, -1, &rcStatus, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(hdc, hOldFont);
        }

        HPEN hBorderPen = CreatePen(PS_SOLID, 1, RGB(140, 140, 140));
        HPEN hOldBPen = (HPEN)SelectObject(hdc, hBorderPen);
        HBRUSH hOldBBrush = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1, 5, 5);
        SelectObject(hdc, hOldBPen); SelectObject(hdc, hOldBBrush); DeleteObject(hBorderPen);
        return TRUE;
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            if (!g_inModal && !g_activating) {
                if (g_temporaryMode) { g_quitting = TRUE; PostMessageW(hDlg, WM_CLOSE, 0, 0); }
                else                 { ShowWindow(hDlg, SW_HIDE); }
            }
        }
        break;

    case WM_LBUTTONDOWN:
        SendMessageW(hDlg, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return TRUE;

    case WM_EXITSIZEMOVE:
        SaveState(hDlg);
        return TRUE;

    case WM_HOTKEY:
        if (wp == HOTKEY_ID) ResetUI(hDlg, TRUE);
        return TRUE;

    case WM_QR_ACTIVATE:
        ResetUI(hDlg, TRUE);
        return TRUE;

    /* [FIX 1] 校验 lpData，防止外部构造畸形 COPYDATASTRUCT 导致空指针崩溃 */
    case WM_COPYDATA: {
        COPYDATASTRUCT *cds = (COPYDATASTRUCT *)lp;
        if (cds && cds->dwData == QR_IPC_MAGIC &&
            cds->cbData == sizeof(QR_IPC) && cds->lpData) {
            QR_IPC *ip = (QR_IPC *)cds->lpData;
            wcsncpy(g_prefill, ip->input, MAX_INPUT - 1);
            g_prefill[MAX_INPUT - 1] = 0;
            ResetUI(hDlg, TRUE);
        }
        return TRUE;
    }

    case WM_TIMER:
        if (wp == IDT_EVDEBOUNCE) { KillTimer(hDlg, IDT_EVDEBOUNCE); AppendEverythingResults(hDlg); }
        return TRUE;

    case WM_APP_TRAY:
        if (lp == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW, L"显示界面(&S)");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_RESETPOS, L"重置窗口(&Z)\tF4");            
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_RELOAD, L"重载配置(&R)\tF5");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_DATA, L"打开数据文件(&D)\tF2");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_CONFIG, L"打开配置文件(&C)\tF3");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_DIAG, L"导出诊断信息(&E)\tF9");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            BOOL autoStart = IsAutoStartEnabled();
            AppendMenuW(hMenu, MF_STRING | (autoStart ? MF_CHECKED : 0), ID_TRAY_AUTOSTART, L"开机自启(&A)");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_HELP, L"帮助(&H)\tF1");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出(&X)");

            SetForegroundWindow(hDlg);
            g_inModal = TRUE;
            int cmd = (int)TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hDlg, NULL);
            g_inModal = FALSE;
            DestroyMenu(hMenu);
            PostMessageW(hDlg, WM_NULL, 0, 0);

            switch (cmd) {
            case ID_TRAY_SHOW:      PostMessageW(hDlg, WM_QR_ACTIVATE, 0, 0); break;
            case ID_RESETPOS:       PostMessageW(hDlg, WM_COMMAND, MAKEWPARAM(ID_RESETPOS, 0), 0); break;
            case ID_DATA:           PostMessageW(hDlg, WM_COMMAND, MAKEWPARAM(ID_DATA, 0), 0); break;
            case ID_TRAY_CONFIG:    PostMessageW(hDlg, WM_COMMAND, MAKEWPARAM(ID_TRAY_CONFIG, 0), 0); break;
            case ID_TRAY_RELOAD:    PostMessageW(hDlg, WM_APP_RELOAD, 0, 0); break;
            case ID_TRAY_DIAG:      PostMessageW(hDlg, WM_COMMAND, MAKEWPARAM(ID_TRAY_DIAG, 0), 0); break;
            case ID_TRAY_AUTOSTART:
                SetAutoStart(!IsAutoStartEnabled());
                if (IsAutoStartEnabled()) ShowQuickToast(hDlg, L"✅ 已开启开机自启", FALSE);
                else                      ShowQuickToast(hDlg, L"🚫 已关闭开机自启", TRUE);
                break;
            case ID_HELP:           PostMessageW(hDlg, WM_COMMAND, MAKEWPARAM(ID_HELP, 0), 0); break;
            case ID_TRAY_EXIT:      g_quitting = TRUE; PostMessageW(hDlg, WM_CLOSE, 0, 0); break;
            }
        } else if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) {
            PostMessageW(hDlg, WM_QR_ACTIVATE, 0, 0);
        }
        return TRUE;

    case WM_APP_RELOAD:
        ReloadAll(hDlg);
        return TRUE;

    case WM_MEASUREITEM: {
        LPMEASUREITEMSTRUCT mis = (LPMEASUREITEMSTRUCT)lp;
        if (mis->CtlID == ID_LIST) { mis->itemHeight = (UINT)(g_listItemHeight * g_dpiScale); return TRUE; }
        break;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT dis = (LPDRAWITEMSTRUCT)lp;
        if (dis->CtlID != ID_LIST) break;
        if (dis->itemID == (UINT)-1) return TRUE;

        int idx = (int)SendMessageW(dis->hwndItem, LB_GETITEMDATA, dis->itemID, 0);
        if (idx < 0 || idx >= g_matchCount) return TRUE;

        HDC  hdc = dis->hDC;
        RECT rc  = dis->rcItem;
        BOOL sel = (dis->itemState & ODS_SELECTED) ? TRUE : FALSE;
        BOOL isFile = (g_matchCmds[idx][0] == 0);

        if (sel) { if (!g_hSelBrush) g_hSelBrush = CreateSolidBrush(RGB(14, 99, 156)); FillRect(hdc, &rc, g_hSelBrush); }
        else { FillRect(hdc, &rc, GetSysColorBrush(COLOR_WINDOW)); }

        HFONT hOld = (HFONT)SelectObject(hdc, g_hFontUI);
        SetBkMode(hdc, TRANSPARENT);

        WCHAR num[8];
        wsprintfW(num, L"%d.", idx + 1);
        RECT rcNum = rc; rcNum.left += 8;
        SetTextColor(hdc, sel ? RGB(200, 200, 200) : RGB(160, 160, 160));
        DrawTextW(hdc, num, -1, &rcNum, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT rcKey = rc;
        {
            SIZE szNum = { 0, 0 };
            GetTextExtentPoint32W(hdc, num, (int)wcslen(num), &szNum);
            rcKey.left += 8 + szNum.cx + 8;
        }
        if (isFile) {
            SetTextColor(hdc, sel ? RGB(255, 255, 255) : RGB(43, 87, 154));
            DrawTextW(hdc, g_matches[idx], -1, &rcKey, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        } else {
            WCHAR *colon = wcschr(g_matches[idx], L':');
            RECT rcDesc = rc; rcDesc.right -= 8;
            SIZE szCat = { 0, 0 }; SIZE szMain = { 0, 0 };

            if (colon && (colon - g_matches[idx] < 32)) {
                WCHAR cat[32];
                size_t clen = colon - g_matches[idx];
                wcsncpy(cat, g_matches[idx], clen);
                cat[clen] = 0;

                SetTextColor(hdc, sel ? RGB(180, 180, 180) : RGB(130, 130, 130));
                DrawTextW(hdc, cat, -1, &rcKey, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                GetTextExtentPoint32W(hdc, cat, (int)wcslen(cat), &szCat);
                rcKey.left += szCat.cx;
                rcKey.left += 8;

                SetTextColor(hdc, sel ? RGB(255, 255, 255) : RGB(64, 64, 64));
                DrawTextW(hdc, colon + 1, -1, &rcKey, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                GetTextExtentPoint32W(hdc, colon + 1, (int)wcslen(colon + 1), &szMain);
                rcDesc.left = rcKey.left + szMain.cx + 12;
            } else {
                SetTextColor(hdc, sel ? RGB(255, 255, 255) : RGB(64, 64, 64));
                DrawTextW(hdc, g_matches[idx], -1, &rcKey, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                GetTextExtentPoint32W(hdc, g_matches[idx], (int)wcslen(g_matches[idx]), &szMain);
                rcDesc.left = rcKey.left + szMain.cx + 12;
            }

            SetTextColor(hdc, sel ? RGB(224, 224, 224) : RGB(130, 130, 130));
            DrawTextW(hdc, g_matchDescs[idx], -1, &rcDesc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        SelectObject(hdc, hOld);
        return TRUE;
    }

    case WM_CTLCOLOREDIT: {
        if ((HWND)lp == GetDlgItem(hDlg, ID_EDIT)) {
            SetBkColor((HDC)wp, RGB(255, 255, 255));
            SetTextColor((HDC)wp, RGB(0, 0, 0));
            return (INT_PTR)GetStockObject(WHITE_BRUSH);
        }
        break;
    }

    case WM_COMMAND: {
        WORD id   = LOWORD(wp);
        WORD code = HIWORD(wp);

        if (id == ID_EDIT && code == EN_CHANGE) {
            g_selected = -1;
            SendMessageW(GetDlgItem(hDlg, ID_LIST), LB_SETCURSEL, (WPARAM)-1, 0);
            UpdateSearch(hDlg);
            return TRUE;
        }

        if (id == ID_LIST && code == LBN_SELCHANGE) {
            int sel = (int)SendMessageW(GetDlgItem(hDlg, ID_LIST), LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_matchCount) g_selected = sel;
            SetFocus(GetDlgItem(hDlg, ID_EDIT));
            UpdateStatusBar(hDlg);
            return TRUE;
        }

        if (id == ID_LIST && code == LBN_DBLCLK) {
            int sel = (int)SendMessageW(GetDlgItem(hDlg, ID_LIST), LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_matchCount) { g_selected = sel; PostMessageW(hDlg, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0); }
            return TRUE;
        }

        if (id == IDOK) {
            ExecuteInput(hDlg, code == 1);
            if (g_temporaryMode) { g_quitting = TRUE; PostMessageW(hDlg, WM_CLOSE, 0, 0); }
            else                 { ShowWindow(hDlg, SW_HIDE); }
            return TRUE;
        }

        if (id == IDCANCEL) {
            if (g_temporaryMode) { g_quitting = TRUE; PostMessageW(hDlg, WM_CLOSE, 0, 0); }
            else                 { ShowWindow(hDlg, SW_HIDE); }
            return TRUE;
        }

        if (id == ID_DATA) {
            HINSTANCE hi = ShellExecuteW(hDlg, L"open", g_dataFile, NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)hi <= 32) {
                WCHAR errMsg[800];
                wsprintfW(errMsg, L"无法打开数据文件：\n%s\n\n错误码：%d", g_dataFile, (int)(INT_PTR)hi);
                g_inModal = TRUE;
                MessageBoxW(hDlg, errMsg, L"错误", MB_ICONERROR);
                g_inModal = FALSE;
            }
            SetFocus(GetDlgItem(hDlg, ID_EDIT));
            return TRUE;
        }

        if (id == ID_TRAY_CONFIG) {
            HINSTANCE hi = ShellExecuteW(hDlg, L"open", g_iniPath, NULL, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)hi <= 32) {
                WCHAR errMsg[800];
                wsprintfW(errMsg, L"无法打开配置文件：\n%s\n\n错误码：%d", g_iniPath, (int)(INT_PTR)hi);
                g_inModal = TRUE;
                MessageBoxW(hDlg, errMsg, L"错误", MB_ICONERROR);
                g_inModal = FALSE;
            }
            SetFocus(GetDlgItem(hDlg, ID_EDIT));
            return TRUE;
        }

        if (id == ID_TRAY_DIAG) {
            ExportDiagnostics(hDlg);
            SetFocus(GetDlgItem(hDlg, ID_EDIT));
            return TRUE;
        }

        if (id == ID_HELP) {
            g_inModal = TRUE;
            ShowHelpWindow(hDlg, FALSE);
            if (!g_hHelpWnd || !IsWindow(g_hHelpWnd)) g_inModal = FALSE;
            return TRUE;
        }

        if (id == ID_RESETPOS) {
            WritePrivateProfileStringW(L"Window", L"X", NULL, g_statePath);
            WritePrivateProfileStringW(L"Window", L"Y", NULL, g_statePath);
            int screenW = GetSystemMetrics(SM_CXSCREEN);
            int screenH = GetSystemMetrics(SM_CYSCREEN);
            int x = (screenW - g_initW) / 2;
            int y = (screenH - g_expandedH) / 2;
            SetWindowPos(hDlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            return TRUE;
        }

        if (id == ID_OPEN_FOLDER) {
            if (g_selected >= 0 && g_selected < g_matchCount && g_matchCmds[g_selected][0] == 0) {
                WCHAR arg[2048];
                wsprintfW(arg, L"/select,\"%s\"", g_matchFullPaths[g_selected]);
                ShellExecuteW(hDlg, L"open", L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
            }
            SetFocus(GetDlgItem(hDlg, ID_EDIT));
            return TRUE;
        }
        return TRUE;
    }

    case WM_HELP: return TRUE;

    /* [FIX 2] --once 模式下关闭窗口应直接退出，避免隐藏后无托盘/无热键的僵尸进程 */
    case WM_CLOSE:
        if (g_quitting || g_temporaryMode) DestroyWindow(hDlg);
        else                               ShowWindow(hDlg, SW_HIDE);
        return TRUE;

    case WM_DESTROY:
	    g_hMainDlg = NULL;
        if (g_hQuickToast && IsWindow(g_hQuickToast)) DestroyWindow(g_hQuickToast);
        RemoveTrayIcon(hDlg);
        if (g_hotkeyRegistered) { UnregisterHotKey(hDlg, HOTKEY_ID); g_hotkeyRegistered = FALSE; }
        SaveState(hDlg);
        if (g_hFontUI)   { DeleteObject(g_hFontUI);   g_hFontUI   = NULL; }
        if (g_hFontEdit) { DeleteObject(g_hFontEdit); g_hFontEdit = NULL; }
        if (g_hSelBrush) { DeleteObject(g_hSelBrush); g_hSelBrush = NULL; }
        PostQuitMessage(0);
        return TRUE;
    }
    return FALSE;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShow)
{
    WCHAR path[MAX_PATH];
    WCHAR dataFile[MAX_PATH * 2];
    WCHAR *slash;
    DWORD  n;
    int    argc = 0;
    LPWSTR *argv;
    WCHAR  customData[MAX_PATH * 2] = { 0 };
    BOOL   hasCustomData = FALSE;
    BOOL   wantHelp = FALSE;
    BOOL   wantShow = FALSE;
    int    i;

    (void)hPrev; (void)lpCmdLine; (void)nShow;

    EnableDpiAwareness();

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    for (i = 1; i < argc; i++) {
        if (_wcsicmp(argv[i], L"-h") == 0 || _wcsicmp(argv[i], L"--help") == 0 || _wcsicmp(argv[i], L"/?") == 0) {
            wantHelp = TRUE;
        } else if (_wcsicmp(argv[i], L"--no-fallback") == 0) {
            g_noFallback = TRUE;
        } else if (_wcsicmp(argv[i], L"--silent") == 0) {
            g_fromSilent = TRUE;
        } else if (_wcsicmp(argv[i], L"--once") == 0) {
            g_temporaryMode = TRUE;
        } else if (_wcsicmp(argv[i], L"--no-tray") == 0) {
            g_forceNoTray = TRUE;
        } else if (_wcsicmp(argv[i], L"--show") == 0) {
            wantShow = TRUE;
        } else if (wcsncmp(argv[i], L"--data=", 7) == 0) {
            /* --date= 是 --data= 的历史兼容别名 */
            wcsncpy(customData, argv[i] + 7, MAX_PATH * 2 - 1);
            customData[MAX_PATH * 2 - 1] = 0;
            TrimW(customData);
            if (customData[0]) hasCustomData = TRUE;
        } else if (wcsncmp(argv[i], L"--input=", 8) == 0) {
            wcsncpy(g_prefill, argv[i] + 8, MAX_INPUT - 1);
            g_prefill[MAX_INPUT - 1] = 0;
        } else if (!g_input[0]) {
            wcsncpy(g_input, argv[i], MAX_INPUT - 1);
            g_input[MAX_INPUT - 1] = 0;
        } else if (g_argCount < MAX_ARGS) {
            wcsncpy(g_args[g_argCount], argv[i], MAX_INPUT - 1);
            g_args[g_argCount][MAX_INPUT - 1] = 0;
            g_argCount++;
        }
    }

    HANDLE hMutex = NULL;
    if (!g_input[0] && !wantHelp && !g_temporaryMode) {
        hMutex = CreateMutexW(NULL, FALSE, L"QuickRun_SingleInstance_Mutex");
        if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            if (g_fromSilent) {
                CloseHandle(hMutex); if (argv) LocalFree(argv); return 0;
            }

            HWND hExisting = NULL;
            for (i = 0; i < 50 && !hExisting; i++) {
                hExisting = FindWindowW(NULL, L"QuickRun");
                if (!hExisting) Sleep(20);
            }
            if (hExisting) {
                /* [FIX 5] 无参数二次启动也应唤起已有窗口；--silent 保持静默；
                   有 prefill 时由 WM_COPYDATA 内部激活，避免重复激活。 */
                if (g_prefill[0]) {
                    QR_IPC payload = { 0 };
                    wcsncpy(payload.input, g_prefill, MAX_INPUT - 1);
                    payload.input[MAX_INPUT - 1] = 0;
                    COPYDATASTRUCT cds = { 0 };
                    cds.dwData = QR_IPC_MAGIC;
                    cds.cbData = sizeof(payload);
                    cds.lpData = &payload;
                    SendMessageW(hExisting, WM_COPYDATA, 0, (LPARAM)&cds);
                } else if (wantShow || !g_fromSilent) {
                    PostMessageW(hExisting, WM_QR_ACTIVATE, 0, 0);
                }
            }
            CloseHandle(hMutex);
            if (argv) LocalFree(argv);
            return 0;
        }
    }

    if (wantHelp) {
        /* [FIX 6] --help 独立启动时先初始化 DPI，避免高 DPI 下帮助窗口偏小 */
        {
            UINT dpi = GetDpiForWindowSafe(NULL);
            if (!dpi) dpi = 96;
            g_dpiScale = dpi / 96.0;
        }
        ShowHelpWindow(NULL, TRUE); /* 传入 TRUE */
        if (!g_hHelpWnd || !IsWindow(g_hHelpWnd)) {
            if (hMutex) CloseHandle(hMutex);
            if (argv) LocalFree(argv);
            return 0;
        }
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (hMutex) CloseHandle(hMutex);
        if (argv)   LocalFree(argv);
        return 0;
    }

    n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        if (hMutex) CloseHandle(hMutex);
        if (argv)   LocalFree(argv);
        return 0;
    }
    slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = 0;
    else       path[0] = 0;

    if (hasCustomData) {
        if (customData[0] == L'\\' || (customData[0] && customData[1] == L':')) wcsncpy(dataFile, customData, MAX_PATH * 2 - 1);
        else {
            wcsncpy(dataFile, path, MAX_PATH * 2 - 1);
            wcsncat(dataFile, customData, MAX_PATH * 2 - 1 - wcslen(dataFile));
        }
    } else {
        wcsncpy(dataFile, path, MAX_PATH * 2 - 1);
        wcsncat(dataFile, L"QuickRun.txt", MAX_PATH * 2 - 1 - wcslen(dataFile));
    }
    dataFile[MAX_PATH * 2 - 1] = 0;

    /* [FIX 11] 完整检查 GetFullPathNameW 返回值：0 表示失败，>= 缓冲区表示截断 */
    {
        DWORD gpRet = GetFullPathNameW(dataFile, MAX_PATH * 2, g_dataFile, NULL);
        if (gpRet == 0 || gpRet >= MAX_PATH * 2) {
            wcsncpy(g_dataFile, dataFile, MAX_PATH * 2 - 1);
            g_dataFile[MAX_PATH * 2 - 1] = 0;
        }
    }

    /* 将子进程（如 cmd）的默认工作目录设置为用户主目录（同 Win+R 行为） */
    WCHAR userProfile[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", userProfile, MAX_PATH) > 0) {
        SetCurrentDirectoryW(userProfile);
    } else {
        SetCurrentDirectoryW(path); /* 获取失败时回退到 exe 目录 */
    }

    BuildPaths();
    EnsureConfigFileExists();
    LoadConfig();

    if (g_forceNoTray) g_showTrayIcon = FALSE;

    LoadState();
    EvLoad();
    EnsureDataFileExists();

    if (!LoadDataFile(g_dataFile)) {
        if (!g_input[0]) {
            MessageBoxW(NULL, L"未找到或无法读取数据文件。", L"错误", MB_ICONERROR);
            if (hMutex) CloseHandle(hMutex);
            if (argv)   LocalFree(argv);
            EvUnload();
            return 0;
        }
    }

    if (!g_input[0]) {
        BuildTemplate();
        if (g_tplOverflow) {
            MessageBoxW(NULL, L"内部错误：对话框模板溢出，程序无法启动。", L"QuickRun", MB_ICONERROR);
            if (hMutex) CloseHandle(hMutex);
            if (argv)   LocalFree(argv);
            EvUnload();
            return 0;
        }
        HWND hDlg = CreateDialogIndirectParamW(hInst, (LPDLGTEMPLATE)g_tpl.b, NULL, DlgProc, (LPARAM)(wantShow ? 1 : 0));
        if (hDlg) {
            MSG msg;
            while (GetMessageW(&msg, NULL, 0, 0) > 0) {
                if (!IsDialogMessageW(hDlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            }
        }
        if (hMutex) CloseHandle(hMutex);
        if (argv)   LocalFree(argv);
        EvUnload();
        return 0;
    }

    /* 命令行模式 */
    {
        if (g_argCount > 0) {
            BOOL matched = FALSE;
            for (i = 0; i < g_entryCount; i++) {
                if (_wcsicmp(g_entries[i].key, g_input) == 0) {
                    RunCommandEx(g_entries[i].cmd, g_entries[i].desc, g_entries[i].useCmd);
                    matched = TRUE; break;
                }
            }
            if (!matched && !g_noFallback) {
                WCHAR full[4096] = { 0 };
                WCHAR params[4096] = { 0 };
                size_t flen = 0, plen = 0;
                int k;
                AppendQuotedArg(full, 4096, &flen, g_input);
                for (k = 0; k < g_argCount; k++) {
                    if (flen + 1 < 4096) { full[flen++] = L' '; full[flen] = 0; }
                    AppendQuotedArg(full, 4096, &flen, g_args[k]);
                    /* 修改 4：复用 AppendQuotedArg 构造 params，正确处理末尾反斜杠+引号 */
                    if (k > 0 && plen + 1 < 4096) { params[plen++] = L' '; params[plen] = 0; }
                    AppendQuotedArg(params, 4096, &plen, g_args[k]);
                }
                params[plen] = 0;

                HINSTANCE hi = ShellExecuteW(NULL, L"open", g_input, g_argCount ? params : NULL, NULL, SW_SHOWNORMAL);
                if ((INT_PTR)hi <= 32) {
                    /* 修改 3：full 最长 4095，加上 "cmd /c " 和 " || pause" 后需要约 4112 */
                    WCHAR cmdLine[4400];
                    wsprintfW(cmdLine, L"cmd /c %s || timeout /t 2 >nul", full);
                    STARTUPINFOW si = { 0 };
                    PROCESS_INFORMATION pi = { 0 };
                    si.cb = sizeof(si);
                    if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                    }
                }
            }
        } else {
            BOOL matched = FALSE;
            for (i = 0; i < g_entryCount; i++) {
                if (_wcsicmp(g_entries[i].key, g_input) == 0) {
                    RunCommandEx(g_entries[i].cmd, g_entries[i].desc, g_entries[i].useCmd);
                    matched = TRUE; break;
                }
            }
            if (!matched && !g_noFallback) {
                HINSTANCE hi = ShellExecuteW(NULL, L"open", g_input, NULL, NULL, SW_SHOWNORMAL);
                if ((INT_PTR)hi <= 32) {
                    WCHAR cmdLine[2048];
                    wsprintfW(cmdLine, L"cmd /c %s || timeout /t 2 >nul", g_input);
                    STARTUPINFOW si = { 0 };
                    PROCESS_INFORMATION pi = { 0 };
                    si.cb = sizeof(si);
                    if (CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
                    }
                }
            }
        }
    }

    if (hMutex) CloseHandle(hMutex);
    if (argv) LocalFree(argv);
    EvUnload();
    return 0;
}