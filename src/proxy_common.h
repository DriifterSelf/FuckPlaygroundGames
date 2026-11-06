// Shared state for the AGS proxy (Fixes-Only Clean Build)
#pragma once
#include <windows.h>
#include <stdint.h>

#define PROXY_VERSION "3.13.2"

enum component_status
{
    st_pending = 0,
    st_ok = 1,
    st_fail = 2,
    st_disabled = 3,
    st_warn = 4
};

struct proxy_config
{
    int vram_uncap;
    int vram_margin_percent;
    int oom_guard;
    int oom_guard_max;
    int hybrid_governor;
    int particle_fix;
};

struct proxy_telemetry
{
    char exe_name[MAX_PATH];
    DWORD exe_timestamp;
    uintptr_t exe_base;
    uintptr_t exe_end;

    int st_ags_load;
    int st_ags_init;
    int ags_init_result;
    char driver_version[64];
    char gpu_name[128];
    int vendor_id;
    int device_id;
    int asic_family;
    int num_cus;
    int num_wgps;
    int num_rops;
    UINT64 ags_local_mem;

    int st_dx12;
    int dx12_result;
    uint32_t ext_bits;

    int st_vram;
    char vram_err[96];
    volatile LONG vram_calls;
    volatile LONG vram_uncap_hits;
    volatile LONG vram_passed;
    UINT64 dedicated_bytes;
    UINT64 current_usage_bytes;
    UINT64 os_budget_bytes;
    UINT64 reported_budget_bytes;

    int st_guard;
    volatile LONG guard_hits;
    volatile LONG guard_suppressed;
    volatile LONG guard_passed;

    // Scheduler & Thread Governor Telemetry
    volatile LONG heavy_threads_pinned;
    volatile LONG ecore_threads_active;
    volatile LONG sched_scans;
    volatile LONG sched_migrations;
    DWORD_PTR p_core_mask;
    DWORD_PTR e_core_mask;
    DWORD_PTR all_cores_mask;
    int mmcss_active;
};

extern proxy_config g_cfg;
extern proxy_telemetry g_tel;

void log_msg(const char* fmt, ...);
void open_log_and_config();
void identify_game();

void vram_hook_install();
void guard_install();
void sched_governor_start();
struct ID3D12Device;
void particles_hook_install(struct ID3D12Device* pDevice);
