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
                // Found at RVA 0x34BA060, 0x34C11D0, 0x34C1245 in forzahorizon6.exe
                // Calling context: ForzaTech internal crash trap / panic assertion handler.
                // Deliberately triggers access violation on null-write to force process termination.
                // Suppressing this instruction and advancing RIP by 11 bytes safely bypasses the panic
                // and allows the engine to recover or continue normal execution.
                if (isWrite == 1 && faultAddr == 0) {
                    if (rip[0] == 0xC7 && rip[1] == 0x04 && rip[2] == 0x25 &&
                        *(uint32_t*)(rip + 3) == 0 && *(uint32_t*)(rip + 7) == 0) {

                        DWORD64 rax = pInfo->ContextRecord->Rax;
                        DWORD64 rbx = pInfo->ContextRecord->Rbx;

                        LONG hit = InterlockedIncrement(&g_tel.guard_hits);

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

                // CASE 3: Secondary NULL/Poison Dereference & Dead Object Cleanup
                // Instruction reading from null address ([0x0] to [0x10000]) or poison pointer (0xCC...)
                // Found at RVA 0x33B6E83, 0x33BDFE3, 0x33BDFE6, and 0x5BF0261 in forzahorizon6.exe
                bool is_bad_addr = (faultAddr < 0x10000 || faultAddr >= 0x7FFF000000000000ULL || (faultAddr & 0xFFFF000000000000ULL) == 0xCCCC000000000000ULL);
                if (isWrite == 0 && is_bad_addr) {
                    if (rva == 0x05BF0261 || (rva >= 0x05BE0000 && rva <= 0x05C10000) ||
                        rva == 0x033B6E83 || rva == 0x033BDFE3 || (rva >= 0x033B0000 && rva <= 0x033E0000)) {
                        LONG hit = InterlockedIncrement(&g_tel.guard_hits);
                        InterlockedIncrement(&g_tel.guard_suppressed);

                        log_msg("[GUARD] Intercepted secondary invalid dereference #%ld at RIP=%p (RVA=0x%llX, FaultAddr=0x%llX, Opcode=%02X %02X %02X %02X). Rescuing context.",
                                hit, rip, rva, faultAddr, rip[0], rip[1], rip[2], rip[3]);

                        // Robust instruction length decoder for x86-64 memory load patterns:
                        // Handles general-purpose MOV, MOVZX, MOVSX, and SSE/AVX vector loads (MOVAPS, MOVUPS, MOVSS, MOVSD, MOVQ)
                        int instrLen = 3;
                        uint8_t* p = rip;
                        int prefixLen = 0;
                        while (*p == 0xF3 || *p == 0xF2 || *p == 0x66 || (*p >= 0x40 && *p <= 0x4F)) {
                            p++;
                            prefixLen++;
                            if (prefixLen > 4) break;
                        }

                        if (*p == 0x8B || *p == 0x8A || *p == 0x38 || *p == 0x39 || *p == 0x3A || *p == 0x3B || *p == 0x85) {
                            p++; // Skip opcode
                            uint8_t modrm = *p++;
                            uint8_t mod = modrm >> 6;
                            uint8_t rm = modrm & 7;
                            instrLen = prefixLen + 2;
                            if (rm == 4 && mod != 3) { p++; instrLen++; } // SIB
                            if (mod == 1) { instrLen += 1; }              // disp8
                            else if (mod == 2 || (mod == 0 && rm == 5)) { instrLen += 4; } // disp32
                        } else if (*p == 0xFF) {
                            p++; // Skip 0xFF (indirect call/jmp: e.g. FF 50 28)
                            uint8_t modrm = *p++;
                            uint8_t mod = modrm >> 6;
                            uint8_t rm = modrm & 7;
                            instrLen = prefixLen + 2;
                            if (rm == 4 && mod != 3) { p++; instrLen++; } // SIB
                            if (mod == 1) { instrLen += 1; }              // disp8
                            else if (mod == 2 || (mod == 0 && rm == 5)) { instrLen += 4; } // disp32
                        } else if (*p == 0x0F) {
                            p++; // Skip 0x0F
                            uint8_t op2 = *p++;
                            uint8_t modrm = *p++;
                            uint8_t mod = modrm >> 6;
                            uint8_t rm = modrm & 7;
                            instrLen = prefixLen + 3;
                            if (rm == 4 && mod != 3) { p++; instrLen++; } // SIB
                            if (mod == 1) { instrLen += 1; }              // disp8
                            else if (mod == 2 || (mod == 0 && rm == 5)) { instrLen += 4; } // disp32
                        } else if (rip[0] == 0x48 && rip[1] == 0x8B) {
                            uint8_t modrm = rip[2];
                            uint8_t mod = modrm >> 6;
                            if (mod == 0) {
                                if ((modrm & 7) == 4) instrLen = 4;
                                else if ((modrm & 7) == 5) instrLen = 7;
                                else instrLen = 3;
                            } else if (mod == 1) {
                                instrLen = ((modrm & 7) == 4) ? 5 : 4;
                            } else if (mod == 2) {
                                instrLen = ((modrm & 7) == 4) ? 8 : 7;
                            }
                        }

                        // Safely zero target registers to prevent propagation of corrupt pointers
                        pInfo->ContextRecord->Rax = 0;
                        pInfo->ContextRecord->Rdx = 0;
                        pInfo->ContextRecord->Rcx = 0;
                        pInfo->ContextRecord->R8 = 0;
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
