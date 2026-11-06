// Direct3D 12 Particle SRV Swizzle Hook
// Eliminates neon magenta cubes, pink chromatic tint, and opaque billboard quads on BC4 textures
// Restores soft depth particle fading by providing unified neutral luminance & alpha (Data, Data, Data, Data)
// SSAO (R8) and Shadow Maps (R16) remain 100% native driver handled (zero black smudges)
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
                // Only touch static 2D texture assets (particle atlases, masks, droplets)
                if (rdesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
                    D3D12_SHADER_RESOURCE_VIEW_DESC desc_copy = *pDesc;

                    // Neutral Monochromatic Swizzle (0, 0, 0, 0):
                    //   Src0 = 0 (FROM_MEMORY_COMPONENT_0 -> R = Data)
                    //   Src1 = 0 (FROM_MEMORY_COMPONENT_0 -> G = Data)
                    //   Src2 = 0 (FROM_MEMORY_COMPONENT_0 -> B = Data)
                    //   Src3 = 0 (FROM_MEMORY_COMPONENT_0 -> A = Data)
                    //
                    // Visual Truth & Validation (via DXIL disassembly):
                    // 1. 6point_alphafade particle lighting:
                    //    - Component 0 provides smooth opacity/alpha falloff (eliminates solid square bounding boxes).
                    //    - Components 1 & 2 provide neutral isotropic directional lighting vectors (eliminates pink / magenta tint).
                    // 2. Simple particle shaders (fxp_tst_shared_simple_shader):
                    //    - tex.rgb = (Data, Data, Data) multiplies emitter vertex tint cleanly (white smoke, golden sparks, clear droplets).
                    //    - tex.a = Data provides true soft alpha blending (no opaque bounding boxes).
                    // 3. SSAO & Shadows integrity:
                    //    - R8 (SSAO) and R16 (Shadow Maps / Scene Depth) are 100% untouched (eliminates black smudges / "manchas negras").
                    desc_copy.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);

                    LONG count = InterlockedIncrement(&s_particle_fixes_applied);
                    if (count <= 10) {
                        log_msg("[PARTICLES] [FIX #%ld] Applied Monochromatic Swizzle (0,0,0,0) to BC4 %ux%u (Array=%u, Mips=%u).",
                                count, (UINT)rdesc.Width, rdesc.Height, rdesc.DepthOrArraySize, rdesc.MipLevels);
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
            log_msg("[PARTICLES] Hooked ID3D12Device::CreateShaderResourceView (vtbl[18]) for Neutral Swizzle Fix (0,0,0,0).");
        } else {
            log_msg("[PARTICLES] [WARN] VirtualProtect failed on ID3D12Device::CreateShaderResourceView.");
        }
    }
}
