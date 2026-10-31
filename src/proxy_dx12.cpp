// D3D12 Feature Acceleration & Compute Shader Offload Hook
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

static HRESULT STDMETHODCALLTYPE Hook_CheckFeatureSupport(
    ID3D12Device* This,
    D3D12_FEATURE Feature,
    void* pFeatureSupportData,
    UINT FeatureSupportDataSize
) {
    HRESULT hr = s_orig_cfs(This, Feature, pFeatureSupportData, FeatureSupportDataSize);

    if (SUCCEEDED(hr) && pFeatureSupportData != NULL && g_cfg.compute_offload) {
        InterlockedIncrement(&g_tel.d3d12_feature_queries);
        log_msg("[DX12] CheckFeatureSupport query: Feature=%d (0x%X)", Feature, Feature);

        switch (Feature) {
        case D3D12_FEATURE_D3D12_OPTIONS: {
            // Feature 3: Bindless Rendering, Single-Pass Cubemaps, and Hardware ROVs
            D3D12_FEATURE_DATA_D3D12_OPTIONS* opt = (D3D12_FEATURE_DATA_D3D12_OPTIONS*)pFeatureSupportData;
            opt->ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
            opt->VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = TRUE;
            opt->ROVsSupported = TRUE;
            opt->ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
            g_tel.d3d12_bindless_tier3 = 1;
            g_tel.d3d12_single_pass_cubemaps = 1;
            g_tel.d3d12_rovs_oit = 1;
            log_msg("[DX12] [INJECT] Enabled Bindless Tier 3, Single-Pass Cubemaps, and Hardware ROVs (OIT)");
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS1: {
            // WaveOps and Wave32/64 execution mode for AMD RDNA4
            D3D12_FEATURE_DATA_D3D12_OPTIONS1* opt1 = (D3D12_FEATURE_DATA_D3D12_OPTIONS1*)pFeatureSupportData;
            opt1->WaveOps = TRUE;
            opt1->WaveLaneCountMin = 32;
            opt1->WaveLaneCountMax = 64;
            opt1->TotalLaneCount = g_tel.num_cus * 64;
            g_tel.d3d12_wave_ops = 1;
            log_msg("[DX12] [INJECT] Enabled WaveOps & Wave32 for RDNA4 (TotalLaneCount=%u)", opt1->TotalLaneCount);
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS2: {
            // Feature 23: GPU Depth Bounds Testing (Offloads light depth culling from CPU to GPU ROPs)
            D3D12_FEATURE_DATA_D3D12_OPTIONS2* opt2 = (D3D12_FEATURE_DATA_D3D12_OPTIONS2*)pFeatureSupportData;
            opt2->DepthBoundsTestSupported = TRUE;
            g_tel.d3d12_depth_bounds = 1;
            log_msg("[DX12] [INJECT] Enabled GPU Hardware Depth Bounds Testing for Light Culling");
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS3: {
            // Enable Async Copy Queue timestamps and casting for fast texture streaming
            D3D12_FEATURE_DATA_D3D12_OPTIONS3* opt3 = (D3D12_FEATURE_DATA_D3D12_OPTIONS3*)pFeatureSupportData;
            opt3->CopyQueueTimestampQueriesSupported = TRUE;
            opt3->CastingFullyTypedFormatSupported = TRUE;
            g_tel.d3d12_async_copy = 1;
            log_msg("[DX12] [INJECT] Enabled Async Copy Queue & Typed Format Casting");
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS5: {
            // Force Raytracing Tier 1.1 support for Inline Raytracing (RayQuery in Compute Shaders)
            D3D12_FEATURE_DATA_D3D12_OPTIONS5* opt5 = (D3D12_FEATURE_DATA_D3D12_OPTIONS5*)pFeatureSupportData;
            if (opt5->RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
                opt5->RaytracingTier = D3D12_RAYTRACING_TIER_1_1;
            }
            opt5->RenderPassesTier = D3D12_RENDER_PASS_TIER_1;
            g_tel.d3d12_rayquery = 1;
            log_msg("[DX12] [INJECT] Enabled DXR Tier 1.1 (Inline RayQuery in Compute Shaders)");
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS7: {
            // Enable Mesh Shader Tier 1 for GPU cluster culling
            D3D12_FEATURE_DATA_D3D12_OPTIONS7* opt7 = (D3D12_FEATURE_DATA_D3D12_OPTIONS7*)pFeatureSupportData;
            if (opt7->MeshShaderTier < D3D12_MESH_SHADER_TIER_1) {
                opt7->MeshShaderTier = D3D12_MESH_SHADER_TIER_1;
            }
            g_tel.d3d12_mesh_shader = 1;
            log_msg("[DX12] [INJECT] Enabled Mesh Shader Tier 1 (GPU Cluster Culling)");
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS9: {
            // Feature 34: 64-bit GPU Atomics, Mesh Derivatives, and WaveMMA
            D3D12_FEATURE_DATA_D3D12_OPTIONS9* opt9 = (D3D12_FEATURE_DATA_D3D12_OPTIONS9*)pFeatureSupportData;
            opt9->MeshShaderPipelineStatsSupported = TRUE;
            opt9->MeshShaderSupportsFullRangeRenderTargetArrayIndex = TRUE;
            opt9->AtomicInt64OnTypedResourceSupported = TRUE;
            opt9->AtomicInt64OnGroupSharedSupported = TRUE;
            opt9->DerivativesInMeshAndAmplificationShadersSupported = TRUE;
            opt9->WaveMMATier = D3D12_WAVE_MMA_TIER_1_0;
            g_tel.d3d12_gpu_atomics64 = 1;
            log_msg("[DX12] [INJECT] Enabled 64-bit GPU Atomics, WaveMMA, and Mesh Derivatives");
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS12: {
            // Feature 40: Enhanced Barriers (Direct hardware layout transitions without CPU tracking)
            D3D12_FEATURE_DATA_D3D12_OPTIONS12* opt12 = (D3D12_FEATURE_DATA_D3D12_OPTIONS12*)pFeatureSupportData;
            opt12->EnhancedBarriersSupported = TRUE;
            opt12->RelaxedFormatCastingSupported = TRUE;
            g_tel.d3d12_enhanced_barriers = 1;
            log_msg("[DX12] [INJECT] Enabled Enhanced Barriers (Zero CPU Barrier Tracking)");
            break;
        }
        default:
            break;
        }
    }
    return hr;
}

void dx12_hook_install(ID3D12Device* pDevice) {
    if (!pDevice) return;

    void** vtbl = *(void***)pDevice;
    // CheckFeatureSupport is method index 13 in ID3D12DeviceVtbl (offset 104 bytes = 13 * 8)
    if (vtbl && vtbl[13] != (void*)Hook_CheckFeatureSupport) {
        DWORD oldProtect;
        if (VirtualProtect(&vtbl[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            s_orig_cfs = (PFN_CheckFeatureSupport)vtbl[13];
            vtbl[13] = (void*)Hook_CheckFeatureSupport;
            VirtualProtect(&vtbl[13], sizeof(void*), oldProtect, &oldProtect);
            log_msg("[DX12] Successfully hooked ID3D12Device::CheckFeatureSupport (vtbl[13]) for compute offload & DXR 1.1.");
        } else {
            log_msg("[DX12] [WARN] VirtualProtect failed on ID3D12Device::CheckFeatureSupport.");
        }
    }
}
