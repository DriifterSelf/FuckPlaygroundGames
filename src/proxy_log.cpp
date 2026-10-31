// Logging, configuration and game identification
#include <stdio.h>
#include <share.h>
#include "proxy_common.h"

proxy_config g_cfg = {};
proxy_telemetry g_tel = {};

static FILE* s_log = NULL;
static SRWLOCK s_log_lock = SRWLOCK_INIT;
static wchar_t s_ini_path[MAX_PATH];

void log_msg(const char* fmt, ...)
{
    AcquireSRWLockExclusive(&s_log_lock);
    if (s_log)
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(s_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list args;
        va_start(args, fmt);
        vfprintf(s_log, fmt, args);
        va_end(args);
        fprintf(s_log, "\n");
        fflush(s_log);
    }
    ReleaseSRWLockExclusive(&s_log_lock);
}

static void write_default(const wchar_t* key, const wchar_t* value)
{
    wchar_t buf[16];
    GetPrivateProfileStringW(L"proxy", key, L"", buf, 16, s_ini_path);
    if (buf[0] == 0)
    {
        WritePrivateProfileStringW(L"proxy", key, value, s_ini_path);
    }
}

void open_log_and_config()
{
    wchar_t base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH))
    {
        return;
    }

    wchar_t log_path[MAX_PATH];
    swprintf_s(log_path, L"%s\\rdna4_ags_proxy.log", base);
    s_log = _wfsopen(log_path, L"w", _SH_DENYNO);

    swprintf_s(s_ini_path, L"%s\\rdna4_ags_proxy.ini", base);
    write_default(L"Overlay", L"1");
    write_default(L"OverlayToggleKey", L"121");
    write_default(L"VramUncap", L"1");
    write_default(L"VramMarginPercent", L"8");
    write_default(L"OomGuard", L"1");
    write_default(L"OomGuardMaxSuppress", L"20");
    write_default(L"ComputeCullOffload", L"1");
    write_default(L"HybridCoreGovernor", L"1");
    write_default(L"ECoreThresholdMs", L"8");

    g_cfg.overlay = GetPrivateProfileIntW(L"proxy", L"Overlay", 1, s_ini_path);
    g_cfg.overlay_key = GetPrivateProfileIntW(L"proxy", L"OverlayToggleKey", VK_F10, s_ini_path);
    g_cfg.vram_uncap = GetPrivateProfileIntW(L"proxy", L"VramUncap", 1, s_ini_path);
    g_cfg.vram_margin_percent = GetPrivateProfileIntW(L"proxy", L"VramMarginPercent", 8, s_ini_path);
    g_cfg.oom_guard = GetPrivateProfileIntW(L"proxy", L"OomGuard", 1, s_ini_path);
    g_cfg.oom_guard_max = GetPrivateProfileIntW(L"proxy", L"OomGuardMaxSuppress", 20, s_ini_path);
    g_cfg.compute_offload = GetPrivateProfileIntW(L"proxy", L"ComputeCullOffload", 1, s_ini_path);
    g_cfg.hybrid_governor = GetPrivateProfileIntW(L"proxy", L"HybridCoreGovernor", 1, s_ini_path);
    g_cfg.ecore_threshold_ms = GetPrivateProfileIntW(L"proxy", L"ECoreThresholdMs", 8, s_ini_path);
    if (g_cfg.ecore_threshold_ms < 1) g_cfg.ecore_threshold_ms = 8;

    if (g_cfg.vram_margin_percent < 1) g_cfg.vram_margin_percent = 1;
    if (g_cfg.vram_margin_percent > 50) g_cfg.vram_margin_percent = 50;

    log_msg("==============================================================");
    log_msg("[PROXY] AGS proxy v%s loaded (self-aware build)", PROXY_VERSION);
    log_msg("[CONFIG] Overlay=%d Key=%d VramUncap=%d Margin=%d%% OomGuard=%d MaxSuppress=%d ComputeOffload=%d HybridGov=%d ECoreThreshold=%dms",
            g_cfg.overlay, g_cfg.overlay_key, g_cfg.vram_uncap, g_cfg.vram_margin_percent,
            g_cfg.oom_guard, g_cfg.oom_guard_max, g_cfg.compute_offload, g_cfg.hybrid_governor, g_cfg.ecore_threshold_ms);
}

void identify_game()
{
    HMODULE exe = GetModuleHandleW(NULL);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(exe, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    WideCharToMultiByte(CP_UTF8, 0, name ? name + 1 : path, -1,
                        g_tel.exe_name, sizeof(g_tel.exe_name), NULL, NULL);

    const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)exe;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const BYTE*)exe + dos->e_lfanew);
    g_tel.exe_timestamp = nt->FileHeader.TimeDateStamp;
    g_tel.exe_base = (uintptr_t)exe;
    g_tel.exe_end = (uintptr_t)exe + nt->OptionalHeader.SizeOfImage;

    log_msg("[GAME] %s | PE timestamp=0x%08X | image size=%llu MB",
            g_tel.exe_name, g_tel.exe_timestamp,
            (unsigned long long)((g_tel.exe_end - g_tel.exe_base) / (1024 * 1024)));
}
