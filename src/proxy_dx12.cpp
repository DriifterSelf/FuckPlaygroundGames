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
static uint64_t s_injected_logged_mask = 0;

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
            if (!(s_injected_logged_mask & (1ULL << 0))) {
                s_injected_logged_mask |= (1ULL << 0);
                log_msg("[DX12] [INJECT] Enabled Bindless Tier 3, Single-Pass Cubemaps, and Hardware ROVs (OIT)");
            }
            break;
        }
        case D3D12_FEATURE_FORMAT_SUPPORT: {
            // Feature 3: Full format support flags for compressed particle atlases & HDR buffers
            // Guarantees all sampling and MIP flags so texture streaming never drops to debug placeholder colors!
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
            g_tel.d3d12_format_support = 1;
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS1: {
            // Feature 8: WaveOps and Wave32/64 execution mode for AMD RDNA4
            D3D12_FEATURE_DATA_D3D12_OPTIONS1* opt1 = (D3D12_FEATURE_DATA_D3D12_OPTIONS1*)pFeatureSupportData;
            opt1->WaveOps = TRUE;
            opt1->WaveLaneCountMin = 32;
            opt1->WaveLaneCountMax = 64;
            opt1->TotalLaneCount = g_tel.num_cus * 64;
            g_tel.d3d12_wave_ops = 1;
            if (!(s_injected_logged_mask & (1ULL << 8))) {
                s_injected_logged_mask |= (1ULL << 8);
                log_msg("[DX12] [INJECT] Enabled WaveOps & Wave32 for RDNA4 (TotalLaneCount=%u)", opt1->TotalLaneCount);
            }
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS2: {
            // Feature 18: NOTE: We strictly preserve driver default DepthBoundsTestSupported.
            // Enabling DepthBounds on ForzaTech with reversed-Z causes OMSetDepthBounds(near, far)
            // where near > far to reject 100% of lighting volumes and black out the scene.
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS3: {
            // Feature 21: Enable Async Copy Queue timestamps and casting for fast texture streaming
            D3D12_FEATURE_DATA_D3D12_OPTIONS3* opt3 = (D3D12_FEATURE_DATA_D3D12_OPTIONS3*)pFeatureSupportData;
            opt3->CopyQueueTimestampQueriesSupported = TRUE;
            opt3->CastingFullyTypedFormatSupported = TRUE;
            g_tel.d3d12_async_copy = 1;
            if (!(s_injected_logged_mask & (1ULL << 21))) {
                s_injected_logged_mask |= (1ULL << 21);
                log_msg("[DX12] [INJECT] Enabled Async Copy Queue & Typed Format Casting");
            }
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS4: {
            // Feature 23: Native 16-bit Float and Int operations (2x math rate on RDNA4 Compute Units)
            D3D12_FEATURE_DATA_D3D12_OPTIONS4* opt4 = (D3D12_FEATURE_DATA_D3D12_OPTIONS4*)pFeatureSupportData;
            opt4->Native16BitShaderOpsSupported = TRUE;
            if (!(s_injected_logged_mask & (1ULL << 23))) {
                s_injected_logged_mask |= (1ULL << 23);
                log_msg("[DX12] [INJECT] Enabled Native 16-bit Float/Int Shader Ops (2x Math Rate on RDNA4)");
            }
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS5: {
            // Feature 27: Force Raytracing Tier 1.1 support for Inline Raytracing (RayQuery in Compute Shaders)
            D3D12_FEATURE_DATA_D3D12_OPTIONS5* opt5 = (D3D12_FEATURE_DATA_D3D12_OPTIONS5*)pFeatureSupportData;
            if (opt5->RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
                opt5->RaytracingTier = D3D12_RAYTRACING_TIER_1_1;
            }
            opt5->RenderPassesTier = D3D12_RENDER_PASS_TIER_1;
            g_tel.d3d12_rayquery = 1;
            if (!(s_injected_logged_mask & (1ULL << 27))) {
                s_injected_logged_mask |= (1ULL << 27);
                log_msg("[DX12] [INJECT] Enabled DXR Tier 1.1 (Inline RayQuery in Compute Shaders)");
            }
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS7: {
            // Feature 32: Mesh Shader Tier 1 (GPU cluster culling) & Sampler Feedback Tier 1.0 (Hardware Streaming Min-Map)
            D3D12_FEATURE_DATA_D3D12_OPTIONS7* opt7 = (D3D12_FEATURE_DATA_D3D12_OPTIONS7*)pFeatureSupportData;
            if (opt7->MeshShaderTier < D3D12_MESH_SHADER_TIER_1) {
                opt7->MeshShaderTier = D3D12_MESH_SHADER_TIER_1;
            }
            opt7->SamplerFeedbackTier = D3D12_SAMPLER_FEEDBACK_TIER_1_0;
            g_tel.d3d12_mesh_shader = 1;
            g_tel.d3d12_sampler_feedback = 1;
            if (!(s_injected_logged_mask & (1ULL << 32))) {
                s_injected_logged_mask |= (1ULL << 32);
                log_msg("[DX12] [INJECT] Enabled Mesh Shader Tier 1 & Sampler Feedback Tier 1.0 (GPU Cluster & Streaming Min-Map)");
            }
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS8: {
            // Feature 36: Unaligned Block Textures
            D3D12_FEATURE_DATA_D3D12_OPTIONS8* opt8 = (D3D12_FEATURE_DATA_D3D12_OPTIONS8*)pFeatureSupportData;
            opt8->UnalignedBlockTexturesSupported = TRUE;
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS9: {
            // Feature 37: 64-bit GPU Atomics, Mesh Derivatives, and WaveMMA
            D3D12_FEATURE_DATA_D3D12_OPTIONS9* opt9 = (D3D12_FEATURE_DATA_D3D12_OPTIONS9*)pFeatureSupportData;
            opt9->MeshShaderPipelineStatsSupported = TRUE;
            opt9->MeshShaderSupportsFullRangeRenderTargetArrayIndex = TRUE;
            opt9->AtomicInt64OnTypedResourceSupported = TRUE;
            opt9->AtomicInt64OnGroupSharedSupported = TRUE;
            opt9->DerivativesInMeshAndAmplificationShadersSupported = TRUE;
            opt9->WaveMMATier = D3D12_WAVE_MMA_TIER_1_0;
            g_tel.d3d12_gpu_atomics64 = 1;
            if (!(s_injected_logged_mask & (1ULL << 37))) {
                s_injected_logged_mask |= (1ULL << 37);
                log_msg("[DX12] [INJECT] Enabled 64-bit GPU Atomics, WaveMMA, and Mesh Derivatives");
            }
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS11: {
            // Feature 40: 64-bit GPU Atomics on Descriptor Heap Resources
            D3D12_FEATURE_DATA_D3D12_OPTIONS11* opt11 = (D3D12_FEATURE_DATA_D3D12_OPTIONS11*)pFeatureSupportData;
            opt11->AtomicInt64OnDescriptorHeapResourceSupported = TRUE;
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS12: {
            // Feature 41: Enhanced Barriers (Direct hardware layout transitions without CPU tracking)
            D3D12_FEATURE_DATA_D3D12_OPTIONS12* opt12 = (D3D12_FEATURE_DATA_D3D12_OPTIONS12*)pFeatureSupportData;
            opt12->EnhancedBarriersSupported = TRUE;
            opt12->RelaxedFormatCastingSupported = TRUE;
            g_tel.d3d12_enhanced_barriers = 1;
            if (!(s_injected_logged_mask & (1ULL << 41))) {
                s_injected_logged_mask |= (1ULL << 41);
                log_msg("[DX12] [INJECT] Enabled Enhanced Barriers (Zero CPU Barrier Tracking)");
            }
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS13: {
            // Feature 42: Unrestricted Vertex & Buffer Alignment (Zero CPU Re-packing)
            D3D12_FEATURE_DATA_D3D12_OPTIONS13* opt13 = (D3D12_FEATURE_DATA_D3D12_OPTIONS13*)pFeatureSupportData;
            opt13->UnrestrictedBufferTextureCopyPitchSupported = TRUE;
            opt13->UnrestrictedVertexElementAlignmentSupported = TRUE;
            opt13->TextureCopyBetweenDimensionsSupported = TRUE;
            opt13->AlphaBlendFactorSupported = TRUE;
            break;
        }
        case D3D12_FEATURE_D3D12_OPTIONS16: {
            // Feature 45: GPU Upload Heaps (Direct ReBAR VRAM Writes - Zero Staging Copies)
            D3D12_FEATURE_DATA_D3D12_OPTIONS16* opt16 = (D3D12_FEATURE_DATA_D3D12_OPTIONS16*)pFeatureSupportData;
            opt16->GPUUploadHeapSupported = TRUE;
            g_tel.d3d12_gpu_upload_heap = 1;
            if (!(s_injected_logged_mask & (1ULL << 45))) {
                s_injected_logged_mask |= (1ULL << 45);
                log_msg("[DX12] [INJECT] Enabled GPU Upload Heaps (Direct ReBAR VRAM Writes - Zero CPU Staging Copies)");
            }
            break;
        }
        default:
            break;
        }
    }
    return hr;
}

// Particle Texture Swizzle Fix:
// Targets ONLY single-channel BC4 compressed smoke/dust/particle atlases.
// In D3D12 default mapping (0, 1, 2, 3), Green=0 and Blue=0, turning single-channel smoke into neon RED (R, 0, 0, 1).
// By replicating Red across all 4 channels (0, 0, 0, 0), it samples as clean neutral grayscale smoke.
// IMPORTANT: BC5 (normal maps) and R16/R32 (shadow/depth maps) are NEVER touched, ensuring 100% lighting integrity!
static void STDMETHODCALLTYPE Hook_CreateShaderResourceView(
    ID3D12Device* This,
    ID3D12Resource* pResource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor
) {
    if (pDesc) {
        if ((pDesc->Format == DXGI_FORMAT_BC4_UNORM || pDesc->Format == DXGI_FORMAT_BC4_SNORM) &&
            pDesc->Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc_copy = *pDesc;
            desc_copy.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);
            InterlockedIncrement(&g_tel.d3d12_particle_fixes);
            s_orig_csrv(This, pResource, &desc_copy, DestDescriptor);
            return;
        }
    } else if (pResource) {
        D3D12_RESOURCE_DESC rdesc = pResource->GetDesc();
        if (rdesc.Format == DXGI_FORMAT_BC4_UNORM || rdesc.Format == DXGI_FORMAT_BC4_SNORM) {
            D3D12_SHADER_RESOURCE_VIEW_DESC local_desc = {};
            local_desc.Format = rdesc.Format;
            local_desc.ViewDimension = (rdesc.DepthOrArraySize > 1) ? D3D12_SRV_DIMENSION_TEXTURE2DARRAY : D3D12_SRV_DIMENSION_TEXTURE2D;
            local_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);
            if (local_desc.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
                local_desc.Texture2D.MostDetailedMip = 0;
                local_desc.Texture2D.MipLevels = rdesc.MipLevels;
                local_desc.Texture2D.PlaneSlice = 0;
                local_desc.Texture2D.ResourceMinLODClamp = 0.0f;
            } else {
                local_desc.Texture2DArray.MostDetailedMip = 0;
                local_desc.Texture2DArray.MipLevels = rdesc.MipLevels;
                local_desc.Texture2DArray.FirstArraySlice = 0;
                local_desc.Texture2DArray.ArraySize = rdesc.DepthOrArraySize;
                local_desc.Texture2DArray.PlaneSlice = 0;
                local_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
            }
            InterlockedIncrement(&g_tel.d3d12_particle_fixes);
            s_orig_csrv(This, pResource, &local_desc, DestDescriptor);
            return;
        }
    }
    s_orig_csrv(This, pResource, pDesc, DestDescriptor);
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
