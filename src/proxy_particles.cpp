// Direct3D 12 Particle SRV Swizzle Hook
// Eliminates neon magenta cubes, pink chromatic tint, and opaque billboard quads on BC4 particle textures
// Restores soft depth particle fading by providing unified neutral luminance & alpha (Data, Data, Data, Data)
// Streetlights at night (Flare_strobe_streetlight 64x64), colored flares, Moon, and gradient ramps
// remain 100% native driver handled (eliminates blinding night supernovas / "ES DE NOCHE")
// SSAO (R8) and Shadow Maps (R16) remain 100% native driver handled (zero black smudges)
#include <d3d12.h>
#include <stdio.h>
#include "proxy_common.h"

// WKPDID_D3DDebugObjectNameW: 4fc37827-4fc1-4424-ac6b-31d798e10986
static const GUID s_WKPDID_D3DDebugObjectNameW = { 0x4fc37827, 0x4fc1, 0x4424, { 0xac, 0x6b, 0x31, 0xd7, 0x98, 0xe1, 0x09, 0x86 } };

typedef void (STDMETHODCALLTYPE *PFN_CreateShaderResourceView)(
    ID3D12Device* This,
    ID3D12Resource* pResource,
    const D3D12_SHADER_RESOURCE_VIEW_DESC* pDesc,
    D3D12_CPU_DESCRIPTOR_HANDLE DestDescriptor
);

static PFN_CreateShaderResourceView s_orig_csrv = NULL;
static LONG s_particle_fixes_applied = 0;
static LONG s_particle_skipped = 0;

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

            // STRICT INTEGRITY GUARD 1:
            // NEVER touch render targets, depth stencils, or unordered access views!
            // SSAO (R8), Shadow Maps (R16/D32), Scene Depth, and Hi-Z pyramids MUST remain 100% native driver handled!
            if (!(rdesc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET |
                                 D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
                                 D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
            {
                // STRICT INTEGRITY GUARD 2:
                // Only touch static 2D texture assets that are genuine particle flipbooks or atlases:
                //
                // 1. Streetlight & Flare Isolation (Fix for "ES DE NOCHE" White Supernova):
                //    - Small streetlight flares (Flare_strobe_streetlight 64x64 Mips=7, Flare_strobe_white 64x64)
                //    - Colored light flares (Flare_amber, Flare_blue, Flare_red 64x64)
                //    - Tiny normal/flash sprites (32x32, 128x128)
                //    Under LightGlow_Flare.pshader, Component 1 (Green) is read as the flare radius multiplier:
                //      %44 = FMax(Texture G, MinLength)
                //      %45 = FMin(%44, MaxLength)
                //    Native DirectX 12 hardware mapping sets Green = 0.0f (FORCE_VALUE_0).
                //    By preserving native mapping for small textures (Width < 256 || Height < 128 || Mips < 8),
                //    Green remains 0.0f, flare radius stays MinLength (~5 pixels), and streetlights NEVER explode!
                //
                // 2. Non-Square Gradient Ramp Isolation:
                //    - Exclude 1D gradient ramps (*GRAD.swatchbin: 256x1, 256x2, 256x4, 256x16 where Height <= 16)
                //
                // 3. Genuine Particle Atlases & Flipbooks:
                //    - Smoke, tire smoke, dust, mud, water spray, rain bubbles, snow, debris:
                //      * Square sheets: 256x256, 512x512, 1024x1024, 2048x2048, 4096x4096 (MipLevels >= 8)
                //      * 2:1 flipbooks: 256x128, 512x256, 1024x512, 2048x1024 (MipLevels >= 8)
                //      * Width >= 256 && Height >= 128 && MipLevels >= 8 && Width >= Height && (Width / Height) <= 2
                if (rdesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
                    rdesc.Width >= 256 &&
                    rdesc.Height >= 128 &&
                    rdesc.MipLevels >= 8 &&
                    rdesc.Width >= rdesc.Height &&
                    (rdesc.Width / rdesc.Height) <= 2)
                {
                    // Secondary Name Check (if debug names are present in runtime):
                    bool is_sky_or_flare = false;
                    wchar_t wname[128] = {0};
                    UINT wlen = sizeof(wname);
                    if (SUCCEEDED(pResource->GetPrivateData(s_WKPDID_D3DDebugObjectNameW, &wlen, wname))) {
                        if (wcsstr(wname, L"Moon") || wcsstr(wname, L"moon") ||
                            wcsstr(wname, L"Sky")  || wcsstr(wname, L"sky")  ||
                            wcsstr(wname, L"Flare")|| wcsstr(wname, L"flare")||
                            wcsstr(wname, L"Light")|| wcsstr(wname, L"light"))
                        {
                            is_sky_or_flare = true;
                        }
                    }

                    if (!is_sky_or_flare) {
                        D3D12_SHADER_RESOURCE_VIEW_DESC desc_copy = *pDesc;

                        // Neutral Monochromatic Swizzle (0, 0, 0, 0):
                        //   Src0 = 0 (FROM_MEMORY_COMPONENT_0 -> R = Data)
                        //   Src1 = 0 (FROM_MEMORY_COMPONENT_0 -> G = Data)
                        //   Src2 = 0 (FROM_MEMORY_COMPONENT_0 -> B = Data)
                        //   Src3 = 0 (FROM_MEMORY_COMPONENT_0 -> A = Data)
                        //
                        // Visual Truth & Validation:
                        // - Pure neutral grayscale RGB eliminates neon pink and magenta cubes.
                        // - Memory[0] in Alpha provides smooth transparency falloff, eliminating solid PNG bounding boxes.
                        desc_copy.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 0, 0, 0);

                        LONG count = InterlockedIncrement(&s_particle_fixes_applied);
                        if (count <= 20) {
                            log_msg("[PARTICLES] [FIX #%ld] Applied Monochromatic Swizzle (0,0,0,0) to Particle Atlas BC4 %ux%u (Mips=%u).",
                                    count, (UINT)rdesc.Width, rdesc.Height, rdesc.MipLevels);
                        }

                        s_orig_csrv(This, pResource, &desc_copy, DestDescriptor);
                        return;
                    }
                } else {
                    LONG skip = InterlockedIncrement(&s_particle_skipped);
                    if (skip <= 10) {
                        log_msg("[PARTICLES] [NATIVE] Preserved native driver mapping for BC4 %ux%u (Mips=%u) - Flare/Ramp/Sky protected.",
                                (UINT)rdesc.Width, rdesc.Height, rdesc.MipLevels);
                    }
                }
            }
        }
    }

    // STRICT INTEGRITY: All multi-channel, color (BC7, RGBA), normal (BC5), depth (R16/R32/D32),
    // SSAO (R8), streetlight flares (64x64), and null-desc calls pass 100% native!
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
            log_msg("[PARTICLES] Hooked ID3D12Device::CreateShaderResourceView (vtbl[18]) for Isolated Particle Swizzle Fix (0,0,0,0).");
        } else {
            log_msg("[PARTICLES] [WARN] VirtualProtect failed on ID3D12Device::CreateShaderResourceView.");
        }
    }
}
