// AMD AGS Proxy - Main Entry Point & Forwarder
#include <stdio.h>
#include <stdint.h>
#include "proxy_common.h"
#include "forwarders.h"

typedef int (*PFN_agsInitialize)(int agsVersion, const void* config, void** context, void* gpuInfo);
typedef int (*PFN_agsGetGPUInfo)(void* context, void* gpuInfo);
typedef int (*PFN_agsDriverExtensionsDX12_CreateDevice)(void* context, const void* deviceCreationParams, const void* extensionParams, void* returnedParams);

static HMODULE s_hOrig = NULL;
static PFN_agsInitialize s_pfn_agsInitialize = NULL;
static PFN_agsGetGPUInfo s_pfn_agsGetGPUInfo = NULL;
static PFN_agsDriverExtensionsDX12_CreateDevice s_pfn_agsDriverExtensionsDX12_CreateDevice = NULL;

static BOOL EnsureOrigLoaded() {
    if (!s_hOrig) {
        s_hOrig = LoadLibraryA("amd_ags_x64_orig.dll");
        if (!s_hOrig) {
            DWORD err = GetLastError();
            g_tel.st_ags_load = st_fail;
            log_msg("[AGS] [FAIL] Could not load amd_ags_x64_orig.dll (error=%lu)", err);
            return FALSE;
        }
        g_tel.st_ags_load = st_ok;
        s_pfn_agsInitialize = (PFN_agsInitialize)GetProcAddress(s_hOrig, "agsInitialize");
        s_pfn_agsGetGPUInfo = (PFN_agsGetGPUInfo)GetProcAddress(s_hOrig, "agsGetGPUInfo");
        s_pfn_agsDriverExtensionsDX12_CreateDevice = (PFN_agsDriverExtensionsDX12_CreateDevice)GetProcAddress(s_hOrig, "agsDriverExtensionsDX12_CreateDevice");
        log_msg("[AGS] Successfully loaded amd_ags_x64_orig.dll and resolved entry points.");
    }
    return TRUE;
}

static void InspectAGSGPUInfo(void* gpuInfo) {
    if (!gpuInfo) return;

    uint8_t* base = (uint8_t*)gpuInfo;
    int numDevices = *(int*)(base + 16);
    uint8_t* devicesPtr = *(uint8_t**)(base + 24);

    log_msg("[AGS] Inspecting AGSGPUInfo: numDevices=%d, devicesPtr=%p", numDevices, devicesPtr);

    if (numDevices > 0 && devicesPtr != NULL) {
        uint8_t* dev = devicesPtr; // Primary device
        const char* adapterStr = *(const char**)dev;
        int* pFamily = (int*)(dev + 8);
        int* pVendor = (int*)(dev + 16);
        int* pDevice = (int*)(dev + 20);
        int* pRevision = (int*)(dev + 24);
        int* pNumCUs = (int*)(dev + 28);
        int* pNumWGPs = (int*)(dev + 32);
        int* pNumROPs = (int*)(dev + 36);
        float* pTeraFlops = (float*)(dev + 52);
        uint64_t* pLocalMemoryBytes = (uint64_t*)(dev + 56);

        if (adapterStr && strlen(adapterStr) > 0) {
            strncpy_s(g_tel.gpu_name, adapterStr, sizeof(g_tel.gpu_name) - 1);
        }
        g_tel.vendor_id = *pVendor;
        g_tel.device_id = *pDevice;
        g_tel.asic_family = *pFamily;
        g_tel.num_cus = *pNumCUs;
        g_tel.num_wgps = *pNumWGPs;
        g_tel.num_rops = *pNumROPs;
        g_tel.ags_local_mem = *pLocalMemoryBytes;

        // If WGP is 0 but CUs exist, compute WGPs dynamically (standard RDNA = 2 CUs per WGP)
        if (*pNumWGPs <= 0 && *pNumCUs > 0) {
            *pNumWGPs = *pNumCUs / 2;
            g_tel.num_wgps = *pNumWGPs;
        }

        log_msg("[AGS] Native GPU Telemetry: '%s' | Vendor=0x%X Device=0x%X Family=%d CUs=%d WGPs=%d ROPs=%d VRAM=%llu MB",
                g_tel.gpu_name[0] ? g_tel.gpu_name : "Unknown",
                *pVendor, *pDevice, *pFamily, *pNumCUs, *pNumWGPs, *pNumROPs,
                *pLocalMemoryBytes / (1024 * 1024));
    }
}

extern "C" __declspec(dllexport) int agsInitialize(int agsVersion, const void* config, void** context, void* gpuInfo) {
    if (!EnsureOrigLoaded()) {
        g_tel.st_ags_init = st_fail;
        return 1;
    }

    // Ensure VRAM hook is installed when game initializes AGS (outside of loader lock)
    vram_hook_install();

    log_msg("[AGS] agsInitialize called (agsVersion=0x%X)", agsVersion);
    int res = s_pfn_agsInitialize(agsVersion, config, context, gpuInfo);
    g_tel.ags_init_result = res;
    g_tel.st_ags_init = (res == 0 ? st_ok : st_fail);

    log_msg("[AGS] agsInitialize result: %d", res);
    if (res == 0 && gpuInfo != NULL) {
        InspectAGSGPUInfo(gpuInfo);
    }
    return res;
}

extern "C" __declspec(dllexport) int agsGetGPUInfo(void* context, void* gpuInfo) {
    if (!EnsureOrigLoaded()) return 1;
    int res = s_pfn_agsGetGPUInfo(context, gpuInfo);
    if (res == 0 && gpuInfo != NULL) {
        InspectAGSGPUInfo(gpuInfo);
    }
    return res;
}

extern "C" __declspec(dllexport) int agsDriverExtensionsDX12_CreateDevice(
    void* context, 
    const void* deviceCreationParams, 
    const void* extensionParams, 
    void* returnedParams
) {
    if (!EnsureOrigLoaded()) {
        g_tel.st_dx12 = st_fail;
        return 1;
    }

    // Ensure VRAM hook is active
    vram_hook_install();

    log_msg("[AGS] agsDriverExtensionsDX12_CreateDevice called");
    int res = s_pfn_agsDriverExtensionsDX12_CreateDevice(context, deviceCreationParams, extensionParams, returnedParams);
    g_tel.dx12_result = res;
    g_tel.st_dx12 = (res == 0 ? st_ok : st_fail);

    if (res == 0 && returnedParams != NULL) {
        ID3D12Device* pDevice = *(ID3D12Device**)returnedParams;
        if (pDevice) {
            dx12_hook_install(pDevice);
        }
        uint32_t* pExt = (uint32_t*)((uint8_t*)returnedParams + sizeof(void*));
        g_tel.ext_bits = *pExt;
        log_msg("[AGS] Native AMD Driver Extensions: 0x%08X (Preserved 100%% Genuine Driver Capabilities - Zero UAV/Intrinsics Hijacking)", g_tel.ext_bits);
    } else {
        log_msg("[AGS] agsDriverExtensionsDX12_CreateDevice failed with code %d", res);
    }
    return res;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);

        open_log_and_config();
        identify_game();
        guard_install();
        sched_governor_start();
        overlay_start();

        // VRAM hook is initialized when overlay thread starts or when agsInitialize is called.
    } else if (reason == DLL_PROCESS_DETACH) {
        log_msg("[PROXY] Proxy detaching from process.");
    }
    return TRUE;
}
