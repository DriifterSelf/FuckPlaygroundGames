// Dynamic DXGI VRAM budget uncap hook
#include <dxgi1_4.h>
#include <stdio.h>
#include "proxy_common.h"

typedef HRESULT (STDMETHODCALLTYPE *PFN_QueryVideoMemoryInfo)(
    IDXGIAdapter3* This,
    UINT NodeIndex,
    DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
    DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo
);

static PFN_QueryVideoMemoryInfo s_orig_qvmi = NULL;
static UINT64 s_last_logged_budget = 0;
static DWORD s_last_log_tick = 0;
static UINT64 s_cached_dedicated = 0;
static UINT64 s_cached_max_safe_budget = 0;

static HRESULT STDMETHODCALLTYPE Hook_QueryVideoMemoryInfo(
    IDXGIAdapter3* This,
    UINT NodeIndex,
    DXGI_MEMORY_SEGMENT_GROUP MemorySegmentGroup,
    DXGI_QUERY_VIDEO_MEMORY_INFO* pVideoMemoryInfo
) {
    HRESULT hr = s_orig_qvmi(This, NodeIndex, MemorySegmentGroup, pVideoMemoryInfo);
    if (SUCCEEDED(hr) && pVideoMemoryInfo != NULL && MemorySegmentGroup == DXGI_MEMORY_SEGMENT_GROUP_LOCAL) {
        InterlockedIncrement(&g_tel.vram_calls);
        g_tel.current_usage_bytes = pVideoMemoryInfo->CurrentUsage;

        if (g_cfg.vram_uncap) {
            // High-performance cached budget calculation (0 syscalls, < 5ns overhead)
            if (s_cached_max_safe_budget == 0) {
                DXGI_ADAPTER_DESC desc;
                if (SUCCEEDED(This->GetDesc(&desc)) && desc.DedicatedVideoMemory > 0) {
                    s_cached_dedicated = desc.DedicatedVideoMemory;
                    UINT64 min_reserve = 768ULL * 1024ULL * 1024ULL;
                    UINT64 margin = (desc.DedicatedVideoMemory * (UINT64)g_cfg.vram_margin_percent) / 100ULL;
                    if (margin < min_reserve) {
                        margin = min_reserve;
                    }
                    s_cached_max_safe_budget = (desc.DedicatedVideoMemory > margin)
                        ? (desc.DedicatedVideoMemory - margin)
                        : desc.DedicatedVideoMemory;
                }
            }

            if (s_cached_max_safe_budget > 0) {
                g_tel.dedicated_bytes = s_cached_dedicated;
                g_tel.os_budget_bytes = pVideoMemoryInfo->Budget;

                UINT64 usage = pVideoMemoryInfo->CurrentUsage;
                UINT64 os_budget = pVideoMemoryInfo->Budget;

                // Base target budget is physical safe limit or OS budget, whichever is higher
                UINT64 target_budget = (s_cached_max_safe_budget > os_budget)
                                     ? s_cached_max_safe_budget
                                     : os_budget;

                // CRITICAL HEADROOM & OOM PREVENTION:
                // Maintain at least 512 MB headroom above current usage at all times!
                // ABSOLUTE INTEGRITY RULE: Never allow target_budget < usage!
                // Clamping target_budget below CurrentUsage causes DXGI QueryVideoMemoryInfo
                // to report over-budget, which immediately triggers ForzaTech's internal
                // fail-fast assert 0x8007000E (E_OUTOFMEMORY) when VRAM usage is high.
                const UINT64 min_headroom = 512ULL * 1024ULL * 1024ULL;
                if (target_budget < usage + min_headroom) {
                    target_budget = usage + min_headroom;
                }

                pVideoMemoryInfo->Budget = target_budget;
                g_tel.reported_budget_bytes = target_budget;
                InterlockedIncrement(&g_tel.vram_uncap_hits);

                // Zero-stutter rate-limited logging: maximum once every 2.5 seconds and only on significant shifts
                DWORD now = GetTickCount();
                if ((now - s_last_log_tick > 2500) &&
                    (target_budget > s_last_logged_budget + 64ULL * 1024ULL * 1024ULL ||
                     s_last_logged_budget > target_budget + 64ULL * 1024ULL * 1024ULL ||
                     s_last_logged_budget == 0)) {
                    s_last_logged_budget = target_budget;
                    s_last_log_tick = now;
                    log_msg("[VRAM] Dynamically adjusted budget: %llu MB (Usage: %llu MB, OS Budget: %llu MB, Dedicated: %llu MB, Margin: %d%%)",
                            target_budget / (1024 * 1024),
                            usage / (1024 * 1024),
                            g_tel.os_budget_bytes / (1024 * 1024),
                            s_cached_dedicated / (1024 * 1024),
                            g_cfg.vram_margin_percent);
                }
            }
        } else {
            g_tel.reported_budget_bytes = pVideoMemoryInfo->Budget;
            InterlockedIncrement(&g_tel.vram_passed);
        }
    }
    return hr;
}

