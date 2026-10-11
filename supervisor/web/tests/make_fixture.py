"""SYNTHETIC session for the dashboard tests and the README screenshots. Not measured data.

A small fixed-priority preemptive scheduler simulates the task set of the bench firmware
(names, priorities, periods and declared deadlines from firmware/Core/Src/main.c and faultlab.c;
execution times are rough assumptions, not measurements), then produces the messages the
gateway would send: task table, per-task statistics, system statistics, motor telemetry,
detector events and the binary context-switch trace. The session is flagged "synthetic":
the dashboard shows "simulated data" while it is replayed.
At 6.0 s the fault lab's CPU hog is injected (labCtl runs 150 ms without blocking), so the
lower-priority tasks are late for a moment. Writes tests/fixture-session.json.
    python supervisor/web/tests/make_fixture.py
"""
import base64
import heapq
import json
import random
import struct
from pathlib import Path

FCPU = 80_000_000
CPU = FCPU // 1_000_000                 # cycles per microsecond
END_US = 9_000_000
FAULT_US = 6_000_000
OUT = Path(__file__).with_name("fixture-session.json")
rnd = random.Random(7)

# tag: name, priority, stack words, never-used words, period (us) or None, declared deadline (ms), execution time (us)
TASKS = {
    1: ("healthTask", 8, 256, 180, 1_000_000, 1000, lambda: 45),
    2: ("taskA", 20, 256, 201, 10_000, 10, lambda: 1950 + rnd.randint(-10, 10)),
    3: ("taskB", 16, 256, 199, 25_000, 25, lambda: 4900 + rnd.randint(-20, 20)),
    4: ("ctxRx", 24, 256, 214, None, 0, lambda: 4),
    5: ("ctxTx", 21, 256, 207, 5_000, 5, lambda: 8),
    6: ("acq", 22, 256, 148, None, 1, lambda: 95 + rnd.randint(-15, 15)),
    7: ("link", 17, 384, 121, 10_000, 10, lambda: 420 + rnd.randint(-120, 120)),
    8: ("labCtl", 18, 384, 290, None, 0, lambda: 150_000),
    9: ("labLow", 9, 192, 160, None, 0, lambda: 0),
    10: ("labHigh", 21, 192, 165, None, 0, lambda: 0),
    11: ("labRx", 9, 192, 162, None, 0, lambda: 0),
    12: ("IDLE", 0, 128, 101, None, 0, None),
    13: ("Tmr Svc", 2, 256, 230, None, 0, None),
}
IDLE = 12


