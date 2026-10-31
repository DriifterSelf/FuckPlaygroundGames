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
        switch (Feature) {
        case D3D12_FEATURE_D3D12_OPTIONS1: {
            // WaveOps and Wave32/64 execution mode for AMD RDNA4
            D3D12_FEATURE_DATA_D3D12_OPTIONS1* opt1 = (D3D12_FEATURE_DATA_D3D12_OPTIONS1*)pFeatureSupportData;
            opt1->WaveOps = TRUE;
            opt1->WaveLaneCountMin = 32;
            opt1->WaveLaneCountMax = 64;
            opt1->TotalLaneCount = g_tel.num_cus * 64;
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS5: {
            // Force Raytracing Tier 1.1 support for Inline Raytracing (RayQuery in Compute Shaders)
            D3D12_FEATURE_DATA_D3D12_OPTIONS5* opt5 = (D3D12_FEATURE_DATA_D3D12_OPTIONS5*)pFeatureSupportData;
            if (opt5->RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
                opt5->RaytracingTier = D3D12_RAYTRACING_TIER_1_1;
            }
            opt5->RenderPassesTier = D3D12_RENDER_PASS_TIER_1;
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS7: {
            // Enable Mesh Shader Tier 1 for GPU cluster culling
            D3D12_FEATURE_DATA_D3D12_OPTIONS7* opt7 = (D3D12_FEATURE_DATA_D3D12_OPTIONS7*)pFeatureSupportData;
            if (opt7->MeshShaderTier < D3D12_MESH_SHADER_TIER_1) {
                opt7->MeshShaderTier = D3D12_MESH_SHADER_TIER_1;
            }
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
    if (vtbl && vtbl[40] != (void*)Hook_CheckFeatureSupport) {
        DWORD oldProtect;
        if (VirtualProtect(&vtbl[40], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            s_orig_cfs = (PFN_CheckFeatureSupport)vtbl[40];
            vtbl[40] = (void*)Hook_CheckFeatureSupport;
            VirtualProtect(&vtbl[40], sizeof(void*), oldProtect, &oldProtect);
            log_msg("[DX12] Successfully hooked ID3D12Device::CheckFeatureSupport (vtbl[40]) for compute offload & DXR 1.1.");
        } else {
            log_msg("[DX12] [WARN] VirtualProtect failed on ID3D12Device::CheckFeatureSupport.");
        }
    }
}
