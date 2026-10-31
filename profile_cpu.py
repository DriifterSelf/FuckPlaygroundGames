import ctypes
import ctypes.wintypes as wintypes
import time
import os

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

THREAD_QUERY_INFORMATION = 0x0040
THREAD_SET_INFORMATION = 0x0020
TH32CS_SNAPTHREAD = 0x00000004

class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ('dwSize', wintypes.DWORD),
        ('cntUsage', wintypes.DWORD),
        ('th32ThreadID', wintypes.DWORD),
        ('th32OwnerProcessID', wintypes.DWORD),
        ('tpBasePri', wintypes.LONG),
        ('tpDeltaPri', wintypes.LONG),
        ('dwFlags', wintypes.DWORD)
    ]

class FILETIME(ctypes.Structure):
    _fields_ = [('dwLowDateTime', wintypes.DWORD), ('dwHighDateTime', wintypes.DWORD)]

def get_thread_times(h_thread):
    cr = FILETIME()
    ex = FILETIME()
    k = FILETIME()
    u = FILETIME()
    if kernel32.GetThreadTimes(h_thread, ctypes.byref(cr), ctypes.byref(ex), ctypes.byref(k), ctypes.byref(u)):
        k_val = (k.dwHighDateTime << 32) | k.dwLowDateTime
        u_val = (u.dwHighDateTime << 32) | u.dwLowDateTime
        return k_val + u_val
    return 0

def get_thread_affinity(tid):
    h = kernel32.OpenThread(THREAD_QUERY_INFORMATION | THREAD_SET_INFORMATION, False, tid)
    if not h:
        return 0
    kernel32.SetThreadAffinityMask.restype = ctypes.c_size_t
    kernel32.SetThreadAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
    prev = kernel32.SetThreadAffinityMask(h, 0xFFFF)
    if prev != 0:
        kernel32.SetThreadAffinityMask(h, prev)
    kernel32.CloseHandle(h)
    return prev

def find_pid_by_name(name):
    # Find process
    import subprocess
    out = subprocess.check_output(f'powershell -NoProfile -Command "(Get-Process -Name {name} -ErrorAction SilentlyContinue).Id"', shell=True).decode().strip()
    if out:
        return int(out.split()[0])
    return None

def profile_process(pid, duration=3.0):
    print(f"Profiling Process PID {pid} over {duration} seconds...")

    # Pass 1
    h_snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(THREADENTRY32)

    t1 = {}
    priorities = {}
    if kernel32.Thread32First(h_snap, ctypes.byref(te)):
        while True:
            if te.th32OwnerProcessID == pid:
                tid = te.th32ThreadID
                priorities[tid] = te.tpBasePri
                h_th = kernel32.OpenThread(THREAD_QUERY_INFORMATION, False, tid)
                if h_th:
                    t_val = get_thread_times(h_th)
                    kernel32.CloseHandle(h_th)
                    t1[tid] = t_val
            if not kernel32.Thread32Next(h_snap, ctypes.byref(te)):
                break
    kernel32.CloseHandle(h_snap)

    time.sleep(duration)

    # Pass 2
    h_snap2 = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te2 = THREADENTRY32()
    te2.dwSize = ctypes.sizeof(THREADENTRY32)

    results = []
    if kernel32.Thread32First(h_snap2, ctypes.byref(te2)):
        while True:
            if te2.th32OwnerProcessID == pid:
                tid = te2.th32ThreadID
                if tid in t1:
                    h_th = kernel32.OpenThread(THREAD_QUERY_INFORMATION, False, tid)
                    if h_th:
                        t_val = get_thread_times(h_th)
                        kernel32.CloseHandle(h_th)
                        delta_units = t_val - t1[tid]
                        # 1 unit = 100 ns -> 10,000 units = 1 ms
                        delta_ms = delta_units / 10000.0
                        pct = (delta_ms / (duration * 1000.0)) * 100.0
                        mask = get_thread_affinity(tid)
                        
                        mask_str = "Unknown"
                        if mask == 0x0FFF:
                            mask_str = "P-Cores Only (0-11)"
                        elif mask == 0xF000:
                            mask_str = "E-Cores Only (12-15)"
                        elif mask == 0xFFFF:
                            mask_str = "All 16 Cores (P+E)"
                        else:
                            mask_str = f"0x{mask:04X}"
                            
                        results.append({
                            'tid': tid,
                            'priority': priorities.get(tid, te2.tpBasePri),
                            'delta_ms': delta_ms,
                            'cpu_pct': pct,
                            'mask': mask,
                            'mask_str': mask_str
                        })
            if not kernel32.Thread32Next(h_snap2, ctypes.byref(te2)):
                break
    kernel32.CloseHandle(h_snap2)

    results.sort(key=lambda x: x['delta_ms'], reverse=True)
    return results

if __name__ == '__main__':
    pid = find_pid_by_name('forzahorizon6')
    if not pid:
        print("forzahorizon6 is not running!")
    else:
        results = profile_process(pid, 3.0)
        total_cpu_ms = sum(r['delta_ms'] for r in results)
        total_pct = sum(r['cpu_pct'] for r in results)
        
        print(f"\nTotal Threads Profiled: {len(results)}")
        print(f"Total Process CPU Work in 3s: {total_cpu_ms:.1f} ms ({total_pct:.1f}% across all cores)")
        print(f"Normalized CPU Usage (16 Cores): {total_pct / 16.0:.1f}%\n")

        print(f"{'TID':<8} {'Priority':<10} {'CPU (ms/3s)':<14} {'CPU %':<10} {'Affinity Mask':<22}")
        print("-" * 68)
        for r in results[:20]:
            print(f"{r['tid']:<8} {r['priority']:<10} {r['delta_ms']:<14.1f} {r['cpu_pct']:<10.1f} {r['mask_str']:<22}")
