// Direct3D 12 Particle Stability Fix
// Eliminates neon magenta cubes, 2x2/4x4 coarsened particle chunks, and bubble PNG quads
// by disabling Variable Rate Shading (VRS) in ID3D12Device::CheckFeatureSupport (vtbl[13]).
//
// Eliminates the famous ForzaTech + AMD RDNA bug where VRS evaluates coarsened pixel blocks
// with uninitialized or miscalculated color channels on tire smoke, rain droplets, and exhaust backfire.
//
// Architectural Integrity:
// - ID3D12Device::CreateShaderResourceView (vtbl[18]) is strictly LEFT UNHOOKED.
//   Overriding Shader4ComponentMapping on BC4 textures destroyed alpha channels and discard tests,
//   causing crowd billboard cards (Crowds.zip / Autumn_Crowd00_MASK) and cloud shadows (Sky.zip / CloudShadow)
//   to render as massive solid gray walls cutting across the viewport, while exploding 64x64 streetlight flares
//   into blinding white supernovas at night.
// - By cleanly reporting D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED in CheckFeatureSupport:
//   1. ForzaTech renders all particle pipelines at native 1x1 per-pixel resolution (no magenta cubes / no bubble PNGs).
//   2. Crowd billboard cards cleanly discard transparent space (zero flat walls).
//   3. Cloud shadow maps and sky dome render natively (zero gray sky slabs).
//   4. Streetlight flares render natively (zero night supernovas).
//   5. SSAO (R8) and Shadow Maps (R16) render natively (zero black smudges).
//   6. 100% stable: zero descriptor copies, zero memory side effects.
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
static LONG s_vrs_disable_applied = 0;

static HRESULT STDMETHODCALLTYPE Hook_CheckFeatureSupport(
    ID3D12Device* This,
    D3D12_FEATURE Feature,
    void* pFeatureSupportData,
    UINT FeatureSupportDataSize
) {
    HRESULT hr = s_orig_cfs(This, Feature, pFeatureSupportData, FeatureSupportDataSize);

    if (SUCCEEDED(hr) && pFeatureSupportData != NULL && g_cfg.particle_fix != 0) {
        if (Feature == D3D12_FEATURE_D3D12_OPTIONS6) {
            // Feature 30: Variable Rate Shading & Background Processing
            // Reporting NOT_SUPPORTED forces ForzaTech to use native 1x1 full-rate particle evaluation,
            // completely eliminating coarsened 2x2/4x4 magenta cubes and bubble PNG borders.
            D3D12_FEATURE_DATA_D3D12_OPTIONS6* opt6 = (D3D12_FEATURE_DATA_D3D12_OPTIONS6*)pFeatureSupportData;
            opt6->VariableShadingRateTier = D3D12_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
            opt6->AdditionalShadingRatesSupported = FALSE;
            opt6->PerPrimitiveShadingRateSupportedWithViewportIndexing = FALSE;

            LONG hit = InterlockedIncrement(&s_vrs_disable_applied);
            if (hit == 1) {
                log_msg("[PARTICLES] [FIX] Variable Rate Shading (VRS) disabled -> Restored native 1x1 full-rate particle rendering (Zero magenta cubes / Zero bubble PNGs).");
            }
        } else if (Feature == D3D12_FEATURE_D3D12_OPTIONS10) {
            // Feature 39: Mesh Shader Shading Rate & Combiner
            D3D12_FEATURE_DATA_D3D12_OPTIONS10* opt10 = (D3D12_FEATURE_DATA_D3D12_OPTIONS10*)pFeatureSupportData;
            opt10->VariableRateShadingSumCombinerSupported = FALSE;
            opt10->MeshShaderPerPrimitiveShadingRateSupported = FALSE;
        }
    }

    return hr;
}

void particles_hook_install(ID3D12Device* pDevice) {
    if (!pDevice) return;

    static bool s_installed = false;
    if (s_installed) return;
    s_installed = true;

    if (g_cfg.particle_fix == 0) {
        log_msg("[PARTICLES] Particle stability fix is DISABLED by configuration.");
        return;
    }

    void** vtbl = *(void***)pDevice;
    if (!vtbl) return;

    // Hook ID3D12Device::CheckFeatureSupport (vtbl[13]) to disable VRS.
    // ID3D12Device::CreateShaderResourceView (vtbl[18]) is strictly LEFT UNTOUCHED:
    // This guarantees crowd cards, sky shadows, streetlight flares, and SSAO/shadows
    // retain 100% native driver handling and proper alpha discard.
    if (vtbl[13] != (void*)Hook_CheckFeatureSupport) {
        DWORD oldProtect;
        if (VirtualProtect(&vtbl[13], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            s_orig_cfs = (PFN_CheckFeatureSupport)vtbl[13];
            vtbl[13] = (void*)Hook_CheckFeatureSupport;
            VirtualProtect(&vtbl[13], sizeof(void*), oldProtect, &oldProtect);
            log_msg("[PARTICLES] Hooked ID3D12Device::CheckFeatureSupport (vtbl[13]) for Full-Rate 1x1 Particle Fix (VRS Disabled).");
        } else {
            log_msg("[PARTICLES] [WARN] VirtualProtect failed on ID3D12Device::CheckFeatureSupport.");
        }
    }
}
