#!/usr/bin/env python3
"""Watches the MD 'ready' window and, as soon as it appears, sends messages over
the CCB UL ring and checks whether the state mirror moves before the exception.

Usage: sudo python3 ccb_window_test.py [n_messages] [gap_ms]
"""
import fcntl, mmap, os, struct, subprocess, sys, time

RAW = "/dev/ccci_raw_mdm"
COUNT = sys.argv[1] if len(sys.argv) > 1 else "3"
GAP = sys.argv[2] if len(sys.argv) > 2 else "0"
MODE = sys.argv[3] if len(sys.argv) > 3 else "ccb"   # ccb | kick

def u32(fd, req):
    b = bytearray(4); fcntl.ioctl(fd, req, b, True); return struct.unpack("<I", b)[0]

fd = os.open(RAW, os.O_RDONLY | os.O_NONBLOCK)
ln = u32(fd, 0x80044331)
m = mmap.mmap(fd, ln, mmap.MAP_SHARED, mmap.PROT_READ)
def w(off): return struct.unpack_from("<I", m, off)[0]
def st():
    return subprocess.run(["getprop", "vendor.mtk.md1.status"],
                          capture_output=True, text=True).stdout.strip()

OFFS = (0x40000, 0x40304, 0x40308, 0x40310, 0x40314, 0x4031C, 0x40340, 0x40348, 0x4036C)
NAMES = "tag CAP inflight beat reason flags msgs cert lim".split()
def row(): return tuple(w(o) for o in OFFS)
def show(t, s, r):
    print("%6.2f %-9s %s" % (t, s, " ".join("%s=%08x" % (n, v) for n, v in zip(NAMES, r))))

# CCB UL indices (to see whether the modem drains)
def ul_idx():
    c = os.open("/dev/ccci_ccb_ctrl", os.O_RDONLY)
    cl = u32(c, 0x8004433E)
    cm = mmap.mmap(c, cl, mmap.MAP_SHARED, mmap.PROT_READ)
    v = struct.unpack_from("<16I", cm, 19 * 64)
    cm.close(); os.close(c)
    return v[9:13]  # alloc, free, read, write

print("before     : ul(idx alloc/free/read/write)=%s md1=%s" % (ul_idx(), st()))
show(0.0, st(), row())

c = os.open("/dev/ccci_ioctl0", os.O_RDWR); fcntl.ioctl(c, 0x430B, 0); os.close(c)
t0 = time.time(); last_kick = 0.0; fired = False; prev = row(); last = None
while time.time() - t0 < 60:
    t = time.time() - t0
    s = st()
    if s != last:
        show(t, s, row()); last = s
    if s == "ready" and not fired:
        fired = True
        t_send = time.time()
        if MODE == "kick":
            cmd = ["./send_ccb", "--text", "AT+CGMI", "--count", COUNT, "--gap-ms", GAP,
                   "--wait-ms", "200"]
        else:
            cmd = ["./send_ccb", "--send", "41542b43474d490d", "--count", COUNT,
                   "--gap-ms", GAP, "--wait-ms", "200"]
        r = subprocess.run(cmd, capture_output=True, text=True, cwd="/tmp/ccb")
        print("---- %s at t=%.2f s (%.0f ms) ----" % (MODE, t, (time.time() - t_send) * 1000))
        for line in (r.stdout or "").splitlines():
            print("   " + line)
        print("   rc=%d" % r.returncode)
        prev = row()
    if fired:
        r = row()
        if r != prev:
            show(time.time() - t0, st(), r)
            prev = r
    if s == "exception":
        for _ in range(10):
            time.sleep(0.3)
            show(time.time() - t0, st(), row())
        break
    if s == "bootup" and t - last_kick > 5.0:
        subprocess.run(["setprop", "vendor.ril.mtk.restart", "1"])
        print("%6.2f -- rild restart" % t); last_kick = t
    time.sleep(0.03)
print("after      : ul(idx alloc/free/read/write)=%s md1=%s" % (ul_idx(), st()))
show(time.time() - t0, st(), row())
m.close(); os.close(fd)
