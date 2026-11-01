// Shared state for the AGS proxy (self-aware build)
#pragma once
#include <windows.h>
#include <stdint.h>

#define PROXY_VERSION "3.11.0"

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
    int overlay;
    int overlay_key;
    int vram_uncap;
    int vram_margin_percent;
    int oom_guard;
    int oom_guard_max;
    int compute_offload;
    int hybrid_governor;
    int ecore_threshold_ms;
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
    int ags_mdi_unlocked;

    // Direct3D 12 Compute & Feature Accelerators
    volatile LONG d3d12_feature_queries;
    volatile LONG d3d12_cfs_injected;
    volatile LONG d3d12_cfs_passed;
    int d3d12_wave_ops;
    int d3d12_mesh_shader;
    int d3d12_rayquery;
    int d3d12_async_copy;
    int d3d12_bindless_tier3;
    int d3d12_single_pass_cubemaps;
    int d3d12_rovs_oit;
    int d3d12_depth_bounds;
    int d3d12_enhanced_barriers;
    int d3d12_gpu_atomics64;
    volatile LONG d3d12_particle_fixes;
    volatile LONG d3d12_srv_total;
    volatile LONG d3d12_srv_passed;
    int d3d12_execute_indirect_1_1;
    int d3d12_work_graphs;
    int d3d12_sampler_feedback;
    int d3d12_gpu_upload_heap;
    int d3d12_predication;
    int d3d12_format_support;
    int d3d12_audio_compute;
    volatile LONG d3d12_audio_queries;
    int d3d12_hiz_culling;
    volatile LONG d3d12_hiz_queries;
    int d3d12_compute_skinning;
    volatile LONG d3d12_skinning_queries;
    int d3d12_placed_resources;
    volatile LONG d3d12_gpu_upload_rescued;
    volatile LONG d3d12_committed_resources_created;
    volatile LONG d3d12_heaps_created;
    int d3d12_displayable;
    int d3d12_isolated_mmu;
    volatile LONG d3d12_compute_queues_elevated;
    volatile LONG d3d12_copy_queues_elevated;
    int mmcss_active;

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

    int st_overlay;
};

extern proxy_config g_cfg;
extern proxy_telemetry g_tel;

void log_msg(const char* fmt, ...);
void open_log_and_config();
void identify_game();

void vram_hook_install();
void guard_install();
void overlay_start();
struct ID3D12Device;
void dx12_hook_install(struct ID3D12Device* pDevice);
void sched_governor_start();
