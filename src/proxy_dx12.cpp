// D3D12 Feature Acceleration, VRS Disabling & Native Execution Hook
#include <d3d12.h>
#include <stdio.h>
#include "proxy_common.h"

typedef HRESULT (STDMETHODCALLTYPE *PFN_CheckFeatureSupport)(
    ID3D12Device* This,
    D3D12_FEATURE Feature,
    void* pFeatureSupportData,
    UINT FeatureSupportDataSize
);

static PFN_CheckFeatureSupport s_orig_cfs = NULL;
static uint64_t s_features_logged_mask = 0;
static uint64_t s_injected_logged_mask = 0;

// High-Performance Thread-Local Telemetry Batcher:
struct D3D12TelemetryBatcher {
    int cfs_queries;
    int cfs_injected;
    int cfs_passed;
};
static thread_local D3D12TelemetryBatcher s_tl_batch = {};

static inline void FlushD3D12Batch() {
    if (s_tl_batch.cfs_queries >= 64) {
        InterlockedAdd(&g_tel.d3d12_feature_queries, s_tl_batch.cfs_queries);
        s_tl_batch.cfs_queries = 0;
    }
    if (s_tl_batch.cfs_injected >= 64) {
        InterlockedAdd(&g_tel.d3d12_cfs_injected, s_tl_batch.cfs_injected);
        s_tl_batch.cfs_injected = 0;
    }
    if (s_tl_batch.cfs_passed >= 64) {
        InterlockedAdd(&g_tel.d3d12_cfs_passed, s_tl_batch.cfs_passed);
        s_tl_batch.cfs_passed = 0;
    }
}

static HRESULT STDMETHODCALLTYPE Hook_CheckFeatureSupport(
    ID3D12Device* This,
    D3D12_FEATURE Feature,
    void* pFeatureSupportData,
    UINT FeatureSupportDataSize
) {
    HRESULT hr = s_orig_cfs(This, Feature, pFeatureSupportData, FeatureSupportDataSize);

    if (SUCCEEDED(hr) && pFeatureSupportData != NULL) {
        s_tl_batch.cfs_queries++;

        if (Feature < 64) {
            if (!(s_features_logged_mask & (1ULL << Feature))) {
                s_features_logged_mask |= (1ULL << Feature);
                log_msg("[DX12] CheckFeatureSupport query: Feature=%d (0x%X)", Feature, Feature);
            }
        }

        bool injected = false;
        switch (Feature) {
        case D3D12_FEATURE_D3D12_OPTIONS6: {
            // Feature 30: Variable Rate Shading & Background Processing
            // Hardcoded in compiled binary: Unconditionally DISABLE Variable Rate Shading (VRS)
            // Eliminates 2x2 / 4x4 coarsened particle blocks and restores 100% full-rate 1x1 shading
            D3D12_FEATURE_DATA_D3D12_OPTIONS6* opt6 = (D3D12_FEATURE_DATA_D3D12_OPTIONS6*)pFeatureSupportData;
            opt6->VariableShadingRateTier = D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
            opt6->AdditionalShadingRatesSupported = FALSE;
            opt6->PerPrimitiveShadingRateSupportedWithViewportIndexing = FALSE;
            injected = true;
            if (!(s_injected_logged_mask & (1ULL << 30))) {
                s_injected_logged_mask |= (1ULL << 30);
                log_msg("[DX12] [COMPILED] Variable Rate Shading (VRS) permanently disabled in compiled binary.");
            }
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS10: {
            // Feature 39: Mesh Shader Shading Rate & Combiner
            // Hardcoded in compiled binary: Disable VRS Combiner and Primitive Shading Rate
            D3D12_FEATURE_DATA_D3D12_OPTIONS10* opt10 = (D3D12_FEATURE_DATA_D3D12_OPTIONS10*)pFeatureSupportData;
            opt10->VariableRateShadingSumCombinerSupported = FALSE;
            opt10->MeshShaderPerPrimitiveShadingRateSupported = FALSE;
            injected = true;
            break;
        }
        default:
            // ALL OTHER FEATURES: Strictly preserve 100% native AMD driver responses!
            // No synthetic sampler modes, no fake query timestamps, no fake GPU upload heaps,
            // no fake predication. Zero memory corruption, zero billboard UV clamping.
            injected = false;
            break;
        }

        if (injected) {
            s_tl_batch.cfs_injected++;
        } else {
            s_tl_batch.cfs_passed++;
        }
    } else {
        s_tl_batch.cfs_passed++;
    }
    FlushD3D12Batch();
    return hr;
}

static void HookVtblEntry(void** slot, void* hookFunc, void** origFunc, const char* name) {
    if (*slot != hookFunc) {
        DWORD oldProtect;
        if (VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *origFunc = *slot;
            *slot = hookFunc;
            VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
            log_msg("[DX12] Hooked %s (%p -> %p)", name, *origFunc, hookFunc);
        } else {
            log_msg("[DX12] [WARN] VirtualProtect failed on %s", name);
        }
    }
}

void dx12_hook_install(ID3D12Device* pDevice) {
    if (!pDevice) return;

    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;

    void** vtbl = *(void***)pDevice;
    if (!vtbl) return;

    // ONLY Hook CheckFeatureSupport (vtbl[13]) to permanently disable VRS.
    // ALL resource, heap, queue, and extended vtable slots remain 100% UNTOUCHED
    // to preserve native D3D12Core 1.619 internal allocator structures.
    HookVtblEntry(&vtbl[13], (void*)Hook_CheckFeatureSupport, (void**)&s_orig_cfs, "ID3D12Device::CheckFeatureSupport (vtbl[13])");
}
