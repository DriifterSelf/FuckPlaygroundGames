# FuckPlaygroundGames (FPG)
### AMD AGS & DXGI VRAM / Memory Optimizer Proxy for ForzaTech (Forza Horizon / Forza Motorsport)

[![Build and Package Proxy](https://github.com/DriifterSelf/FuckPlaygroundGames/actions/workflows/build.yml/badge.svg)](https://github.com/DriifterSelf/FuckPlaygroundGames/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Release](https://img.shields.io/github/v/release/DriifterSelf/FuckPlaygroundGames)](https://github.com/DriifterSelf/FuckPlaygroundGames/releases)

**FuckPlaygroundGames (FPG)** is a clean-room, open-source native C++20 proxy DLL (`amd_ags_x64.dll`) engineered to eliminate persistent engine defects, artificial memory throttling, and allocator crash loops in Playground Games / Turn 10 PC titles (Forza Horizon 5, Forza Horizon 6, Forza Motorsport).

---

## 🔬 How It Works (Deep Technical Architecture)

Unlike generic injectors or memory hacks, FPG works as a transparent low-level shim between the **ForzaTech engine**, the **DirectX 12 runtime**, and the **AMD GPU Services (AGS) driver**.

```
+--------------------------------------------------------------------------+
|                       ForzaTech Engine Executable                        |
|                           (forzahorizon6.exe)                            |
+--------------------+--------------------------------+--------------------+
                     |                                |
   DXGI Query Calls  |              AGS API Calls     |
                     v                                v
+--------------------+--------------+   +-------------+--------------------+
|  IDXGIAdapter3::QueryVideoMemory  |   |        FPG Proxy Library          |
|      vtable[14] Hook (Hook_QVMI)  |   |        (amd_ags_x64.dll)          |
+--------------------+--------------+   +-------------+--------------------+
                     |                                |
  Overridden Budget  |                Export Forward  |  (34 exports)
  (Real VRAM - 8%)   |                                v
                     |                  +-------------+--------------------+
                     |                  |     Original AMD AGS DLL         |
                     |                  |    (amd_ags_x64_orig.dll)        |
                     |                  +-------------+--------------------+
                     |                                |
                     v                                v
+-----------------------------------------------------+--------------------+
|                   AMD Adrenalin Driver & Windows WDDM                    |
|                        (Radeon RX 9070 XT, etc.)                         |
+--------------------------------------------------------------------------+
```

---

### 1. The Proxy Architecture (`src/proxy_main.cpp` & `src/forwarders.h`)
* When the game starts, it links to `amd_ags_x64.dll`. FPG replaces this file and renames the genuine AMD library to `amd_ags_x64_orig.dll`.
* **Zero Overhead Forwarding:** 34 non-critical exports (e.g. DX11 breadcrumbs, markers, topologies) are forwarded directly to `amd_ags_x64_orig.dll` at the PE export directory level via MSVC linker directives (`#pragma comment(linker, "/EXPORT:...")`). These execute with zero CPU overhead.
* **Intercepted Functions:** FPG intercepts 3 primary initialization routines:
  1. `agsInitialize`: Validates driver communication, inspects GPU properties, and installs the DXGI budget hook safely outside Windows Loader Lock.
  2. `agsGetGPUInfo`: Extracts live GPU telemetry (Device ID, Vendor ID, ASIC Family, CUs, WGPs, ROPs, and VRAM).
  3. `agsDriverExtensionsDX12_CreateDevice`: Monitors enabled hardware capabilities (WaveMatrix/WMMA, 16/17-bit Intrinsics, Shader Clock).

---

### 2. The 13 GB VRAM Bug & Dynamic DXGI Hooking (`src/proxy_vram.cpp`)

#### The Underlying Flaw in ForzaTech:
* In Windows 10/11 (WDDM 2.0+), Windows reserves roughly **18% to 20%** of GPU memory for Desktop Window Manager (`dwm.exe`), browser tabs, and OS paging headroom.
* When a game calls `IDXGIAdapter3::QueryVideoMemoryInfo`, the OS returns two values: `Budget` and `CurrentUsage`.
* On a **16 GB GPU** (16,191 MB Dedicated), Windows calculates:
  $$\text{Budget} \approx 16,191 \text{ MB} \times 0.81 = \sim 13,100 \text{ MB to } 13,400 \text{ MB}$$
* Most modern engines use `DedicatedVideoMemory` or stream adaptively. **ForzaTech, however, treats `Budget` (~13.1 GB) as the hard maximum capacity of the graphics card.**
* When asset usage reaches ~11.8 GB (90% of 13.1 GB), the engine panics:
  - It triggers the on-screen warning: *"Low Video Memory: Current video memory usage is near budget."*
  - It throttles texture mip-levels, causing blurry liveries, low-resolution road surfaces, and severe stuttering as it prematurely evicts resources — **even though more than 3 GB of physical GDDR6 remains completely empty.**

#### The Hook Implementation:
FPG installs an in-memory virtual method table hook on the active DXGI adapter:
1. Enumerates hardware adapters using `CreateDXGIFactory1` and `EnumAdapters1`.
2. Queries the `IDXGIAdapter3` COM interface and resolves its virtual method table (`vtbl`).
3. Replaces `vtbl[14]` (`QueryVideoMemoryInfo`) with `Hook_QueryVideoMemoryInfo`:
   ```cpp
   // Intercepts the engine's query before it receives the struct:
   HRESULT hr = s_orig_qvmi(This, NodeIndex, MemorySegmentGroup, pVideoMemoryInfo);
   if (SUCCEEDED(hr) && MemorySegmentGroup == DXGI_MEMORY_SEGMENT_GROUP_LOCAL) {
       // Dedicated physical memory (e.g. 16,191 MB)
       UINT64 dedicated = desc.DedicatedVideoMemory;
       
       // Dynamic safety reserve (default 8% = ~1,295 MB for Windows DWM)
       UINT64 margin = (dedicated * g_cfg.vram_margin_percent) / 100ULL;
       UINT64 target_budget = dedicated - margin; // ~14,896 MB (~14.9 GB)
       
       if (pVideoMemoryInfo->Budget < target_budget) {
           pVideoMemoryInfo->Budget = target_budget; // Uncap budget
       }
   }
   ```
4. **Why 8% margin instead of 3%?**
   Earlier builds experimented with an aggressive 3% margin (485 MB). However, Windows DWM and background desktop tasks require 800 MB to 1.2 GB. Under a 3% margin, total system VRAM demand exceeded the physical 16.2 GB hardware ceiling, forcing AMD's D3D12 driver to reject allocations with `0x8007000E` (`E_OUTOFMEMORY`). Setting `VramMarginPercent = 8%` provides a massive **~1.8 GB of extra usable VRAM** to the engine while ensuring DirectX never oversubscribes physical memory.

---

### 3. Fail-Fast Panic Guard & Memory Leak Autopsy (`src/proxy_guard.cpp`)

#### The Game's Fatal Assertion:
In ForzaTech, when the internal model buffer allocator (`SYSTEM_MODEL_BUFFER_ALLOCATOR`) encounters an allocation failure (`0x8007000E` / `E_OUTOFMEMORY`), Playground Games embedded a hardcoded fail-fast panic:
```assembly
mov dword ptr [0], 0   ; C7 04 25 00 00 00 00 00 00 00 00
```
This forces an instant `STATUS_ACCESS_VIOLATION` (`0xC0000005`) crash to desktop.

#### The Crash Dump Autopsy (`forzahorizon6.exe.20616.dmp` & v3.1 Architecture):
Analysis with disassembly of the crash dumps revealed the two-stage failure sequence:
1. When the allocator failed with `E_OUTOFMEMORY`, Playground Games' code called a fatal assertion handler (`0x7ff7cc909140` -> `0x34BA060`) that intentionally executed `mov dword ptr [0], 0`.
2. A naive guard simply skipping the 11-byte null write instruction (`mov [0], 0`) allowed the function to return to `0xFECFCA`, where the caller assumed allocation had succeeded:
   ```assembly
   00007ff7`cc8ecfca 488b0e      mov rcx, qword ptr [rsi]     ; rcx = 0x0000000000000000 (NULL because allocation failed)
   00007ff7`cc8ecfcd 488b5128    mov rdx, qword ptr [rcx+28h] ; CRASH! Access violation reading 0x28
   00007ff7`cc8ecfd1 488b8c24c0  mov rcx, qword ptr [rsp+c0h] ; Destination pointer
   00007ff7`cc8ecfd9 488911      mov qword ptr [rcx], rdx
   00007ff7`cc8ecfdc e989000000  jmp 00007ff7`cc8ed06a        ; Safe epilogue
   ```
3. **v3.1 Dual-Stage Active Guard:**
   - **Stage 1 (Assertion Interception):** Suppresses the `mov dword ptr [0], 0` fail-fast assertion at `0x34BA060`.
   - **Stage 2 (Active Fallback Recovery):** Intercepts the secondary NULL dereference at `0xFECFCD`, sets `rdx = 0`, sets `rax = 0` (clean false return), safely nullifies the output resource pointer at `[rsp + 0xC0]`, and redirects execution to the clean epilogue `0xFED06A` (`rip + 0x9D`). This guarantees zero crash and allows the engine's streaming fallback to use existing lower mips smoothly!
4. **v3.1 Dynamic Pressure Feedback Loop (`src/proxy_vram.cpp`):**
   - Eliminates the root cause of OOM. Instead of a static ceiling, when `CurrentUsage` approaches `max_safe_budget` (< 1 GB headroom), the reported budget dynamically contracts towards `CurrentUsage`.
   - When within 256 MB of physical limits, it reports `Budget = CurrentUsage - 64MB`, triggering ForzaTech's internal LRU cache collector to evict stale textures/meshes before `CreateCommittedResource` can ever fail.

---

### 4. Non-Intrusive In-Game Diagnostic HUD (`src/proxy_overlay.cpp`)
* Spawns on an isolated worker thread (`OverlayThread`), completely decoupled from the game's message and render pipeline.
* Built as a click-through, non-activating layered Win32 window:
  `WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`
* **Zero Input Interruption:** Mouse clicks and controller inputs pass directly through the HUD into the game.
* **Hotkey Listener:** Uses non-blocking `GetAsyncKeyState(VK_F10)` / `GetAsyncKeyState(VK_INSERT)` to toggle HUD visibility.
* **State Machine:** Every module reports live health with color coding:
  - `[OK]` (Green): Fully attached and operating normally.
  - `[PENDING]` (Yellow): Waiting for the game engine to invoke the target DirectX interface.
  - `[FAILED]` (Red): An error occurred (displays exact Win32 / HRESULT error code).
  - `[DISABLED]` (Gray): Feature turned off in `rdna4_ags_proxy.ini`.

---

### 5. Deterministic Hybrid CPU Cluster Governor (`src/proxy_sched.cpp`)
* **Dual-Cluster Partitioning:**
  - **Performance Cluster (`0x0FFF`, Cores 0-11):** Frame-critical engine threads (`tpBasePri >= 9`), including Render Thread, Main Simulation Loop, TaskGraph Workers, Physics Solver, and Audio Mixer, share all 6 Golden Cove P-Cores with 4.6 GHz clocks and unified 20 MB L3 cache.
  - **Efficiency Cluster (`0xF000`, Cores 12-15):** Background tasks (`tpBasePri <= 8`), including DirectStorage disk I/O, Oodle texture/mesh decompression, telemetry, and HTTP networking, are confined exclusively to Gracemont E-Cores. They never steal cycles or cache lines from P-Cores.
* **100% Elimination of Convoy Stalls:**
  Because the P-Core mask completely excludes E-Cores (`0xF000`), no TaskGraph worker can ever be scheduled on a Gracemont core. Barrier completion times remain deterministic with zero join stalls.
* **Zero Dynamic Migrations (Immutable Affinities):**
  Threads are classified and affinitized **exactly once** upon creation. Dynamic bouncing between cores is eliminated (`sched_migrations = 0`), keeping L1/L2 instruction and data caches warm at all times.
* **Hardware QoS & Power Throttling Integration:**
  Directly configures Windows 11 `ThreadPowerThrottling` (`HighPerformance` for P-Core threads, `EcoQoS` for E-Core threads) and MMCSS `"Games"` multimedia quantum scheduling.

---

## 🎮 Recommended Game Settings (Anti-Leak)

In `%LOCALAPPDATA%\Packages\Microsoft.ForteBaseGame_...\LocalCache\Local\LocalStorage_Shared\ForzaUserConfigSelections\UserConfigSelections`:

| Setting | Recommended Value | Reason |
| :--- | :---: | :--- |
| **Optimización Dinámica (UseDynamicOptimization)** | **ON (1)** | **Critical.** When OFF (0), ForzaTech disables asset garbage collection, leaking memory until it crashes. |
| **Calidad de Texturas de Entorno** | Ultra / Extreme | With FPG's 14.9 GB budget, Ultra and Extreme run smoothly without downsampling. |
| **Reflejos Ray Tracing** | Ultra | Extreme RT Reflections generate massive BVH acceleration trees that degrade stability over long sessions. |
| **Paginación de Windows (Pagefile)** | **Mínimo 16 GB** | DX12 committed allocations require virtual memory commit backing. A 4 GB pagefile risks commit exhaustion. |

---

## 📥 Installation

1. Download `amd_ags_x64.dll` from the latest [Release](https://github.com/DriifterSelf/FuckPlaygroundGames/releases).
2. Open your Forza installation folder (e.g. for Xbox / WindowsApps):
   ```text
   C:\Program Files\WindowsApps\Microsoft.ForteBaseGame_3.461.691.0_x64__8wekyb3d8bbwe\
   ```
3. Rename the original `amd_ags_x64.dll` to:
   ```text
   amd_ags_x64_orig.dll
   ```
4. Copy the downloaded `amd_ags_x64.dll` into the game directory.
5. Launch the game normally. Press **`F10`** or **`Insert`** to toggle the diagnostic HUD.

---

## ⚙️ Configuration (`rdna4_ags_proxy.ini`)

Located in `%LOCALAPPDATA%\rdna4_ags_proxy.ini`:

```ini
[proxy]
; In-game diagnostic overlay (1 = ON, 0 = OFF)
Overlay=1

; Hotkey to toggle overlay (121 = VK_F10, 45 = VK_INSERT)
OverlayToggleKey=121

; Uncap VRAM budget from Windows WDDM default (1 = ON, 0 = OFF)
VramUncap=1

; Safety reserve percent for Windows DWM and driver overhead (default 8% = ~1.3 GB)
VramMarginPercent=8

; Vectored Exception Handler to monitor allocator faults (1 = ON, 0 = OFF)
OomGuard=1

; Maximum allocation faults to log before letting exception pass
OomGuardMaxSuppress=20
```

---

## 🛠️ Building

### Build Locally (MSVC x64)
Run from a Developer Command Prompt:
```cmd
build.bat
```
Output: `bin\amd_ags_x64.dll`.

### Build with CMake
```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```
Output: `build\Release\amd_ags_x64.dll`.

---

## 📄 License

MIT License — Copyright (c) 2026 DriifterSelf.  
Clean-room implementation verified by runtime telemetry and WinDbg crash analysis.
