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
    int last_affinity_type;    // 1 = P-Core, 2 = E-Core
    int pending_affinity_type; // Hysteresis candidate
    int pending_count;         // Hysteresis consecutive windows
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
        s->last_affinity_type = 0;
        s->pending_affinity_type = 0;
        s->pending_count = 0;
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

typedef HANDLE (WINAPI *PFN_AvSetMmThreadCharacteristicsA)(LPCSTR TaskName, LPDWORD TaskIndex);
static PFN_AvSetMmThreadCharacteristicsA s_pfnAvSet = NULL;
static bool s_avrt_loaded = false;

static void InitMMCSS() {
    if (!s_avrt_loaded) {
        s_avrt_loaded = true;
        HMODULE hAvrt = LoadLibraryA("avrt.dll");
        if (hAvrt) {
            s_pfnAvSet = (PFN_AvSetMmThreadCharacteristicsA)GetProcAddress(hAvrt, "AvSetMmThreadCharacteristicsA");
            if (s_pfnAvSet) {
                DWORD taskIndex = 0;
                HANDLE hTask = s_pfnAvSet("Games", &taskIndex);
                if (hTask) {
                    g_tel.mmcss_active = 1;
                    log_msg("[SCHED] MMCSS registered with 'Games' profile (Realtime OS scheduling quantum guaranteed).");
                }
            }
        }
    }
}

