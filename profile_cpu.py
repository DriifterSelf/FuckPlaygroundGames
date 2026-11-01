import psutil
import time
import ctypes
from ctypes import wintypes

# Find PID of forzahorizon6
pids = [p.pid for p in psutil.process_iter(['name']) if p.info['name'] and 'forzahorizon6' in p.info['name'].lower()]
if not pids:
    print("forzahorizon6.exe not running")
    exit(1)

pid = pids[0]
p = psutil.Process(pid)
print(f"Profiling PID {pid} ({p.name()}) for 2.0 seconds...")

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
time.sleep(2.0)
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
print(f"{'TID':>6} | {'Total%':>7} | {'User%':>6} | {'Sys%':>6} | {'Pri':>3} | {'Base':>4} | {'Affinity':>8} | State / Activity")
print("=" * 90)

for tid, pct, up, sp, base_pri, pri, aff in thread_stats[:20]:
    print(f"{tid:6d} | {pct:6.2f}% | {up:5.2f}% | {sp:5.2f}% | {pri:3d} | {base_pri:4d} | 0x{aff:04X}")
print("=" * 90)
