// D3D12 Feature Acceleration, Compute Offload & Particle Swizzle Hook
#include <d3d12.h>
#include <stdio.h>
#include "proxy_common.h"

typedef HRESULT (STDMETHODCALLTYPE *PFN_CheckFeatureSupport)(
    ID3D12Device* This,
    D3D12_FEATURE Feature,
    void* pFeatureSupportData,
    UINT FeatureSupportDataSize
);

typedef void (STDMETHODCALLTYPE *PFN_CreateShaderResourceView)(
    ID3D12Device* This,
    ID3D12Resource* pResource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor
);

static PFN_CheckFeatureSupport s_orig_cfs = NULL;
static PFN_CreateShaderResourceView s_orig_csrv = NULL;
static uint64_t s_features_logged_mask = 0;

static HRESULT STDMETHODCALLTYPE Hook_CheckFeatureSupport(
    ID3D12Device* This,
    D3D12_FEATURE Feature,
    void* pFeatureSupportData,
    UINT FeatureSupportDataSize
) {
    HRESULT hr = s_orig_cfs(This, Feature, pFeatureSupportData, FeatureSupportDataSize);

    if (SUCCEEDED(hr) && pFeatureSupportData != NULL && g_cfg.compute_offload) {
        InterlockedIncrement(&g_tel.d3d12_feature_queries);

        if (Feature < 64) {
            if (!(s_features_logged_mask & (1ULL << Feature))) {
                s_features_logged_mask |= (1ULL << Feature);
                log_msg("[DX12] CheckFeatureSupport query: Feature=%d (0x%X)", Feature, Feature);
            }
        }

        switch (Feature) {
        case D3D12_FEATURE_D3D12_OPTIONS: {
            // Feature 0: Bindless Rendering, Single-Pass Cubemaps, and Hardware ROVs
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
        case D3D12_FEATURE_FORMAT_SUPPORT: {
            // Feature 3: Format support flags for compressed particle atlases & HDR buffers
            D3D12_FEATURE_DATA_FORMAT_SUPPORT* fs = (D3D12_FEATURE_DATA_FORMAT_SUPPORT*)pFeatureSupportData;
            if (fs->Format == DXGI_FORMAT_BC4_UNORM || fs->Format == DXGI_FORMAT_BC4_SNORM ||
                fs->Format == DXGI_FORMAT_BC4_TYPELESS ||
                fs->Format == DXGI_FORMAT_BC5_UNORM || fs->Format == DXGI_FORMAT_BC5_SNORM ||
                fs->Format == DXGI_FORMAT_BC5_TYPELESS ||
                fs->Format == DXGI_FORMAT_BC6H_UF16 || fs->Format == DXGI_FORMAT_BC6H_SF16 ||
                fs->Format == DXGI_FORMAT_BC7_UNORM || fs->Format == DXGI_FORMAT_BC7_UNORM_SRGB ||
                fs->Format == DXGI_FORMAT_BC7_TYPELESS ||
                fs->Format == DXGI_FORMAT_R8_UNORM || fs->Format == DXGI_FORMAT_R8_SNORM ||
                fs->Format == DXGI_FORMAT_R8G8_UNORM || fs->Format == DXGI_FORMAT_R8G8_SNORM ||
                fs->Format == DXGI_FORMAT_R8G8B8A8_UNORM || fs->Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                fs->Format == DXGI_FORMAT_R16_UNORM || fs->Format == DXGI_FORMAT_R16_SNORM ||
                fs->Format == DXGI_FORMAT_R16_FLOAT ||
                fs->Format == DXGI_FORMAT_R16G16B16A16_FLOAT || fs->Format == DXGI_FORMAT_R11G11B10_FLOAT)
            {
                fs->Support1 |= (D3D12_FORMAT_SUPPORT1_TEXTURE2D |
                                 D3D12_FORMAT_SUPPORT1_TEXTURE3D |
                                 D3D12_FORMAT_SUPPORT1_TEXTURECUBE |
                                 D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE |
                                 D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE_COMPARISON |
                                 D3D12_FORMAT_SUPPORT1_MIP |
                                 D3D12_FORMAT_SUPPORT1_SHADER_LOAD);
                fs->Support2 |= (D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE);
            }
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
            // Feature 18: GPU Depth Bounds Testing (Offloads light depth culling from CPU to GPU ROPs)
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

// Particle Texture Swizzle & Fallback Fix Hook:
// Intercepts SRV creation. In ForzaTech, single-channel particle smoke textures (BC4/R8)
// and 2-channel flow vectors (BC5) created with default swizzle (0, 1, 2, 3) lack G/B channels,
// causing shaders sampling .rgb to produce neon RED (R,0,0) or neon YELLOW (R,G,0).
// By replicating R to (R,R,R,R) for 1-channel textures and neutralizing Blue for 2-channel textures,
// particles sample as pure neutral volumetric luminance regardless of engine texture streaming delays!
static void STDMETHODCALLTYPE Hook_CreateShaderResourceView(
    ID3D12Device* This,
    ID3D12Resource* pResource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor
) {
    if (!pDesc) {
        if (pResource) {
            D3D12_RESOURCE_DESC rdesc = pResource->GetDesc();
            DXGI_FORMAT fmt = rdesc.Format;

            if (fmt == DXGI_FORMAT_BC4_UNORM || fmt == DXGI_FORMAT_BC4_SNORM || fmt == DXGI_FORMAT_BC4_TYPELESS ||
                fmt == DXGI_FORMAT_R8_UNORM || fmt == DXGI_FORMAT_R8_SNORM || fmt == DXGI_FORMAT_R8_TYPELESS ||
                fmt == DXGI_FORMAT_R16_UNORM || fmt == DXGI_FORMAT_R16_SNORM || fmt == DXGI_FORMAT_R16_FLOAT || fmt == DXGI_FORMAT_R16_TYPELESS)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
                desc.Format = (fmt == DXGI_FORMAT_BC4_TYPELESS) ? DXGI_FORMAT_BC4_UNORM :
                              (fmt == DXGI_FORMAT_R8_TYPELESS) ? DXGI_FORMAT_R8_UNORM :
                              (fmt == DXGI_FORMAT_R16_TYPELESS) ? DXGI_FORMAT_R16_FLOAT : fmt;
                desc.ViewDimension = (rdesc.DepthOrArraySize > 1) ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
                // Replicate R -> (R, R, R, R)
                desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);
                if (desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
                    desc.Texture2D.MostDetailedMip = 0;
                    desc.Texture2D.MipLevels = rdesc.MipLevels;
                    desc.Texture2D.PlaneSlice = 0;
                    desc.Texture2D.ResourceMinLODClamp = 0.0f;
                } else {
                    desc.Texture2DArray.MostDetailedMip = 0;
                    desc.Texture2DArray.MipLevels = rdesc.MipLevels;
                    desc.Texture2DArray.FirstArraySlice = 0;
                    desc.Texture2DArray.ArraySize = rdesc.DepthOrArraySize;
                    desc.Texture2DArray.PlaneSlice = 0;
                    desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
                }
                InterlockedIncrement(&g_tel.d3d12_particle_fixes);
                s_orig_csrv(This, pResource, &desc, DestDescriptor);
                return;
            }
        }
        s_orig_csrv(This, pResource, pDesc, DestDescriptor);
        return;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC desc_copy = *pDesc;
    bool modified = false;

    // 1. Single-channel density & smoke textures (BC4, R8, R16)
    // Default mapping (0, 1, 2, 3) -> Green=0, Blue=0 -> pure NEON RED smoke.
    // Fix: Replicate Red across all 4 components -> Grayscale volumetric smoke.
    if (desc_copy.Format == DXGI_FORMAT_BC4_UNORM || 
        desc_copy.Format == DXGI_FORMAT_BC4_SNORM || 
        desc_copy.Format == DXGI_FORMAT_BC4_TYPELESS ||
        desc_copy.Format == DXGI_FORMAT_R8_UNORM ||
        desc_copy.Format == DXGI_FORMAT_R8_SNORM ||
        desc_copy.Format == DXGI_FORMAT_R8_TYPELESS ||
        desc_copy.Format == DXGI_FORMAT_R16_UNORM ||
        desc_copy.Format == DXGI_FORMAT_R16_SNORM ||
        desc_copy.Format == DXGI_FORMAT_R16_FLOAT ||
        desc_copy.Format == DXGI_FORMAT_R16_TYPELESS)
    {
        if (desc_copy.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) {
            desc_copy.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);
            modified = true;
        }
    }
    // 2. Alpha-only mask textures (A8_UNORM)
    else if (desc_copy.Format == DXGI_FORMAT_A8_UNORM)
    {
        if (desc_copy.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) {
            desc_copy.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(3, 3, 3, 3);
            modified = true;
        }
    }
    // 3. Two-channel flow & velocity particle textures (BC5, R8G8)
    // Default mapping (0, 1, 2, 3) -> Blue=0 -> pure NEON YELLOW (Red + Green).
    // Fix: Map Blue to Component 0 (Red) while keeping .xy intact for normal maps.
    else if (desc_copy.Format == DXGI_FORMAT_BC5_UNORM || 
             desc_copy.Format == DXGI_FORMAT_BC5_SNORM ||
             desc_copy.Format == DXGI_FORMAT_BC5_TYPELESS ||
             desc_copy.Format == DXGI_FORMAT_R8G8_UNORM ||
             desc_copy.Format == DXGI_FORMAT_R8G8_SNORM)
    {
        if (desc_copy.Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING) {
            desc_copy.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 0, 1);
            modified = true;
        }
    }

    if (modified) {
        InterlockedIncrement(&g_tel.d3d12_particle_fixes);
    }
    s_orig_csrv(This, pResource, modified ? &desc_copy : pDesc, DestDescriptor);
}

void dx12_hook_install(ID3D12Device* pDevice) {
    if (!pDevice) return;

    void** vtbl = *(void***)pDevice;
    if (!vtbl) return;

    // CheckFeatureSupport is method index 13 in ID3D12DeviceVtbl (offset 104 bytes = 13 * 8)
    if (vtbl[13] != (void*)Hook_CheckFeatureSupport) {
        DWORD oldProtect;
        if (VirtualProtect(&vtbl[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            s_orig_cfs = (PFN_CheckFeatureSupport)vtbl[13];
            vtbl[13] = (void*)Hook_CheckFeatureSupport;
            VirtualProtect(&vtbl[13], sizeof(void*), oldProtect, &oldProtect);
            log_msg("[DX12] Successfully hooked ID3D12Device::CheckFeatureSupport (vtbl[13]).");
        } else {
            log_msg("[DX12] [WARN] VirtualProtect failed on ID3D12Device::CheckFeatureSupport.");
        }
    }

    // CreateShaderResourceView is method index 18 in ID3D12DeviceVtbl (offset 144 bytes = 18 * 8)
    if (vtbl[18] != (void*)Hook_CreateShaderResourceView) {
        DWORD oldProtect;
        if (VirtualProtect(&vtbl[18], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            s_orig_csrv = (PFN_CreateShaderResourceView)vtbl[18];
            vtbl[18] = (void*)Hook_CreateShaderResourceView;
            VirtualProtect(&vtbl[18], sizeof(void*), oldProtect, &oldProtect);
            log_msg("[DX12] Successfully hooked ID3D12Device::CreateShaderResourceView (vtbl[18]) for Particle Swizzle Fix.");
        } else {
            log_msg("[DX12] [WARN] VirtualProtect failed on ID3D12Device::CreateShaderResourceView.");
        }
    }
}