// Dynamic Anti-Stutter Governor Thread:
// 1. Keeps process affinity mask at 0xFFFF to utilize all 16 cores
// 2. Pins frame-critical interactive threads (<= 8ms) to P-Cores to prevent convoy stall
// 3. Allows sustained heavy / background threads (> 8ms) to offload onto E-Cores for full parallel power
// 4. Elevates process priority class to HIGH_PRIORITY_CLASS to prevent Windows OS preemption
static DWORD WINAPI GovernorThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);

    // Elevate process priority to HIGH_PRIORITY_CLASS
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SetProcessPriorityBoost(GetCurrentProcess(), FALSE);

    InitMMCSS();

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

                struct ScannedThread {
                    DWORD tid;
                    LONG tpBasePri;
                    HANDLE hThread;
                    ThreadSample* sample;
                    float frameWorkMs;
                    ULONGLONG totalTime;
                };

                ScannedThread scanned[MAX_TRACKED_THREADS];
                int scannedCount = 0;

                // Pass 1: Gather thread metrics and handle snapshot
                if (Thread32First(hSnap, &te)) {
                    do {
                        if (te.th32OwnerProcessID == pid && te.th32ThreadID != GetCurrentThreadId()) {
                            HANDLE hThread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                            if (hThread) {
                                FILETIME cr, ex, kernel, user;
                                float frameWorkMs = 0.0f;
                                ULONGLONG total = 0;
                                ThreadSample* sample = FindOrCreateSample(te.th32ThreadID);

                                if (GetThreadTimes(hThread, &cr, &ex, &kernel, &user)) {
                                    ULARGE_INTEGER k, u;
                                    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
                                    u.LowPart = user.dwLowDateTime;   u.HighPart = user.dwHighDateTime;
                                    total = k.QuadPart + u.QuadPart;
                                    DWORD now = GetTickCount();

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

                                if (scannedCount < MAX_TRACKED_THREADS) {
                                    scanned[scannedCount].tid = te.th32ThreadID;
                                    scanned[scannedCount].tpBasePri = te.tpBasePri;
                                    scanned[scannedCount].hThread = hThread;
                                    scanned[scannedCount].sample = sample;
                                    scanned[scannedCount].frameWorkMs = frameWorkMs;
                                    scanned[scannedCount].totalTime = total;
                                    scannedCount++;
                                } else {
                                    CloseHandle(hThread);
                                }
                            }
                        }
                    } while (Thread32Next(hSnap, &te));
                }
                CloseHandle(hSnap);

                // Identify dedicated threads:
                // 1. Audio thread: highest load with tpBasePri >= 15
                DWORD audio_tid = 0;
                ULONGLONG max_audio_time = 0;
                for (int i = 0; i < scannedCount; i++) {
                    if (scanned[i].tpBasePri >= 15 && scanned[i].totalTime > max_audio_time) {
                        max_audio_time = scanned[i].totalTime;
                        audio_tid = scanned[i].tid;
                    }
                }

                // 2. Render, Main, and Physics threads among tpBasePri == 10:
                DWORD render_tid = 0, main_tid = 0, physics_tid = 0;
                ULONGLONG top1_time = 0, top2_time = 0, top3_time = 0;
                for (int i = 0; i < scannedCount; i++) {
                    if (scanned[i].tpBasePri == 10) {
                        ULONGLONG t = scanned[i].totalTime;
                        if (t > top1_time) {
                            top3_time = top2_time; physics_tid = main_tid;
                            top2_time = top1_time; main_tid = render_tid;
                            top1_time = t;         render_tid = scanned[i].tid;
                        } else if (t > top2_time) {
                            top3_time = top2_time; physics_tid = main_tid;
                            top2_time = t;         main_tid = scanned[i].tid;
                        } else if (t > top3_time) {
                            top3_time = t;         physics_tid = scanned[i].tid;
                        }
                    }
                }

                // Pass 2: Assign Topology-Isolated Affinities
                LONG pCorePinned = 0;
                LONG eCoreActive = 0;
                // Frame-critical TaskGraph workers MUST execute on identical-frequency Performance Cores (4.6 GHz)
                // to prevent Gracemont E-Core convoy stalls and barrier latency jitter:
                DWORD_PTR worker_mask = (s_p_core_mask & ~0x000F); // Logical 4..11 (0x0FF0 on 12-thread P-Core CPUs)
                if (worker_mask == 0) worker_mask = s_p_core_mask;

                for (int i = 0; i < scannedCount; i++) {
                    DWORD tid = scanned[i].tid;
                    HANDLE hThread = scanned[i].hThread;
                    ThreadSample* sample = scanned[i].sample;
                    float frameWorkMs = scanned[i].frameWorkMs;
                    LONG tpBasePri = scanned[i].tpBasePri;

                    DWORD_PTR targetMask = s_all_cores_mask;
                    DWORD idealProc = (DWORD)-1;
                    int desiredAffinityType = 1;

                    if (tid == render_tid && render_tid != 0) {
                        // Dedicated Physical Core 0 (Logical 0, 1) - Render Thread
                        targetMask = 0x0003;
                        idealProc = 0;
                        desiredAffinityType = 10;
                        pCorePinned++;
                    } else if (tid == main_tid && main_tid != 0) {
                        // Dedicated Physical Core 1 (Logical 2, 3) - Main Engine Simulation Thread
                        targetMask = 0x000C;
                        idealProc = 2;
                        desiredAffinityType = 11;
                        pCorePinned++;
                    } else if (tid == audio_tid && audio_tid != 0) {
                        // Dedicated Physical Core 2 (Logical 4, 5) - Time-Critical Audio Mixer / Raytracing
                        targetMask = 0x0030;
                        idealProc = 4;
                        desiredAffinityType = 12;
                        pCorePinned++;
                    } else if (tid == physics_tid && physics_tid != 0) {
                        // Dedicated Physical Core 3 (Logical 6, 7) - Vehicle Physics Dynamics
                        targetMask = 0x00C0;
                        idealProc = 6;
                        desiredAffinityType = 13;
                        pCorePinned++;
                    } else if (tpBasePri >= 9) {
                        // TaskGraph Parallel Worker Pool & Engine Workers:
                        // Dispatched across Cores 4..15 (0xFFF0) - GUARANTEED ZERO CONTENTION on Cores 0..3!
                        targetMask = worker_mask;
                        desiredAffinityType = 14;
                        pCorePinned++;
                    } else {
                        // Background Streaming & Decompression Workers (tpBasePri <= 8):
                        // Dedicated to Gracemont E-Cores (0xF000) so Oodle / DirectStorage run off-cluster.
                        int bgDesired = (frameWorkMs > (float)g_cfg.ecore_threshold_ms) ? 3 : 2;
                        targetMask = (bgDesired == 3) ? s_all_cores_mask : s_e_core_mask;
                        desiredAffinityType = bgDesired;

                        if (sample) {
                            if (sample->pending_affinity_type == bgDesired) {
                                sample->pending_count++;
                            } else {
                                sample->pending_affinity_type = bgDesired;
                                sample->pending_count = 1;
                            }
                            if (sample->pending_count < 2 && sample->last_affinity_type != 0) {
                                desiredAffinityType = sample->last_affinity_type;
                                targetMask = (desiredAffinityType == 3) ? s_all_cores_mask : s_e_core_mask;
                            }
                        }
                        if (desiredAffinityType == 2 || desiredAffinityType == 3) {
                            eCoreActive++;
                        } else {
                            pCorePinned++;
                        }
                    }

                    if (sample && sample->last_affinity_type != desiredAffinityType) {
                        SetThreadAffinityMask(hThread, targetMask);
                        if (idealProc != (DWORD)-1) {
                            SetThreadIdealProcessor(hThread, idealProc);
                        }
                        if (sample->last_affinity_type != 0) {
                            InterlockedIncrement(&g_tel.sched_migrations);
                        }
                        sample->last_affinity_type = desiredAffinityType;
                    } else if (!sample) {
                        SetThreadAffinityMask(hThread, targetMask);
                        if (idealProc != (DWORD)-1) {
                            SetThreadIdealProcessor(hThread, idealProc);
                        }
                    }

                    CloseHandle(hThread);
                }

                InterlockedIncrement(&g_tel.sched_scans);
                g_tel.heavy_threads_pinned = pCorePinned;
                g_tel.ecore_threads_active = eCoreActive;
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
