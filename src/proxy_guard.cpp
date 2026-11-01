// Dynamic OOM assertion and secondary null-dereference crash guard using Vectored Exception Handling
#include <stdio.h>
#include "proxy_common.h"

static PVOID s_veh_handle = NULL;

static LONG WINAPI DynamicCrashGuard(PEXCEPTION_POINTERS pInfo) {
    if (pInfo->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR isWrite = pInfo->ExceptionRecord->ExceptionInformation[0];
        uintptr_t faultAddr = (uintptr_t)pInfo->ExceptionRecord->ExceptionInformation[1];
        uint8_t* rip = (uint8_t*)pInfo->ContextRecord->Rip;

        if (rip == NULL) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // Must be within the game executable address space
        if ((uintptr_t)rip >= g_tel.exe_base && (uintptr_t)rip < g_tel.exe_end) {
            uintptr_t rva = (uintptr_t)rip - g_tel.exe_base;

            __try {
                // CASE 1: Primary Fail-Fast Assertion (Write to Address 0x0)
                // Instruction: mov dword ptr [0], 0 (11 bytes: C7 04 25 00 00 00 00 00 00 00 00)
                // Found at RVA 0x34BA060 in forzahorizon6.exe
                if (isWrite == 1 && faultAddr == 0) {
                    if (rip[0] == 0xC7 && rip[1] == 0x04 && rip[2] == 0x25 &&
                        *(uint32_t*)(rip + 3) == 0 && *(uint32_t*)(rip + 7) == 0) {

                        DWORD64 rax = pInfo->ContextRecord->Rax;
                        DWORD64 rbx = pInfo->ContextRecord->Rbx;

                        // Check if associated with an Out of Memory error code in RAX or RBX
                        // 0x8007000E = E_OUTOFMEMORY, 0x887A002B = DXGI_ERROR_OUT_OF_MEMORY
                        bool is_oom = (rax == 0x8007000E || rbx == 0x8007000E ||
                                       (DWORD)rax == 0x8007000E || (DWORD)rbx == 0x8007000E ||
                                       rax == 0x887A002B || rbx == 0x887A002B);

                        LONG hit = InterlockedIncrement(&g_tel.guard_hits);

                        if (is_oom) {
                            if (hit <= (LONG)g_cfg.oom_guard_max) {
                                InterlockedIncrement(&g_tel.guard_suppressed);
                                log_msg("[GUARD] Suppressed fail-fast NULL-write assertion #%ld at RIP=%p (RVA=0x%llX, RAX=0x%llX, RBX=0x%llX). Advancing past assert.",
                                        hit, rip, rva, rax, rbx);
                                pInfo->ContextRecord->Rip += 11;
                                return EXCEPTION_CONTINUE_EXECUTION;
                            } else {
                                InterlockedIncrement(&g_tel.guard_passed);
                                log_msg("[GUARD] [LIMIT] Exceeded maximum allowed suppressions (%d). Letting exception pass.",
                                        g_cfg.oom_guard_max);
                            }
                        } else {
                            InterlockedIncrement(&g_tel.guard_passed);
                            log_msg("[GUARD] [PASS] Null-write at %p (RVA=0x%llX) matched instruction but error codes (RAX=0x%llX, RBX=0x%llX) were not OOM. Passing through.",
                                    rip, rva, rax, rbx);
                        }
                    }
                }

                // CASE 2: Secondary NULL-Dereference after Failed Allocation
                // Instruction: mov rdx, [rcx + 28h] (4 bytes: 48 8B 51 28) with rcx == 0
                // Found at RVA 0xFECFCD in forzahorizon6.exe
                // Calling context: fallback allocation failed, [rsi] is NULL, function attempts
                // to dereference [rcx + 0x28] to store output pointer before jumping to epilogue.
                if (isWrite == 0 && (faultAddr == 0x28 || (faultAddr < 0x100 && pInfo->ContextRecord->Rcx == 0))) {
                    if (rip[0] == 0x48 && rip[1] == 0x8B && rip[2] == 0x51 && rip[3] == 0x28) {
                        LONG hit = InterlockedIncrement(&g_tel.guard_hits);
                        InterlockedIncrement(&g_tel.guard_suppressed);

                        log_msg("[GUARD] Intercepted secondary NULL-dereference #%ld at RIP=%p (RVA=0x%llX, FaultAddr=0x%llX). Nullifying output and jumping to safe epilogue.",
                                hit, rip, rva, faultAddr);

                        pInfo->ContextRecord->Rdx = 0;
                        pInfo->ContextRecord->Rax = 0; // Set return value to false (0)

                        // Safely zero-out destination resource pointer if slot [rsp + 0xC0] is valid
                        uintptr_t rsp = pInfo->ContextRecord->Rsp;
                        uintptr_t* pOutSlot = (uintptr_t*)(rsp + 0xC0);
                        if (!IsBadReadPtr(pOutSlot, sizeof(uintptr_t)) && *pOutSlot != 0) {
                            uintptr_t outDest = *pOutSlot;
                            if (!IsBadWritePtr((void*)outDest, sizeof(uintptr_t))) {
                                *(uintptr_t*)outDest = 0;
                            }
                        }

                        // Jump directly to function epilogue at 0xFED06A (rip + 0x9D)
                        pInfo->ContextRecord->Rip = (DWORD64)(rip + 0x9D);
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }

                // CASE 3: Secondary NULL-Read Dereference on 3D Model / Vertex Buffers
                // Instruction reading from null address ([0x0]) at RVA 0x33B6E83
                if (isWrite == 0 && (faultAddr == 0 || faultAddr < 0x100)) {
                    if (rva == 0x033B6E83 || (rva >= 0x033B6000 && rva <= 0x033B8000)) {
                        LONG hit = InterlockedIncrement(&g_tel.guard_hits);
                        InterlockedIncrement(&g_tel.guard_suppressed);

                        log_msg("[GUARD] Intercepted secondary NULL-read dereference #%ld at RIP=%p (RVA=0x%llX, FaultAddr=0x%llX, Opcode=%02X %02X %02X %02X). Rescuing context.",
                                hit, rip, rva, faultAddr, rip[0], rip[1], rip[2], rip[3]);

                        // Decode instruction length for common x86-64 MOV patterns:
                        int instrLen = 3;
                        if (rip[0] == 0x48 && rip[1] == 0x8B) {
                            uint8_t modrm = rip[2];
                            uint8_t mod = modrm >> 6;
                            if (mod == 0) {
                                if ((modrm & 7) == 4) instrLen = 4; // SIB byte
                                else if ((modrm & 7) == 5) instrLen = 7; // disp32
                                else instrLen = 3;
                            } else if (mod == 1) {
                                instrLen = ((modrm & 7) == 4) ? 5 : 4; // disp8
                            } else if (mod == 2) {
                                instrLen = ((modrm & 7) == 4) ? 8 : 7; // disp32
                            }
                        } else if (rip[0] == 0x8B) {
                            uint8_t modrm = rip[1];
                            uint8_t mod = modrm >> 6;
                            if (mod == 0) instrLen = 2;
                            else if (mod == 1) instrLen = 3;
                            else if (mod == 2) instrLen = 6;
                        }

                        pInfo->ContextRecord->Rax = 0;
                        pInfo->ContextRecord->Rip += instrLen;
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
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
