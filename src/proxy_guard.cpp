// Dynamic OOM assertion and secondary null-dereference crash guard using Vectored Exception Handling
#include <stdio.h>
#include "proxy_common.h"

static PVOID s_veh_handle = NULL;
static uintptr_t s_d3d12core_base = 0;
static uintptr_t s_d3d12core_end = 0;

static void EnsureD3D12CoreBounds() {
    if (s_d3d12core_base == 0) {
        HMODULE hMod = GetModuleHandleW(L"D3D12Core.dll");
        if (hMod) {
            uint8_t* base = (uint8_t*)hMod;
            IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
            if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
                IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE) {
                    s_d3d12core_base = (uintptr_t)base;
                    s_d3d12core_end = s_d3d12core_base + nt->OptionalHeader.SizeOfImage;
                    log_msg("[GUARD] Detected D3D12Core.dll range: %p - %p", (void*)s_d3d12core_base, (void*)s_d3d12core_end);
                }
            }
        }
    }
}

// Robust x86-64 instruction decoder that rescues memory read faults on null or invalid pointers
static bool RescueMemoryReadFault(PEXCEPTION_POINTERS pInfo, uintptr_t faultAddr) {
    __try {
        uint8_t* rip = (uint8_t*)pInfo->ContextRecord->Rip;
        if (!rip) return false;

        // Fast-path: mov rdx, [rcx + 28h] (48 8B 51 28)
        if (rip[0] == 0x48 && rip[1] == 0x8B && rip[2] == 0x51 && rip[3] == 0x28) {
            pInfo->ContextRecord->Rdx = 0;
            pInfo->ContextRecord->Rip += 4;
            return true;
        }

        int prefixLen = 0;
        uint8_t rex = 0;
        while (prefixLen < 8) {
            uint8_t b = rip[prefixLen];
            if (b >= 0x40 && b <= 0x4F) {
                rex = b;
                prefixLen++;
            } else if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
                       b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) {
                prefixLen++;
            } else {
                break;
            }
        }

        uint8_t* p = rip + prefixLen;
        int opcodeLen = 0;
        uint8_t opcode = 0;
        bool isTwoByte = false;

        if (*p == 0x0F) {
            isTwoByte = true;
            opcodeLen = 2;
            opcode = *(p + 1);
        } else {
            opcodeLen = 1;
            opcode = *p;
        }

        p += opcodeLen;

        bool isLoad = false;
        bool isCmp = false;
        if (!isTwoByte) {
            if (opcode == 0x8B || opcode == 0x8A) {
                isLoad = true;
            } else if (opcode >= 0x38 && opcode <= 0x3B) {
                isCmp = true;
            } else if (opcode == 0x85) {
                isCmp = true;
            }
        } else {
            if (opcode == 0xB6 || opcode == 0xB7 || opcode == 0xBE || opcode == 0xBF) {
                isLoad = true;
            } else if (opcode == 0x10 || opcode == 0x28) {
                isLoad = true;
            }
        }

        if (!isLoad && !isCmp) {
            return false;
        }

        uint8_t modrm = *p++;
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t reg = (modrm >> 3) & 7;
        uint8_t rm  = modrm & 7;

        if (mod == 3) {
            return false;
        }

        int instrLen = prefixLen + opcodeLen + 1;

        uint8_t sib_base = 0;
        if (rm == 4) {
            uint8_t sib = *p++;
            instrLen++;
            sib_base = sib & 7;
        }

        if (mod == 0) {
            if (rm == 5 || (rm == 4 && sib_base == 5)) {
                instrLen += 4;
            }
        } else if (mod == 1) {
            instrLen += 1;
        } else if (mod == 2) {
            instrLen += 4;
        }

        if (isCmp && (opcode == 0x80 || opcode == 0x81 || opcode == 0x83)) {
            if (opcode == 0x80 || opcode == 0x83) instrLen += 1;
            else instrLen += 4;
        }

        if (isLoad) {
            uint8_t reg_idx = ((rex & 4) ? 8 : 0) | reg;
            switch (reg_idx) {
                case 0:  pInfo->ContextRecord->Rax = 0; break;
                case 1:  pInfo->ContextRecord->Rcx = 0; break;
                case 2:  pInfo->ContextRecord->Rdx = 0; break;
                case 3:  pInfo->ContextRecord->Rbx = 0; break;
                case 4:  break;
                case 5:  pInfo->ContextRecord->Rbp = 0; break;
                case 6:  pInfo->ContextRecord->Rsi = 0; break;
                case 7:  pInfo->ContextRecord->Rdi = 0; break;
                case 8:  pInfo->ContextRecord->R8  = 0; break;
                case 9:  pInfo->ContextRecord->R9  = 0; break;
                case 10: pInfo->ContextRecord->R10 = 0; break;
                case 11: pInfo->ContextRecord->R11 = 0; break;
                case 12: pInfo->ContextRecord->R12 = 0; break;
                case 13: pInfo->ContextRecord->R13 = 0; break;
                case 14: pInfo->ContextRecord->R14 = 0; break;
                case 15: pInfo->ContextRecord->R15 = 0; break;
            }

            if (isTwoByte && (opcode == 0x10 || opcode == 0x28)) {
                switch (reg_idx) {
                    case 0: pInfo->ContextRecord->Xmm0.Low = pInfo->ContextRecord->Xmm0.High = 0; break;
                    case 1: pInfo->ContextRecord->Xmm1.Low = pInfo->ContextRecord->Xmm1.High = 0; break;
                    case 2: pInfo->ContextRecord->Xmm2.Low = pInfo->ContextRecord->Xmm2.High = 0; break;
                    case 3: pInfo->ContextRecord->Xmm3.Low = pInfo->ContextRecord->Xmm3.High = 0; break;
                    case 4: pInfo->ContextRecord->Xmm4.Low = pInfo->ContextRecord->Xmm4.High = 0; break;
                    case 5: pInfo->ContextRecord->Xmm5.Low = pInfo->ContextRecord->Xmm5.High = 0; break;
                    default: break;
                }
            }
        }

        pInfo->ContextRecord->Rip += instrLen;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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

                // CASE 2: Invalid Array Index Dereference (rax == 0xFFFFFFFF / -1 slot at RVA 0x1E86A40)
                // In ForzaTech particle/audio telemetry emitter update, an unassigned slot returns -1.
                // The engine attempts to compute [r8 + rax*4 + 94h] and [r9 + rax*4 + ACh], which wraps to
                // +16 GB into unmapped memory. Advancing RIP by +0x22 (34 bytes) safely skips this dead slot
                // computation and resumes at 0x1E86A62 (mov eax, dword ptr [r8+484h]).
                if (rva == 0x01E86A40 ||
                    (rip[0] == 0xF3 && rip[1] == 0x41 && rip[2] == 0x0F && rip[3] == 0x10 && rip[4] == 0x84 && rip[5] == 0x80))
                {
                    LONG hit = InterlockedIncrement(&g_tel.guard_hits);
                    InterlockedIncrement(&g_tel.guard_suppressed);
                    log_msg("[GUARD] Rescued unmapped -1 slot dereference #%ld at RIP=%p (RVA=0x%llX, RAX=0x%llX, FaultAddr=0x%llX). Advancing +0x22 to next field.",
                            hit, rip, rva, pInfo->ContextRecord->Rax, faultAddr);
                    pInfo->ContextRecord->Rip += 0x22;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }

                // CASE 3: Secondary NULL/Invalid Memory Read after Assert or Allocation Failure
                // Rescues read faults from NULL pointers ([0x0] to [0x10000]), poison pointers (0xCC...),
                // or uncommitted/unmapped virtual addresses verified via VirtualQuery.
                bool is_bad_addr = (faultAddr < 0x10000 || faultAddr >= 0x7FFF000000000000ULL || (faultAddr & 0xFFFF000000000000ULL) == 0xCCCC000000000000ULL);
                if (!is_bad_addr) {
                    MEMORY_BASIC_INFORMATION mbi = {};
                    if (VirtualQuery((LPCVOID)faultAddr, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
                        is_bad_addr = true;
                    }
                }

                if (isWrite == 0 && is_bad_addr) {
                    if (RescueMemoryReadFault(pInfo, faultAddr)) {
                        LONG hit = InterlockedIncrement(&g_tel.guard_hits);
                        InterlockedIncrement(&g_tel.guard_suppressed);
                        log_msg("[GUARD] Rescued invalid memory read #%ld at RIP=%p (RVA=0x%llX, FaultAddr=0x%llX, Opcode=%02X %02X %02X %02X). Resuming next instruction.",
                                hit, rip, rva, faultAddr, rip[0], rip[1], rip[2], rip[3]);
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }

        EnsureD3D12CoreBounds();

        // Check if crash occurred inside D3D12Core.dll
        if (s_d3d12core_base != 0 && (uintptr_t)rip >= s_d3d12core_base && (uintptr_t)rip < s_d3d12core_end) {
            __try {
                // CASE 3: D3D12Core ValidateBeginEndQuery on NULL QueryHeap
                // Instruction: cmp r8d, dword ptr [rdx+64h] (4 bytes: 44 3B 42 64) with rdx == NULL
                if (isWrite == 0 && (faultAddr == 0x64 || faultAddr < 0x100) && pInfo->ContextRecord->Rdx == 0) {
                    if (rip[0] == 0x44 && rip[1] == 0x3B && rip[2] == 0x42 && rip[3] == 0x64) {
                        LONG hit = InterlockedIncrement(&g_tel.guard_hits);
                        InterlockedIncrement(&g_tel.guard_suppressed);
                        log_msg("[GUARD] Rescued D3D12Core ValidateBeginEndQuery on NULL query heap #%ld at RIP=%p (RDX=0, R8=0x%llX). Bypassing to caller epilogue.",
                                hit, rip, pInfo->ContextRecord->R8);

                        uintptr_t* pRsp = (uintptr_t*)pInfo->ContextRecord->Rsp;
                        pInfo->ContextRecord->R14 = pRsp[6];
                        uintptr_t retAddr = pRsp[7];
                        pInfo->ContextRecord->Rsp += 0x40;

                        uint8_t* pRet = (uint8_t*)retAddr;
                        bool found = false;
                        for (int i = 0; i < 256; i++) {
                            if (pRet[i] == 0x48 && pRet[i+1] == 0x8B && pRet[i+2] == 0x5C && pRet[i+3] == 0x24 && pRet[i+4] == 0x68) {
                                pInfo->ContextRecord->Rip = (DWORD64)(pRet + i);
                                found = true;
                                break;
                            }
                        }
                        if (!found) {
                            pInfo->ContextRecord->Rip = (DWORD64)retAddr;
                        }
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }

                // CASE 4: Secondary NULL-read inside D3D12Core.dll
                if (isWrite == 0 && faultAddr < 0x1000) {
                    if (RescueMemoryReadFault(pInfo, faultAddr)) {
                        LONG hit = InterlockedIncrement(&g_tel.guard_hits);
                        InterlockedIncrement(&g_tel.guard_suppressed);
                        uintptr_t core_rva = (uintptr_t)rip - s_d3d12core_base;
                        log_msg("[GUARD] Rescued D3D12Core NULL-read #%ld at RIP=%p (CoreRVA=0x%llX, FaultAddr=0x%llX, Opcode=%02X %02X %02X %02X). Resuming next instruction.",
                                hit, rip, core_rva, faultAddr, rip[0], rip[1], rip[2], rip[3]);
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        uintptr_t unh_rva = ((uintptr_t)rip >= g_tel.exe_base && (uintptr_t)rip < g_tel.exe_end) ? ((uintptr_t)rip - g_tel.exe_base) : 0;
        log_msg("[GUARD] [UNHANDLED] Access Violation at RIP=%p (RVA=0x%llX, FaultAddr=0x%llX, Write=%lu, Opcode=%02X %02X %02X %02X %02X %02X %02X %02X, RAX=0x%llX).",
                rip, unh_rva, faultAddr, isWrite,
                rip[0], rip[1], rip[2], rip[3], rip[4], rip[5], rip[6], rip[7],
                pInfo->ContextRecord->Rax);
        InterlockedIncrement(&g_tel.guard_passed);
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
