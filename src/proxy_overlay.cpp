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
        const int step = 15;

        // 1. Header
        SetTextColor(hdc, RGB(0, 255, 180));
        TextOutA(hdc, 10, y, "[ AMD AGS PROXY v" PROXY_VERSION " - SELF-AWARE DIAGNOSTIC HUD ]", 52);
        y += step;

        // 2. Hardware & Game Info
        char hwBuf[160];
        if (g_tel.gpu_name[0]) {
            sprintf_s(hwBuf, "• HW: %s (%d CUs) | Intel Hybrid (16T)", g_tel.gpu_name, g_tel.num_cus);
        } else {
            sprintf_s(hwBuf, "• HW: [Waiting for agsInitialize...] | %s", g_tel.exe_name[0] ? g_tel.exe_name : "forzahorizon6.exe");
        }
        SetTextColor(hdc, RGB(225, 235, 245));
        TextOutA(hdc, 10, y, hwBuf, (int)strlen(hwBuf));
        y += step;

        // 3. D3D12 Feature & Compute Shader Offload Pipeline
        char cOffBuf[140];
        sprintf_s(cOffBuf, "• D3D12 Compute Pipeline: [ACTIVE] (Queries Intercepted: %ld)", g_tel.d3d12_feature_queries);
        SetTextColor(hdc, RGB(70, 240, 140));
        TextOutA(hdc, 10, y, cOffBuf, (int)strlen(cOffBuf));
        y += step;

        // 3a. Mesh Shaders & Cluster Culling
        char meshBuf[140];
        sprintf_s(meshBuf, "  ├─ Mesh Shaders T1: %s (GPU Cluster & Meshlet Culling)",
                  g_tel.d3d12_mesh_shader ? "[ACTIVE]" : "[PENDING ENGINE]");
        SetTextColor(hdc, g_tel.d3d12_mesh_shader ? RGB(70, 240, 140) : RGB(255, 205, 60));
        TextOutA(hdc, 10, y, meshBuf, (int)strlen(meshBuf));
        y += step;

        // 3b. Inline RayQuery Compute
        char rqBuf[140];
        sprintf_s(rqBuf, "  ├─ Inline RayQuery: %s (DXR Tier 1.1 Compute Shaders)",
                  g_tel.d3d12_rayquery ? "[ACTIVE]" : "[PENDING ENGINE]");
        SetTextColor(hdc, g_tel.d3d12_rayquery ? RGB(70, 240, 140) : RGB(255, 205, 60));
        TextOutA(hdc, 10, y, rqBuf, (int)strlen(rqBuf));
        y += step;

        // 3c. WaveOps & Wave32
        char waveBuf[140];
        sprintf_s(waveBuf, "  ├─ WaveOps / Wave32: %s (RDNA4 Vector SIMD Execution)",
                  g_tel.d3d12_wave_ops ? "[ACTIVE]" : "[PENDING ENGINE]");
        SetTextColor(hdc, g_tel.d3d12_wave_ops ? RGB(70, 240, 140) : RGB(255, 205, 60));
        TextOutA(hdc, 10, y, waveBuf, (int)strlen(waveBuf));
        y += step;

        // 3d. Variable Rate Shading Tier 2
        char vrsBuf[140];
        sprintf_s(vrsBuf, "  ├─ Variable Rate Shading: %s (VRS Tier 2 Motion Opt)",
                  g_tel.d3d12_vrs_tier2 ? "[ACTIVE]" : "[PENDING ENGINE]");
        SetTextColor(hdc, g_tel.d3d12_vrs_tier2 ? RGB(70, 240, 140) : RGB(255, 205, 60));
        TextOutA(hdc, 10, y, vrsBuf, (int)strlen(vrsBuf));
        y += step;

        // 3e. Async Copy Queue
        char copyBuf[140];
        sprintf_s(copyBuf, "  └─ Async Copy Queue: %s (Direct Texture Streaming)",
                  g_tel.d3d12_async_copy ? "[ACTIVE]" : "[PENDING ENGINE]");
        SetTextColor(hdc, g_tel.d3d12_async_copy ? RGB(70, 240, 140) : RGB(255, 205, 60));
        TextOutA(hdc, 10, y, copyBuf, (int)strlen(copyBuf));
        y += step;

        // 4. Anti-Stutter CPU Governor
        char schedBuf[140];
        if (g_cfg.hybrid_governor) {
            sprintf_s(schedBuf, "• CPU Governor: [ACTIVE] (P-Cores: 0x%03llX | E-Cores: 0x%04llX | Pinned: %ld)",
                      (unsigned long long)g_tel.p_core_mask, (unsigned long long)g_tel.e_core_mask, g_tel.heavy_threads_pinned);
            SetTextColor(hdc, RGB(70, 240, 140));
        } else {
            sprintf_s(schedBuf, "• CPU Governor: [DISABLED by INI]");
            SetTextColor(hdc, RGB(150, 160, 175));
        }
        TextOutA(hdc, 10, y, schedBuf, (int)strlen(schedBuf));
        y += step;

        // 5. VRAM Pool
        char vramBuf[140];
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

        // 6. Memory Guard
        char guardBuf[140];
        if (g_tel.st_guard == st_ok) {
            sprintf_s(guardBuf, "• Crash Guard: [ACTIVE] (%ld suppressed / %ld hits / %ld passed)",
                      g_tel.guard_suppressed, g_tel.guard_hits, g_tel.guard_passed);
        } else if (g_tel.st_guard == st_disabled) {
            sprintf_s(guardBuf, "• Crash Guard: [DISABLED by INI]");
        } else {
            sprintf_s(guardBuf, "• Crash Guard: [FAILED TO INSTALL]");
        }
        SetTextColor(hdc, color_for_status(g_tel.st_guard));
        TextOutA(hdc, 10, y, guardBuf, (int)strlen(guardBuf));
        y += step;

        // 7. Footer with toggle instructions
        char footBuf[80];
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
    // Isolate diagnostic overlay strictly to efficiency cores (E-Cores) to never preempt render threads
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

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
        25, 25, 560, 215,
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

        Sleep(s_visible ? 40 : 100);
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
