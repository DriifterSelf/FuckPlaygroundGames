// Lightweight, click-through, self-aware diagnostic HUD overlay
#include <stdio.h>
#include "proxy_common.h"

static HWND s_hwnd = NULL;
static volatile BOOL s_visible = TRUE;

static COLORREF color_for_status(int st) {
    switch (st) {
    case st_ok:       return RGB(70, 240, 140);  // Green
    case st_pending:  return RGB(255, 205, 60);  // Yellow
    case st_fail:     return RGB(255, 80, 80);   // Red
    case st_disabled: return RGB(150, 160, 175); // Gray
    case st_warn:     return RGB(255, 150, 50);  // Orange
    default:          return RGB(200, 200, 200);
    }
}

static const char* text_for_status(int st) {
    switch (st) {
    case st_ok:       return "[OK]";
    case st_pending:  return "[PENDING]";
    case st_fail:     return "[FAILED]";
    case st_disabled: return "[DISABLED]";
    case st_warn:     return "[WARNING]";
    default:          return "[UNKNOWN]";
    }
}

static LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        // Dark background (RGB 12, 16, 20)
        HBRUSH hBg = CreateSolidBrush(RGB(12, 16, 20));
        FillRect(hdc, &rc, hBg);
        DeleteObject(hBg);

        // Dynamic border color: red if any failure, yellow if pending, green if all OK
        bool has_fail = (g_tel.st_vram == st_fail || g_tel.st_guard == st_fail || g_tel.st_dx12 == st_fail);
        bool has_pending = (g_tel.st_ags_init == st_pending || g_tel.st_dx12 == st_pending);
        COLORREF borderColor = has_fail ? RGB(255, 70, 70) : (has_pending ? RGB(255, 205, 60) : RGB(0, 210, 160));

        HPEN hPen = CreatePen(PS_SOLID, 1, borderColor);
        HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, 0, 0, rc.right, rc.bottom);
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);

        SetBkMode(hdc, TRANSPARENT);
        HFONT hFont = CreateFontA(12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

        int y = 7;
        const int step = 16;

        // 1. Header
        SetTextColor(hdc, RGB(0, 255, 180));
        TextOutA(hdc, 10, y, "[ AMD AGS PROXY v" PROXY_VERSION " - DIAGNOSTIC OVERLAY ]", 45);
        y += step;

        // 2. Game Info
        char gameBuf[128];
        sprintf_s(gameBuf, "• Game: %s (PE: 0x%08X)", g_tel.exe_name[0] ? g_tel.exe_name : "forzahorizon6.exe", g_tel.exe_timestamp);
        SetTextColor(hdc, RGB(210, 225, 240));
        TextOutA(hdc, 10, y, gameBuf, (int)strlen(gameBuf));
        y += step;

        // 3. Hardware (dynamic as reported by AGS/driver)
        char hwBuf[128];
        if (g_tel.gpu_name[0]) {
            sprintf_s(hwBuf, "• GPU: %s (CUs: %d | WGPs: %d | Family: %d)",
                      g_tel.gpu_name, g_tel.num_cus, g_tel.num_wgps, g_tel.asic_family);
        } else {
            sprintf_s(hwBuf, "• GPU: [Waiting for agsInitialize...]");
        }
        SetTextColor(hdc, RGB(225, 235, 245));
        TextOutA(hdc, 10, y, hwBuf, (int)strlen(hwBuf));
        y += step;

        // 4. VRAM Status
        char vramBuf[128];
        if (g_tel.st_vram == st_ok) {
            float useGB = (float)g_tel.current_usage_bytes / (1024.0f * 1024.0f * 1024.0f);
            float repGB = (float)g_tel.reported_budget_bytes / (1024.0f * 1024.0f * 1024.0f);
            float dedGB = (float)g_tel.dedicated_bytes / (1024.0f * 1024.0f * 1024.0f);
            sprintf_s(vramBuf, "• VRAM Pool: %s %.1f / %.1f GB (Max: %.1f GB) | Calls: %ld",
                      text_for_status(g_tel.st_vram), useGB, repGB, dedGB, g_tel.vram_calls);
        } else if (g_tel.st_vram == st_disabled) {
            sprintf_s(vramBuf, "• VRAM Pool: [DISABLED by INI] (Using default OS budget)");
        } else if (g_tel.st_vram == st_fail) {
            sprintf_s(vramBuf, "• VRAM Pool: [FAIL] (%s)", g_tel.vram_err);
        } else {
            sprintf_s(vramBuf, "• VRAM Pool: [PENDING] (Awaiting game DXGI query)");
        }
        SetTextColor(hdc, color_for_status(g_tel.st_vram));
        TextOutA(hdc, 10, y, vramBuf, (int)strlen(vramBuf));
        y += step;

        // 5. Memory Guard
        char guardBuf[128];
        if (g_tel.st_guard == st_ok) {
            sprintf_s(guardBuf, "• Memory Guard: [ACTIVE] (%ld suppressed / %ld hits / %ld passed)",
                      g_tel.guard_suppressed, g_tel.guard_hits, g_tel.guard_passed);
        } else if (g_tel.st_guard == st_disabled) {
            sprintf_s(guardBuf, "• Memory Guard: [DISABLED by INI]");
        } else {
            sprintf_s(guardBuf, "• Memory Guard: [FAILED TO INSTALL]");
        }
        SetTextColor(hdc, color_for_status(g_tel.st_guard));
        TextOutA(hdc, 10, y, guardBuf, (int)strlen(guardBuf));
        y += step;

        // 6. AGS & DX12 Extensions
        char dx12Buf[128];
        if (g_tel.st_dx12 == st_ok) {
            bool wmma = (g_tel.ext_bits & 0x20) != 0; // bit 5 = waveMatrix
            sprintf_s(dx12Buf, "• AGS DX12: [CONNECTED] (WMMA: %s | ExtBits: 0x%X)", wmma ? "YES" : "NO", g_tel.ext_bits);
        } else if (g_tel.st_dx12 == st_fail) {
            sprintf_s(dx12Buf, "• AGS DX12: [FAILED] (Result: %d)", g_tel.dx12_result);
        } else {
            sprintf_s(dx12Buf, "• AGS DX12: [PENDING ENGINE CALL]");
        }
        SetTextColor(hdc, color_for_status(g_tel.st_dx12));
        TextOutA(hdc, 10, y, dx12Buf, (int)strlen(dx12Buf));
        y += step;

        // 7. CPU Affinity (Strictly Read-Only, never modified)
        DWORD_PTR curAff = 0, sysAff = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &curAff, &sysAff);
        char affBuf[128];
        sprintf_s(affBuf, "• CPU Affinity: 0x%llX (Read-Only / Untouched by Proxy)", (unsigned long long)curAff);
        SetTextColor(hdc, RGB(180, 195, 215));
        TextOutA(hdc, 10, y, affBuf, (int)strlen(affBuf));
        y += step;

        // 8. Footer with toggle instructions
        char footBuf[64];
        if (g_cfg.overlay_key == VK_F10) {
            sprintf_s(footBuf, "[F10 / Insert] Ocultar / Mostrar Overlay");
        } else {
            sprintf_s(footBuf, "[Tecla %d / Insert] Ocultar / Mostrar Overlay", g_cfg.overlay_key);
        }
        SetTextColor(hdc, RGB(125, 140, 160));
        TextOutA(hdc, 10, y, footBuf, (int)strlen(footBuf));

        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static DWORD WINAPI OverlayThread(LPVOID) {
    // Early installation of VRAM hook from dedicated worker thread (avoids loader lock)
    vram_hook_install();

    WNDCLASSA wc = {};
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "AgsSelfAwareOverlayV3";
    RegisterClassA(&wc);

    s_hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        "AgsSelfAwareOverlayV3",
        "AgsProxyOverlay",
        WS_POPUP | WS_VISIBLE,
        25, 25, 520, 155,
        NULL, NULL, GetModuleHandle(NULL), NULL
    );

    if (!s_hwnd) {
        DWORD err = GetLastError();
        g_tel.st_overlay = st_fail;
        log_msg("[OVERLAY] [FAIL] CreateWindowExA failed (error=%lu)", err);
        return 1;
    }

    // Set 88% opacity (alpha = 225)
    SetLayeredWindowAttributes(s_hwnd, 0, 225, LWA_ALPHA);
    g_tel.st_overlay = st_ok;
    log_msg("[OVERLAY] Overlay window created at (25, 25). Toggle key: %d", g_cfg.overlay_key);

    MSG msg;
    DWORD lastTick = GetTickCount();

    while (TRUE) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        // Check configured toggle key (default VK_F10) or Insert
        if ((GetAsyncKeyState(g_cfg.overlay_key) & 1) || (GetAsyncKeyState(VK_INSERT) & 1)) {
            s_visible = !s_visible;
            ShowWindow(s_hwnd, s_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        }

        // Refresh once per second
        DWORD now = GetTickCount();
        if (now - lastTick >= 1000) {
            lastTick = now;
            if (s_visible) {
                InvalidateRect(s_hwnd, NULL, TRUE);
            }
        }

        Sleep(40);
    }
    return 0;
}

void overlay_start() {
    if (!g_cfg.overlay) {
        g_tel.st_overlay = st_disabled;
        log_msg("[OVERLAY] Overlay is disabled by config (Overlay=0).");
        return;
    }

    CreateThread(NULL, 0, OverlayThread, NULL, 0, NULL);
}
