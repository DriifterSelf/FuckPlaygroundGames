// Dynamic OOM assertion guard using Vectored Exception Handling
#include <stdio.h>
#include "proxy_common.h"

static PVOID s_veh_handle = NULL;

static LONG WINAPI DynamicCrashGuard(PEXCEPTION_POINTERS pInfo) {
    if (pInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t faultAddr = (uintptr_t)pInfo->ExceptionRecord->ExceptionInformation[1];
        uint8_t* rip = (uint8_t*)pInfo->ContextRecord->Rip;

        // 1. Must be a write to address 0
        if (faultAddr == 0 && rip != NULL) {
            // 2. Must be within the game executable address range
            if ((uintptr_t)rip >= g_tel.exe_base && (uintptr_t)rip < g_tel.exe_end) {
                __try {
                    // 3. Must match the exact fail-fast instruction: mov dword ptr [0], 0 (11 bytes: C7 04 25 00 00 00 00 00 00 00 00)
                    if (rip[0] == 0xC7 && rip[1] == 0x04 && rip[2] == 0x25 &&
                        *(uint32_t*)(rip + 3) == 0 && *(uint32_t*)(rip + 7) == 0) {

                        DWORD64 rax = pInfo->ContextRecord->Rax;
                        DWORD64 rbx = pInfo->ContextRecord->Rbx;

                        // 4. Must be associated with an Out of Memory error code in RAX or RBX
                        // 0x8007000E = E_OUTOFMEMORY, 0x887A002B = DXGI_ERROR_OUT_OF_MEMORY
                        bool is_oom = (rax == 0x8007000E || rbx == 0x8007000E ||
                                       (DWORD)rax == 0x8007000E || (DWORD)rbx == 0x8007000E ||
                                       rax == 0x887A002B || rbx == 0x887A002B);

                        LONG hit = InterlockedIncrement(&g_tel.guard_hits);

                        if (is_oom) {
                            if (hit <= (LONG)g_cfg.oom_guard_max) {
                                InterlockedIncrement(&g_tel.guard_suppressed);
                                log_msg("[GUARD] Suppressed fail-fast NULL-write assertion #%ld at RIP=%p (RAX=0x%llX, RBX=0x%llX). Advancing RIP by 11 bytes.",
                                        hit, rip, rax, rbx);
                                pInfo->ContextRecord->Rip += 11;
                                return EXCEPTION_CONTINUE_EXECUTION;
                            } else {
                                InterlockedIncrement(&g_tel.guard_passed);
                                log_msg("[GUARD] [LIMIT] Exceeded maximum allowed suppressions (%d). Letting exception pass to crash handler.",
                                        g_cfg.oom_guard_max);
                            }
                        } else {
                            InterlockedIncrement(&g_tel.guard_passed);
                            log_msg("[GUARD] [PASS] Null-write matched instruction at %p but error codes (RAX=0x%llX, RBX=0x%llX) were not OOM. Passing through.",
                                    rip, rax, rbx);
                        }
                    }
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void guard_install() {
    if (!g_cfg.oom_guard) {
        g_tel.st_guard = st_disabled;
        log_msg("[GUARD] OOM Guard is disabled by config (OomGuard=0).");
        return;
    }

    s_veh_handle = AddVectoredExceptionHandler(1, DynamicCrashGuard);
    if (s_veh_handle) {
        g_tel.st_guard = st_ok;
        log_msg("[GUARD] Vectored Exception Handler installed successfully (dynamic range: %p-%p, max=%d).",
                (void*)g_tel.exe_base, (void*)g_tel.exe_end, g_cfg.oom_guard_max);
    } else {
        DWORD err = GetLastError();
        g_tel.st_guard = st_fail;
        log_msg("[GUARD] [FAIL] AddVectoredExceptionHandler failed (error=%lu).", err);
    }
}
