import psutil
import time
import ctypes
from ctypes import wintypes

pid = 26000
try:
    p = psutil.Process(pid)
except Exception as e:
    print(f"Error opening PID {pid}: {e}")
    exit(1)

print(f"Profiling PID {pid} ({p.name()}) for 3.0 seconds...")

class THREAD_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [
        ('ExitStatus', wintypes.DWORD),
        ('TebBaseAddress', ctypes.c_void_p),
        ('ClientId_UniqueProcess', ctypes.c_void_p),
        ('ClientId_UniqueThread', ctypes.c_void_p),
        ('AffinityMask', ctypes.c_size_t),
        ('Priority', ctypes.c_long),
        ('BasePriority', ctypes.c_long),
    ]

OpenThread = ctypes.windll.kernel32.OpenThread
CloseHandle = ctypes.windll.kernel32.CloseHandle
NtQueryInformationThread = ctypes.windll.ntdll.NtQueryInformationThread

def get_thread_info(tid):
    h = OpenThread(0x0040, False, tid)
    if not h:
        return 0, 0, 0
    tbi = THREAD_BASIC_INFORMATION()
    ret = NtQueryInformationThread(h, 0, ctypes.byref(tbi), ctypes.sizeof(tbi), None)
    CloseHandle(h)
    if ret == 0:
        return tbi.BasePriority, tbi.Priority, tbi.AffinityMask
    return 0, 0, 0

t0 = time.perf_counter()
initial_threads = {t.id: (t.user_time, t.system_time) for t in p.threads()}
time.sleep(3.0)
final_threads = {t.id: (t.user_time, t.system_time) for t in p.threads()}
t1 = time.perf_counter()
wall_time = t1 - t0

thread_stats = []
for tid, (u1, s1) in final_threads.items():
    if tid in initial_threads:
        u0, s0 = initial_threads[tid]
        du = u1 - u0
        ds = s1 - s0
        total = du + ds
        pct = (total / wall_time) * 100.0
        u_pct = (du / wall_time) * 100.0
        s_pct = (ds / wall_time) * 100.0
        base_pri, pri, aff = get_thread_info(tid)
        thread_stats.append((tid, pct, u_pct, s_pct, base_pri, pri, aff))

thread_stats.sort(key=lambda x: x[1], reverse=True)

print("\n" + "=" * 90)
print(f"{'TID':>6} | {'Total%':>7} | {'User%':>6} | {'Sys%':>6} | {'Pri':>3} | {'Base':>4} | {'Affinity':>8} | Role / Classification")
print("=" * 90)

for tid, pct, up, sp, base_pri, pri, aff in thread_stats[:25]:
    role = "Unknown"
    if tid == 32816: role = ">>> RENDER THREAD (D3D12 Submission) <<<"
    elif tid == 3584: role = ">>> MAIN ENGINE THREAD (Game Loop) <<<"
    elif tid == 34204: role = "AUDIO MIXER / RAYTRACING (Time Critical)"
    elif tid == 13272: role = "PHYSICS SIMULATION (Vehicle Dynamics)"
    elif pct >= 35.0: role = f"TaskGraph Worker Pool"
    elif pct >= 15.0: role = "Streaming / Oodle Decompression"
    elif pct >= 5.0:  role = "Driver / DX12 Worker"
    else: role = "Background / Idle"
    print(f"{tid:6d} | {pct:6.2f}% | {up:5.2f}% | {sp:5.2f}% | {pri:3d} | {base_pri:4d} | 0x{aff:04X} | {role}")
print("=" * 90)
