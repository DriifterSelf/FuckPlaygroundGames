# FuckPlaygroundGames (FPG) - Development Rules & Guidelines

- **Immutable CPU Scheduler & Core Affinity (Strictly Locked)**:
  - The CPU scheduler and core affinity architecture in `src/proxy_sched.cpp` (v3.11.0+) is strictly LOCKED and finalized. NEVER change, refactor, or alter thread affinity masks, core pin heuristics, or dynamic migrations.
  - **Dual-Cluster Partitioning**:
    - Performance Cluster (`s_p_core_mask`, `0x0FFF` for 6P/12T): All frame-critical engine threads (`tpBasePri >= 9`), including Render Thread, Main Simulation Loop, TaskGraph Workers, Physics Dynamics, and Audio Mixer, are permanently affinitized to Performance Cores with High-Performance QoS (EcoQoS OFF).
    - Efficiency Cluster (`s_e_core_mask`, `0xF000` for 4E/4T): All background streaming, DirectStorage I/O, Oodle texture decompression, and telemetry threads (`tpBasePri <= 8`) are dedicated to Efficiency Cores with EcoQoS.
  - **Zero-Migration Guarantee**: Threads must be affinitized strictly ONCE upon creation (`IsThreadConfigured()`). NEVER introduce dynamic re-evaluation, ping-ponging, or micro-pinning to single physical cores (e.g. no 0x0003 or 0x000C single-core locks).
  - **Configuration Persistence**: Maintain `HybridCoreGovernor=1` enabled and deterministic. Zero changes to mods or in-game graphics settings.