static void apply_hook_to_adapter(IDXGIAdapter* pAdapter) {
    if (!pAdapter) return;

    DXGI_ADAPTER_DESC desc;
    if (SUCCEEDED(pAdapter->GetDesc(&desc))) {
        g_tel.dedicated_bytes = desc.DedicatedVideoMemory;
    }

    IDXGIAdapter3* pAdapter3 = NULL;
    if (SUCCEEDED(pAdapter->QueryInterface(IID_PPV_ARGS(&pAdapter3)))) {
        void** vtbl = *(void***)pAdapter3;
        if (vtbl && vtbl[14] != (void*)Hook_QueryVideoMemoryInfo) {
            DWORD oldProtect;
            if (VirtualProtect(&vtbl[14], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
                s_orig_qvmi = (PFN_QueryVideoMemoryInfo)vtbl[14];
                vtbl[14] = (void*)Hook_QueryVideoMemoryInfo;
                VirtualProtect(&vtbl[14], sizeof(void*), oldProtect, &oldProtect);
                g_tel.st_vram = st_ok;
                log_msg("[VRAM] Successfully installed hook on IDXGIAdapter3::QueryVideoMemoryInfo (vtbl[14]).");
            } else {
                DWORD err = GetLastError();
                g_tel.st_vram = st_fail;
                sprintf_s(g_tel.vram_err, "VirtualProtect failed: %lu", err);
                log_msg("[VRAM] [FAIL] VirtualProtect on vtbl[14] failed (error=%lu)", err);
            }
        } else if (vtbl && vtbl[14] == (void*)Hook_QueryVideoMemoryInfo) {
            g_tel.st_vram = st_ok;
        }
        pAdapter3->Release();
    } else {
        g_tel.st_vram = st_fail;
        sprintf_s(g_tel.vram_err, "QueryInterface IDXGIAdapter3 failed");
        log_msg("[VRAM] [FAIL] Could not query IDXGIAdapter3 interface from adapter.");
    }
}

void vram_hook_install() {
    if (!g_cfg.vram_uncap) {
        g_tel.st_vram = st_disabled;
        log_msg("[VRAM] VRAM uncap is disabled by config (VramUncap=0).");
        return;
    }

    if (g_tel.st_vram == st_ok) {
        return; // Already hooked
    }

    IDXGIFactory1* pFactory = NULL;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&pFactory));
    if (SUCCEEDED(hr) && pFactory) {
        IDXGIAdapter1* pAdapter1 = NULL;
        bool hooked = false;
        for (UINT i = 0; pFactory->EnumAdapters1(i, &pAdapter1) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc1;
            if (SUCCEEDED(pAdapter1->GetDesc1(&desc1))) {
                if ((desc1.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && desc1.DedicatedVideoMemory > 0) {
                    apply_hook_to_adapter((IDXGIAdapter*)pAdapter1);
                    pAdapter1->Release();
                    hooked = true;
                    break;
                }
            }
            pAdapter1->Release();
        }
        if (!hooked && g_tel.st_vram != st_ok) {
            g_tel.st_vram = st_fail;
            sprintf_s(g_tel.vram_err, "No suitable DXGI hardware adapter found");
            log_msg("[VRAM] [FAIL] No suitable DXGI hardware adapter found.");
        }
        pFactory->Release();
    } else {
        g_tel.st_vram = st_fail;
        sprintf_s(g_tel.vram_err, "CreateDXGIFactory1 hr=0x%08X", hr);
        log_msg("[VRAM] [FAIL] CreateDXGIFactory1 failed hr=0x%08X", hr);
    }
}
