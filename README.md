# FuckPlaygroundGames (FPG)
### AMD AGS & DXGI VRAM / Memory Proxy for ForzaTech (Forza Horizon / Forza Motorsport)

[![Build and Package Proxy](https://github.com/DriifterSelf/FuckPlaygroundGames/actions/workflows/build.yml/badge.svg)](https://github.com/DriifterSelf/FuckPlaygroundGames/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

A lightweight, clean-room, non-intrusive native proxy library (`amd_ags_x64.dll`) designed to fix long-standing engine flaws, memory allocation crashes, and artificial VRAM limits in Playground Games / Turn 10 PC titles (Forza Horizon 5, Forza Horizon 6, Forza Motorsport).

---

## 🏎️ Key Features

### 1. Fixes the "13 GB VRAM Bug" on 16 GB GPUs
* **The Problem:** In Windows WDDM 2.0+, Windows reserves ~18–20% of video memory for the desktop compositor (DWM) and background apps. On 16 GB cards (e.g. AMD Radeon RX 9070 XT, RX 7900 GRE, RX 7800 XT, RX 6800 XT), `IDXGIAdapter3::QueryVideoMemoryInfo` reports a budget of only **~13.1 GB**. ForzaTech uses this `Budget` as a hard limit for texture streaming and graphical settings, causing premature *"Low Video Memory"* alerts, texture pop-in, and aggressive texture downsampling even with 3+ GB of physical GDDR6 sitting completely empty.
* **The Solution:** FPG dynamically hooks `IDXGIAdapter3::QueryVideoMemoryInfo` (vtable slot 14) and uncaps the budget to the card's real `DedicatedVideoMemory` minus a safe, configurable reserve (default 8% = ~1.3 GB for Windows DWM), giving the engine **~14.9 GB** of usable headroom without oversubscribing physical VRAM.

### 2. Zero CPU Affinity Tampering (100% Non-Intrusive)
* Does **NOT** alter process affinity masks (`0xFFFF`), thread priorities, or P-core / E-core assignment.
* Respects custom OS scheduling policies, QoS rules, and external process tuners. The CPU affinity is queried in read-only mode purely for real-time diagnostic reporting.

### 3. Fully Dynamic & "Self-Aware" Architecture
* **Zero Hardcoded Offsets:** Game base address and memory bounds are resolved in real time by parsing the PE headers (`IMAGE_NT_HEADERS`).
* **Dynamic Hardware Detection:** Direct query to the AMD driver and AGS for GPU model, compute units (CUs), workgroup processors (WGPs), RDNA architecture family, and ROPs.
* **Failure Reporting:** If any hook, DirectX interface, or driver extension fails to attach, the state machine marks it immediately with `[FAILED]` on screen and writes the exact HRESULT error code to disk.

### 4. Real-Time In-Game Diagnostic HUD
* Floating semi-transparent diagnostic overlay in the top-left corner.
* Click-through (`WS_EX_TRANSPARENT`) and non-focus stealing (`WS_EX_NOACTIVATE`) so it never interrupts gameplay or controller input.
* **Hotkey:** Press **`F10`** or **`Insert`** at any time to toggle the overlay on/off.
* Displays:
  - Active game executable and PE timestamp.
  - Native GPU telemetry reported by the AMD driver.
  - Live VRAM usage / budget (e.g., `• VRAM Pool: [OK] 11.8 / 14.9 GB (Max: 16.2 GB) | Calls: 4812`).
  - Memory guard status and suppressed allocation assertions.
  - DirectX 12 driver extension flags (WaveMatrix/WMMA, Intrinsics, ShaderClock).
  - Current CPU affinity mask.

---

## 📥 Installation

1. Go to the [Releases](https://github.com/DriifterSelf/FuckPlaygroundGames/releases) or the latest **Actions** build artifact and download `amd_ags_x64.dll`.
2. Navigate to your game installation directory (e.g., for Xbox / WindowsApps or Steam):
   ```text
   C:\Program Files\WindowsApps\Microsoft.ForteBaseGame_...\
   ```
3. Locate the original `amd_ags_x64.dll` and rename it to:
   ```text
   amd_ags_x64_orig.dll
   ```
4. Copy the downloaded `amd_ags_x64.dll` into the game directory.
5. Launch the game normally.

---

## ⚙️ Configuration (`rdna4_ags_proxy.ini`)

The proxy creates a configuration file in `%LOCALAPPDATA%\rdna4_ags_proxy.ini` upon first launch:

```ini
[proxy]
; Enable in-game diagnostic HUD (1 = ON, 0 = OFF)
Overlay=1

; Virtual key code to toggle HUD (121 = F10, 45 = Insert)
OverlayToggleKey=121

; Uncap VRAM budget from Windows default (1 = ON, 0 = OFF)
VramUncap=1

; Percentage of physical VRAM reserved for Windows DWM and OS tasks (default 8% = ~1.3 GB on 16GB cards)
VramMarginPercent=8

; Vectored Exception Handler to monitor allocator faults (1 = ON, 0 = OFF)
OomGuard=1

; Maximum allocation faults to log before letting exception pass
OomGuardMaxSuppress=20
```

---

## 🛠️ Building from Source

### Requirements
* Windows 10/11 x64
* Visual Studio 2022 (with MSVC C++ x64 build tools and Windows SDK)
* CMake 3.20+ (optional, or use `build.bat`)

### Option A: Direct Batch Build
Open developer command prompt or run:
```cmd
build.bat
```
The compiled DLL will be located in `bin\amd_ags_x64.dll`.

### Option B: CMake
```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```
The compiled DLL will be located in `build\Release\amd_ags_x64.dll`.

---

## 📜 Continuous Integration (CI)

Every commit pushed to `main` is automatically compiled and packaged using GitHub Actions:
* **Workflow:** `.github/workflows/build.yml`
* **Artifacts:** Pre-compiled `amd_ags_x64.dll` binaries are generated in the GitHub Actions run summary for instant download.

---

## 📄 License

This project is licensed under the **MIT License** — see the [LICENSE](LICENSE) file for details.
Clean-room implementation written with fresh reverse-engineered telemetry and standard Windows / DirectX / AGS APIs.
