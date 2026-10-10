"""SYNTHETIC test fixture for the dashboard UI test (tests/ui_smoke.mjs). Not measured data:
it only exercises every message type and view. Real demo sessions are recorded on the bench
(Record button). Writes tests/fixture-session.json.
    python supervisor/web/tests/make_fixture.py
"""
import base64
import json
import struct
from pathlib import Path

FCPU = 80_000_000
MS = FCPU // 1000
OUT = Path(__file__).with_name("fixture-session.json")

TASKS = [  # tag, name, base prio, stack words, flags
    (1, "acq", 22, 256, 0), (2, "taskA", 20, 256, 0), (3, "taskB", 16, 256, 0),
    (4, "link", 10, 256, 0), (5, "labCtl", 18, 384, 0), (6, "IDLE", 0, 128, 1),
]
msgs = []


def add(ms, obj):
    msgs.append([ms, "j", obj])


def trace(ms, events):
    blob = b"T" + struct.pack("<HH", 0, 0) + b"".join(struct.pack("<IB", c & 0xFFFFFFFF, i) for c, i in events)
    msgs.append([ms, "b", base64.b64encode(blob).decode()])


def schedule(t0_ms, n_ms):
    """A simple fixed-priority schedule: tick every ms, ADC irq + acq every ms, taskA every 10 ms, taskB every 25 ms."""
    ev = []
    for k in range(t0_ms, t0_ms + n_ms):
        base = k * MS
        ev.append((base, 0xFF))                          # tick
        ev.append((base + 400, 0x41)); ev.append((base + 1600, 0x81))     # ADC DMA irq
        ev.append((base + 1700, 0xC1)); ev.append((base + 2000, 1))       # acq released, runs
        t = base + 2000 + 24000
        if k % 10 == 0:
            ev.append((base + 100, 0xC2)); ev.append((t, 2)); t += 160_000 // 2
            if k % 25 == 0:
                ev.append((base + 100, 0xC3))
            ev.append((t, 6))
        elif k % 25 == 0:
            ev.append((base + 100, 0xC3)); ev.append((t, 3)); ev.append((t + 30000, 6))
        else:
            ev.append((t, 6))                            # idle
    return sorted(ev)


for sec in range(4):
    t = sec * 1000
    for tag, name, prio, stack, flags in TASKS:
        add(t + 5, {"type": "task", "tag": tag, "prio": prio, "stack": stack, "deadline": 0, "flags": flags, "fcpu": FCPU, "name": name})
    add(t + 10, {"type": "rsys", "fcpu": FCPU, "sw": 3100, "ev": 6100, "drop": 0, "idle": 530, "tick": 1000,
                 "ctx": 950, "isr": 1880, "probe": 9, "kernel": 21, "heapTotal": 3000, "heapFree": 1900, "heapMin": 700 if sec >= 2 else 1800})
    add(t + 12, {"type": "sys", "link": 1, "rx": 1000 * (sec + 1), "crc": 0, "lost": 0, "rssi": -45, "ip": "192.168.137.143", "clients": 1, "up": t, "heap": 160000})
    stats = {1: (30, 1000, 22800, 21000, 24000, 23500, MS, MS - 800, MS + 900, 0, 175),
             2: (195, 100, 156000, 155000, 162000, 157000, 10 * MS, 10 * MS - 900, 10 * MS + 800, 0, 219),
             3: (195, 40, 390000, 388000, 566000, 391000, 25 * MS, 25 * MS - 1000, 25 * MS + 1100, 3 if sec >= 2 else 0, 229),
             4: (36, 100, 57000, 40000, 970000, 60000, 10 * MS, 9 * MS, 12 * MS, 2, 100),
             5: (0 if sec < 2 else 150, 0, 12_000_000 if sec >= 2 else 0, 0, 12_100_000 if sec >= 2 else 0, 0, 0, 0, 0, 0, 90)}
    for tag, (cpu, jobs, cmax, cavg, rmax, rmin, tavg, tmin, tmax, miss, stk) in stats.items():
        add(t + 20 + tag, {"type": "tstat", "tag": tag, "state": 2, "prio": 21 if (tag == 5 and sec == 2) else dict((x[0], x[2]) for x in TASKS)[tag],
                           "cpu": cpu, "jobs": jobs, "cmax": cmax, "cavg": cavg, "rmax": rmax, "rmin": rmin,
                           "tavg": tavg, "tmin": tmin, "tmax": tmax, "miss": miss, "stack": stk})
    add(t + 30, {"type": "tstat", "tag": 6, "state": 1, "prio": 0, "cpu": 530, "jobs": 0, "cmax": 0, "cavg": 0, "rmax": 0, "rmin": 0,
                 "tavg": 0, "tmin": 0, "tmax": 0, "miss": 0, "stack": 107})
    add(t + 40, {"type": "qstat", "qid": 1, "len": 8, "wait": 8 if sec >= 2 else 0, "max": 8 if sec >= 2 else 1, "fail": 32 if sec >= 2 else 0, "name": "lab_queue"})
    for b in range(10):
        ms = t + b * 100
        add(ms + 1, {"type": "tel", "mode": "TEST", "fault": 0, "s": [[ms + i * 10, 1500.0, 1499.0 + (i % 3), 50.0] for i in range(10)]})
        trace(ms + 2, schedule(ms, 100))

add(2100, {"type": "evt", "t": 2100, "code": 11, "name": "FAULT_INJECTED", "arg": 1})
add(2300, {"type": "evt", "t": 2300, "code": 13, "name": "CPU_HOG", "arg": 5 | (150 << 8)})
add(2400, {"type": "evt", "t": 2400, "code": 7, "name": "DEADLINE_MISS", "arg": 3 | (3 << 8)})
add(2500, {"type": "evt", "t": 2500, "code": 8, "name": "PRIO_INHERIT", "arg": 4 | (21 << 8) | (2 << 16)})
add(2510, {"type": "evt", "t": 2510, "code": 9, "name": "PRIO_RESTORE", "arg": 4 | (6050 << 8)})
add(2600, {"type": "evt", "t": 2600, "code": 10, "name": "QUEUE_FULL", "arg": 1 | (32 << 8)})
add(2700, {"type": "evt", "t": 2700, "code": 12, "name": "STACK_LOW", "arg": 5 | (90 << 8)})

msgs.sort(key=lambda m: m[0])
OUT.write_text(json.dumps({"format": "rtos-doctor-session", "version": 1, "synthetic": True,
                           "recordedAt": "synthetic test fixture", "fcpu": FCPU, "messages": msgs}))
print(f"{len(msgs)} messages -> {OUT.name}")
