// Direct3D 12 Particle SRV Swizzle Hook
// Eliminates neon red, yellow, and magenta color corruption on single-channel BC4 textures
// Restores soft depth particle fading by providing neutral white luminance (1,1,1) & alpha (R)
// Eliminates "cubos magenta", "png de burbujas", and "manchas negras"
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
static LONG s_particle_fixes_applied = 0;

static inline bool IsBC4ParticleFormat(DXGI_FORMAT fmt) {
    return (fmt == DXGI_FORMAT_BC4_UNORM ||
            fmt == DXGI_FORMAT_BC4_SNORM);
}

static void STDMETHODCALLTYPE Hook_CreateShaderResourceView(
    ID3D12Device* This,
    ID3D12Resource* pResource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor
) {
    if (g_cfg.particle_fix != 0 && pDesc && pResource) {
        if (IsBC4ParticleFormat(pDesc->Format) &&
            pDesc->Shader4ComponentMapping == D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING)
        {
            D3D12_RESOURCE_DESC rdesc = pResource->GetDesc();

            // STRICT INTEGRITY GUARD:
            // NEVER touch render targets, depth stencils, or unordered access views!
            // SSAO (R8), Shadow Maps (R16/D32), Scene Depth, and Hi-Z pyramids MUST remain 100% native driver handled!
            if (!(rdesc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                 D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
            {
                // Only touch static 2D / 2D-Array texture assets (particle atlases, masks, droplets)
                if (rdesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC desc_copy = *pDesc;

                    if (g_cfg.particle_fix == 1) {
                        // Correct Opacity Mask Swizzle (5, 5, 5, 0):
                        //   Src0 = 5 (FORCE_VALUE_1 -> R = 1.0)
                        //   Src1 = 5 (FORCE_VALUE_1 -> G = 1.0)
                        //   Src2 = 5 (FORCE_VALUE_1 -> B = 1.0)
                        //   Src3 = 0 (FROM_MEMORY_COMPONENT_0 -> A = Memory[0])
                        // Result:
                        //   RGB = float3(1.0, 1.0, 1.0) -> Preserves 100% emitter vertex color (pure white smoke, golden sparks, clear droplets)
                        //   A   = Memory[0] -> Provides true soft alpha transparency with edge gradient falloff
                        //   Neutralizes "cubos magenta", "png de burbujas", and "manchas negras" simultaneously!
                        desc_copy.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(5, 5, 5, 0);
                    } else if (g_cfg.particle_fix == 2) {
                        // Monochromatic Swizzle (0, 0, 0, 0):
                        desc_copy.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);
                    }

                    LONG count = InterlockedIncrement(&s_particle_fixes_applied);
                    if (count <= 10) {
                        log_msg("[PARTICLES] [FIX #%ld] Applied Particle Swizzle (%s) to BC4 %ux%u (Array=%u, Mips=%u).",
                                count, (g_cfg.particle_fix == 1) ? "5,5,5,0" : "0,0,0,0",
                                (UINT)rdesc.Width, rdesc.Height, rdesc.DepthOrArraySize, rdesc.MipLevels);
                    }

                    s_orig_csrv(This, pResource, &desc_copy, DestDescriptor);
                    return;
                }
            }
        }
    }

    // STRICT INTEGRITY: All multi-channel, color (BC7, RGBA), normal (BC5), depth (R16/R32/D32),
    // SSAO (R8), and null-desc calls pass 100% native!
    s_orig_csrv(This, pResource, pDesc, DestDescriptor);
}

void particles_hook_install(ID3D12Device* pDevice) {
    if (!pDevice) return;

    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;

    if (g_cfg.particle_fix == 0) {
        log_msg("[PARTICLES] Particle swizzle fix is DISABLED by configuration.");
        return;
    }

    void** vtbl = *(void***)pDevice;
    if (!vtbl) return;

    if (vtbl[18] != (void*)Hook_CreateShaderResourceView) {
        DWORD oldProtect;
        if (VirtualProtect(&vtbl[18], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            s_orig_csrv = (PFN_CreateShaderResourceView)vtbl[18];
            vtbl[18] = (void*)Hook_CreateShaderResourceView;
            VirtualProtect(&vtbl[18], sizeof(void*), oldProtect, &oldProtect);
            log_msg("[PARTICLES] Hooked ID3D12Device::CreateShaderResourceView (vtbl[18]) for Particle Alpha Mask Swizzle Fix (Mode=%d).", g_cfg.particle_fix);
        } else {
            log_msg("[PARTICLES] [WARN] VirtualProtect failed on ID3D12Device::CreateShaderResourceView.");
        }
    }
}
