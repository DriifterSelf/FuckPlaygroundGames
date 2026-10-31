// Anti-Stutter Hybrid Core Governor for Intel 12th/13th/14th Gen
// Eliminates Thread Director convoy stalls by pinning heavy worker threads to P-Cores
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include "proxy_common.h"

static DWORD_PTR s_p_core_mask = 0;
static DWORD_PTR s_e_core_mask = 0;
static volatile BOOL s_governor_running = FALSE;

// Detects P-cores vs E-cores on Intel hybrid architecture
static void DetectHybridTopology() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (len == 0) return;

    BYTE* buffer = (BYTE*)malloc(len);
    if (!buffer) return;

    if (GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer, &len)) {
        BYTE* ptr = buffer;
        DWORD_PTR pMask = 0;
        DWORD_PTR eMask = 0;
        BYTE maxEfficiency = 0;

        // First pass: find max efficiency class (P-cores have higher EfficiencyClass on Intel)
        while (ptr < buffer + len) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)ptr;
            if (info->Relationship == RelationProcessorCore) {
                if (info->Processor.EfficiencyClass > maxEfficiency) {
                    maxEfficiency = info->Processor.EfficiencyClass;
                }
            }
            ptr += info->Size;
        }

        // Second pass: build bitmasks
        ptr = buffer;
        while (ptr < buffer + len) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)ptr;
            if (info->Relationship == RelationProcessorCore) {
                DWORD_PTR mask = info->Processor.GroupMask[0].Mask;
                if (info->Processor.EfficiencyClass == maxEfficiency && maxEfficiency > 0) {
                    pMask |= mask;
                } else if (maxEfficiency > 0) {
                    eMask |= mask;
                }
            }
            ptr += info->Size;
        }

        // Fallback for i5-13400 (6P + 4E) if efficiency class is uniform
        if (pMask == 0 || eMask == 0) {
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            if (si.dwNumberOfProcessors == 16) {
                pMask = 0x0FFF; // Cores 0-11 (6P cores with hyperthreading)
                eMask = 0xF000; // Cores 12-15 (4E cores)
            }
        }

        s_p_core_mask = pMask;
        s_e_core_mask = eMask;
        log_msg("[SCHED] Hybrid CPU topology detected: P-Cores Mask=0x%llX, E-Cores Mask=0x%llX",
                (unsigned long long)s_p_core_mask, (unsigned long long)s_e_core_mask);
    }
    free(buffer);
}

// Background governor thread that ensures heavy game worker threads are never choked on E-cores
static DWORD WINAPI GovernorThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    DWORD pid = GetCurrentProcessId();

    while (s_governor_running) {
        if (s_p_core_mask != 0 && g_cfg.hybrid_governor) {
            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hSnap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te;
                te.dwSize = sizeof(THREADENTRY32);
                if (Thread32First(hSnap, &te)) {
                    do {
                        if (te.th32OwnerProcessID == pid && te.th32ThreadID != GetCurrentThreadId()) {
                            // Target high-priority worker and render threads (tpBasePri >= 1)
                            if (te.tpBasePri >= 1) {
                                HANDLE hThread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                                if (hThread) {
                                    SetThreadAffinityMask(hThread, s_p_core_mask);
                                    CloseHandle(hThread);
                                }
                            }
                        }
                    } while (Thread32Next(hSnap, &te));
                }
                CloseHandle(hSnap);
            }
        }
        Sleep(2000); // Check every 2 seconds
    }
    return 0;
}

void sched_governor_start() {
    if (!g_cfg.hybrid_governor) {
        log_msg("[SCHED] Hybrid Core Governor is disabled by config (HybridCoreGovernor=0).");
        return;
    }

    DetectHybridTopology();
    if (s_p_core_mask != 0) {
        s_governor_running = TRUE;
        HANDLE hThread = CreateThread(NULL, 0, GovernorThread, NULL, 0, NULL);
        if (hThread) {
            CloseHandle(hThread);
            log_msg("[SCHED] Anti-stutter hybrid core governor started successfully.");
        }
    }
}