class Stat:
    def __init__(self):
        self.releases = 0; self.last_rel = None; self.intervals = 0
        self.tavg = 0; self.tmin = None; self.tmax = 0
        self.cmax = 0; self.csum = 0; self.jobs = 0; self.rmax = 0; self.rmin = None; self.miss = 0
        self.run = [0] * (END_US // 1_000_000 + 1)
        self.jobs_s = [0] * len(self.run); self.cmax_s = [0] * len(self.run); self.rmax_s = [0] * len(self.run)
        self.miss_s = [0] * len(self.run)
        self.csum_s = [0] * len(self.run); self.rmin_s = [None] * len(self.run)
        self.tsnap = [None] * len(self.run)          # (tavg, tmin, tmax) as they stood at the end of each second


stats = {tag: Stat() for tag in TASKS}
trace = []                    # (us, id); converted to cycles when written
events = []                   # (us, code, arg)
switches = [0] * len(stats[1].run)
heap = []                     # (time us, order, kind, data)
order = 0


def push(t, kind, data=None):
    global order
    order += 1
    heapq.heappush(heap, (t, order, kind, data))


def add_run(tag, a, b):
    """CPU time of [a, b) split into one-second windows."""
    s = stats[tag]
    while a < b:
        sec = a // 1_000_000
        end = min(b, (sec + 1) * 1_000_000)
        s.run[sec] += end - a
        a = end


# Periodic releases, ticks, the ADC DMA interrupt every millisecond, the fault.
for tag, (_, _, _, _, period, _, _) in TASKS.items():
    if period:
        push(0, "release", tag)
for ms in range(END_US // 1000):
    push(ms * 1000, "tick")
    push(ms * 1000 + 300, "isr_in")
push(FAULT_US, "fault")

jobs = {}                     # tag -> open job {rel, left, c}
ready_order = []              # tags in order of readiness (FIFO among equal priorities)
running = None
isr_until = None
t = 0


def release(tag, now):
    s = stats[tag]
    period = TASKS[tag][4]
    if period:
        push(now + period, "release", tag)
    if tag in jobs:           # late: vTaskDelayUntil does not block, the jobs merge (no release event)
        jobs[tag]["left"] += TASKS[tag][6]()
        return
    if s.last_rel is not None:
        dt = now - s.last_rel
        s.tavg = dt if s.intervals == 0 else s.tavg + int((dt - s.tavg) / 16)
        s.tmin = dt if s.tmin is None else min(s.tmin, dt)
        s.tmax = max(s.tmax, dt)
        s.intervals += 1
        s.tsnap[min(now // 1_000_000, len(s.run) - 1)] = (s.tavg, s.tmin, s.tmax)
    s.releases += 1
    s.last_rel = now
    jobs[tag] = {"rel": now, "left": TASKS[tag][6](), "c": 0, "started": False}
    ready_order.append(tag)
    trace.append((now, 0xC0 | tag))


def finish(tag, now):
    s, j = stats[tag], jobs.pop(tag)
    ready_order.remove(tag)
    c, r = j["c"], now - j["rel"]
    sec = min(now // 1_000_000, len(s.run) - 1)
    s.jobs += 1; s.csum += c; s.cmax = max(s.cmax, c); s.rmax = max(s.rmax, r)
    s.rmin = r if s.rmin is None else min(s.rmin, r)
    s.jobs_s[sec] += 1; s.cmax_s[sec] = max(s.cmax_s[sec], c); s.rmax_s[sec] = max(s.rmax_s[sec], r)
    s.csum_s[sec] += c; s.rmin_s[sec] = r if s.rmin_s[sec] is None else min(s.rmin_s[sec], r)
    d = TASKS[tag][5] * 1000 or (s.tavg if s.intervals >= 4 else 0)
    if d and r > d:
        s.miss += 1; s.miss_s[sec] += 1
    if c >= 50_000:
        events.append((now, 13, tag | ((c // 1000) << 8)))     # CPU_HOG, reported when the job ends


def pick():
    best = None
    for tag in ready_order:
        if best is None or TASKS[tag][1] > TASKS[best][1]:
            best = tag
    return best if best is not None else IDLE


while t < END_US:
    nxt = heap[0][0] if heap else END_US
    if running != IDLE and running is not None and isr_until is None:
        nxt = min(nxt, t + jobs[running]["left"])
    nxt = min(nxt, END_US)
    if running is not None and isr_until is None:
        add_run(running, t, nxt)
        if running != IDLE:
            jobs[running]["left"] -= nxt - t
            jobs[running]["c"] += nxt - t
    t = nxt
    if running not in (None, IDLE) and isr_until is None and jobs[running]["left"] <= 0:
        finish(running, t)
    while heap and heap[0][0] <= t:
        _, _, kind, data = heapq.heappop(heap)
        if kind == "tick":
            trace.append((t, 0xFF))
        elif kind == "isr_in":
            trace.append((t, 0x41)); isr_until = t + 15; push(isr_until, "isr_out")
        elif kind == "isr_out":
            trace.append((t, 0x81)); isr_until = None
            release(6, t)                                   # the DMA interrupt wakes the acquisition task
        elif kind == "release":
            release(data, t)
        elif kind == "fault":
            events.append((t, 11, 1))                       # FAULT_INJECTED: CPU hog
            release(8, t)
    if isr_until is None:
        new = pick()
        if new != running:
            trace.append((t, new)); switches[min(t // 1_000_000, len(switches) - 1)] += 1
            running = new
            if new == 5 and not jobs[5]["started"]:         # ctxTx notifies ctxRx at the start of its job
                push(t + 3, "release", 4)
            if new in jobs:
                jobs[new]["started"] = True

# ---- Motor bench telemetry: TEST mode square wave, PI loop on a first-order motor ------------
tel = []
rpm, integ = 0.0, 0.0
for ms in range(END_US // 1000):
    sp = 1500.0 if (ms // 2000) % 2 == 0 else 0.0
    e = sp - rpm
    duty = 0.001 * e + integ
    if 0.0 < duty < 1.0:
        integ += 0.010 * e * 0.001                          # anti-windup: integrate only when not saturated
    duty = min(max(duty, 0.0), 1.0)
    rpm += (3000.0 * duty - rpm) / 0.100 * 0.001
    if ms % 10 == 0:
        tel.append([ms, sp, round(rpm, 1), round(duty * 100, 1)])

# ---- Messages, in the gateway's format -------------------------------------------------------
msgs = []


def add(ms, obj):
    msgs.append([ms, "j", obj])


add(0, {"type": "evt", "t": 0, "code": 4, "name": "MODE", "arg": 3})
for sec in range(1, END_US // 1_000_000 + 1):
    t0 = sec * 1000 + 5
    w = sec - 1
    for tag, (name, prio, stack, free, period, dl, _) in TASKS.items():
        add(t0, {"type": "task", "tag": tag, "prio": prio, "stack": stack, "deadline": dl,
                 "flags": 1 if tag == IDLE else 0, "fcpu": FCPU, "name": name})
    idle_pm = stats[IDLE].run[w] // 1000
    add(t0 + 1, {"type": "rsys", "fcpu": FCPU, "sw": switches[w], "ev": sum(1 for x in trace if w * 1e6 <= x[0] < sec * 1e6),
                 "drop": 0, "idle": idle_pm, "tick": 1000, "ctx": 523, "isr": 1310, "probe": 9, "kernel": 21,
                 "heapTotal": 3000, "heapFree": 3000, "heapMin": 3000})
    add(t0 + 2, {"type": "sys", "link": 1, "rx": 100 * sec, "crc": 0, "lost": 0, "rssi": -47, "ip": "192.168.137.143",
                 "clients": 1, "up": sec * 1000, "heap": 160000})
    for tag, (name, prio, stack, free, period, dl, _) in TASKS.items():
        s = stats[tag]
        state = 0 if tag == 7 else 1 if tag == IDLE else 2      # the link task reads the states: it is running
        jobs = sum(s.jobs_s[:sec]); cmax = max(s.cmax_s[:sec]); rmax = max(s.rmax_s[:sec])
        rmins = [x for x in s.rmin_s[:sec] if x is not None]
        snaps = [x for x in s.tsnap[:sec] if x is not None]
        tavg, tmin, tmax = snaps[-1] if snaps else (0, 0, 0)    # values since reset, as at the end of this second
        add(t0 + 3 + tag, {"type": "tstat", "tag": tag, "state": state, "prio": prio, "cpu": s.run[w] // 1000,
                           "jobs": s.jobs_s[w], "cmax": cmax * CPU, "cavg": (sum(s.csum_s[:sec]) // jobs if jobs else 0) * CPU,
                           "rmax": rmax * CPU, "rmin": (min(rmins) if rmins else 0) * CPU, "tavg": tavg * CPU,
                           "tmin": tmin * CPU, "tmax": tmax * CPU,
                           "miss": sum(s.miss_s[:sec]), "stack": free, "cmaxl": s.cmax_s[w] * CPU, "rmaxl": s.rmax_s[w] * CPU})
        if s.miss_s[w]:
            add(t0 + 20, {"type": "evt", "t": sec * 1000, "code": 7, "name": "DEADLINE_MISS", "arg": tag | (s.miss_s[w] << 8)})
    add(t0 + 30, {"type": "qstat", "qid": 1, "len": 8, "wait": 0, "max": 1, "fail": 0, "name": "lab_queue"})
for us, code, arg in events:
    add(us // 1000, {"type": "evt", "t": us // 1000, "code": code, "name": "", "arg": arg})
for k in range(0, END_US // 1000, 100):
    add(k + 100, {"type": "tel", "mode": "TEST", "fault": 0, "s": [x for x in tel if k <= x[0] < k + 100]})

trace.sort(key=lambda x: x[0])
batch, start = [], 0
for us, i in trace:                                         # one binary trace message every 100 ms
    while us >= start + 100_000:
        if batch:
            blob = b"T" + struct.pack("<HH", 0, 0) + b"".join(struct.pack("<IB", (u * CPU) & 0xFFFFFFFF, x) for u, x in batch)
            msgs.append([start // 1000 + 100, "b", base64.b64encode(blob).decode()])
        batch, start = [], start + 100_000
    batch.append((us, i))

msgs.sort(key=lambda m: m[0])
OUT.write_text(json.dumps({"format": "rtos-doctor-session", "version": 1, "synthetic": True,
                           "recordedAt": "synthetic: simulated task set, not measured", "fcpu": FCPU, "messages": msgs}))
print(f"{len(msgs)} messages, {len(trace)} trace events -> {OUT.name}")
for tag, s in stats.items():
    print(f"  {TASKS[tag][0]:10s} cpu {sum(s.run) / END_US * 100:5.1f} %  jobs {s.jobs:5d}  Cmax {s.cmax:6d} us  Rmax {s.rmax:6d} us  misses {s.miss}")
