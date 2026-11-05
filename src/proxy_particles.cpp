// Direct3D 12 Particle SRV Swizzle Hook
// Eliminates neon red, yellow, and magenta color corruption on single-channel BC4/R8/R16 textures
// Restores soft depth particle fading by providing unified monochromatic luminance & alpha
#include <d3d12.h>
#include <stdio.h>
#include "proxy_common.h"

typedef void (STDMETHODCALLTYPE *PFN_CreateShaderResourceView)(
    ID3D12Device* This,
    ID3D12Resource* pResource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor
);

static PFN_CreateShaderResourceView s_orig_csrv = NULL;

static inline bool IsSingleChannelFormat(DXGI_FORMAT fmt) {
    return (fmt == DXGI_FORMAT_BC4_UNORM ||
            fmt == DXGI_FORMAT_BC4_SNORM ||
            fmt == DXGI_FORMAT_BC4_TYPELESS ||
            fmt == DXGI_FORMAT_R8_UNORM ||
            fmt == DXGI_FORMAT_R8_SNORM ||
            fmt == DXGI_FORMAT_R8_TYPELESS ||
            fmt == DXGI_FORMAT_R16_UNORM ||
            fmt == DXGI_FORMAT_R16_SNORM ||
            fmt == DXGI_FORMAT_R16_FLOAT ||
            fmt == DXGI_FORMAT_R16_TYPELESS);
}

static void STDMETHODCALLTYPE Hook_CreateShaderResourceView(
    ID3D12Device* This,
    ID3D12Resource* pResource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor
) {
    if (pDesc) {
        if (IsSingleChannelFormat(pDesc->Format) &&
            pDesc->Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC desc_copy = *pDesc;
            // Map (0, 0, 0, 0): R -> R, G -> R, B -> R, A -> R
            // Guarantees R == G == B, completely neutralizing magenta (R=1, G=0, B=1) and neon red (R=1, G=0, B=0)
            // Sets Alpha = R, eliminating hard cube boundaries and restoring soft alpha fade
            desc_copy.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);
            s_orig_csrv(This, pResource, &desc_copy, DestDescriptor);
            return;
        }
    } else if (pResource) {
        D3D12_RESOURCE_DESC rdesc = pResource->GetDesc();
        if (IsSingleChannelFormat(rdesc.Format)) {
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
            s_orig_csrv(This, pResource, &local_desc, DestDescriptor);
            return;
        }
    }

    // STRICT INTEGRITY: All multi-channel, color (BC7, RGBA), normal (BC5), and depth formats pass 100% native!
    s_orig_csrv(This, pResource, pDesc, DestDescriptor);
}

void particles_hook_install(ID3D12Device* pDevice) {
    if (!pDevice) return;

    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;

    void** vtbl = *(void***)pDevice;
    if (!vtbl) return;

    if (vtbl[18] != (void*)Hook_CreateShaderResourceView) {
        DWORD oldProtect;
        if (VirtualProtect(&vtbl[18], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            s_orig_csrv = (PFN_CreateShaderResourceView)vtbl[18];
            vtbl[18] = (void*)Hook_CreateShaderResourceView;
            VirtualProtect(&vtbl[18], sizeof(void*), oldProtect, &oldProtect);
            log_msg("[PARTICLES] Hooked ID3D12Device::CreateShaderResourceView (vtbl[18]) for Particle Swizzle Fix.");
        } else {
            log_msg("[PARTICLES] [WARN] VirtualProtect failed on ID3D12Device::CreateShaderResourceView.");
        }
    }
}
