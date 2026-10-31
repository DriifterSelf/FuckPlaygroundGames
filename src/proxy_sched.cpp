// Anti-Stutter Hybrid Core Governor for Intel 12th/13th/14th Gen
// Eliminates Thread Director convoy stalls by pinning heavy worker threads to P-Cores
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include "proxy_common.h"

static DWORD_PTR s_p_core_mask = 0;
static DWORD_PTR s_e_core_mask = 0;
static DWORD_PTR s_all_cores_mask = 0;
static volatile BOOL s_governor_running = FALSE;

#define MAX_TRACKED_THREADS 256
struct ThreadSample {
    DWORD tid;
    ULONGLONG last_total_time;
    DWORD last_tick;
    float last_frame_work_ms;
};

static ThreadSample s_tracker[MAX_TRACKED_THREADS] = {};
static int s_tracker_count = 0;

static ThreadSample* FindOrCreateSample(DWORD tid) {
    for (int i = 0; i < s_tracker_count; i++) {
        if (s_tracker[i].tid == tid) return &s_tracker[i];
    }
    if (s_tracker_count < MAX_TRACKED_THREADS) {
        ThreadSample* s = &s_tracker[s_tracker_count++];
        s->tid = tid;
        s->last_total_time = 0;
        s->last_tick = GetTickCount();
        s->last_frame_work_ms = 0.0f;
        return s;
    }
    return NULL;
}

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
        s_all_cores_mask = pMask | eMask;
        g_tel.p_core_mask = pMask;
        g_tel.e_core_mask = eMask;
        g_tel.all_cores_mask = s_all_cores_mask;
        log_msg("[SCHED] Hybrid CPU topology detected: P-Cores=0x%llX, E-Cores=0x%llX, AllCores=0x%llX (ECoreThreshold=%dms)",
                (unsigned long long)s_p_core_mask, (unsigned long long)s_e_core_mask,
                (unsigned long long)s_all_cores_mask, g_cfg.ecore_threshold_ms);
    }
    free(buffer);
}

// Dynamic Anti-Stutter Governor Thread:
// 1. Keeps process affinity mask at 0xFFFF to utilize all 16 cores
// 2. Pins frame-critical interactive threads (<= 8ms) to P-Cores to prevent convoy stall
// 3. Allows sustained heavy / background threads (> 8ms) to offload onto E-Cores for full parallel power
static DWORD WINAPI GovernorThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    DWORD pid = GetCurrentProcessId();

    while (s_governor_running) {
        if (s_p_core_mask != 0 && g_cfg.hybrid_governor) {
            // Ensure process has all 16 cores enabled (never starved of CPU power)
            if (s_all_cores_mask != 0) {
                DWORD_PTR curAff = 0, sysAff = 0;
                GetProcessAffinityMask(GetCurrentProcess(), &curAff, &sysAff);
                if ((curAff & s_all_cores_mask) != s_all_cores_mask) {
                    SetProcessAffinityMask(GetCurrentProcess(), s_all_cores_mask);
                }
            }

            HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (hSnap != INVALID_HANDLE_VALUE) {
                THREADENTRY32 te;
                te.dwSize = sizeof(THREADENTRY32);
                LONG pCorePinned = 0;
                LONG eCoreActive = 0;

                if (Thread32First(hSnap, &te)) {
                    do {
                        if (te.th32OwnerProcessID == pid && te.th32ThreadID != GetCurrentThreadId()) {
                            HANDLE hThread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                            if (hThread) {
                                FILETIME cr, ex, kernel, user;
                                float frameWorkMs = 0.0f;

                                if (GetThreadTimes(hThread, &cr, &ex, &kernel, &user)) {
                                    ULARGE_INTEGER k, u;
                                    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
                                    u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;
                                    ULONGLONG total = k.QuadPart + u.QuadPart;
                                    DWORD now = GetTickCount();

                                    ThreadSample* sample = FindOrCreateSample(te.th32ThreadID);
                                    if (sample && sample->last_total_time > 0) {
                                        DWORD elapsed = now - sample->last_tick;
                                        if (elapsed > 0 && total >= sample->last_total_time) {
                                            float deltaMs = (float)(total - sample->last_total_time) / 10000.0f;
                                            frameWorkMs = (deltaMs / (float)elapsed) * 16.67f;
                                            sample->last_frame_work_ms = frameWorkMs;
                                        }
                                    }
                                    if (sample) {
                                        sample->last_total_time = total;
                                        sample->last_tick = now;
                                    }
                                }

                                // DYNAMIC 8ms SCHEDULING RULE:
                                // 1. Elevated-priority threads (tpBasePri >= 10: main loop, render submission, audio)
                                //    MUST ALWAYS remain on P-Cores (0x0FFF) at 4.6 GHz to prevent convoy stalls.
                                // 2. Heavy sustained background workers (frameWorkMs > ECoreThresholdMs, e.g. > 8ms):
                                //    Offloaded directly to E-Cores (0xF000) so Oodle decompression and chunk streaming
                                //    chew through background work without stealing cycles or cache from P-Cores.
                                // 3. Interactive/lightweight workers (frameWorkMs <= ECoreThresholdMs):
                                //    Pinned to P-Cores (0x0FFF) for low latency.
                                if (te.tpBasePri >= 10) {
                                    SetThreadAffinityMask(hThread, s_p_core_mask);
                                    pCorePinned++;
                                } else if (frameWorkMs > (float)g_cfg.ecore_threshold_ms) {
                                    SetThreadAffinityMask(hThread, s_e_core_mask);
                                    eCoreActive++;
                                } else {
                                    SetThreadAffinityMask(hThread, s_p_core_mask);
                                    pCorePinned++;
                                }

                                CloseHandle(hThread);
                            }
                        }
                    } while (Thread32Next(hSnap, &te));
                }

                g_tel.heavy_threads_pinned = pCorePinned;
                g_tel.ecore_threads_active = eCoreActive;
                CloseHandle(hSnap);
            }
        }
        Sleep(500); // Dynamic evaluation every 500 ms
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
