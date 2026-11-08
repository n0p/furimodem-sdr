#!/usr/bin/env python3
"""One MD boot with fine sampling of the state mirror: does any marker appear
between 'ready' and the exception?  Usage: sudo python3 watch_md_boot.py [seconds]
"""
import fcntl, mmap, os, struct, subprocess, sys, time

RAW = "/dev/ccci_raw_mdm"
TOTAL = float(sys.argv[1]) if len(sys.argv) > 1 else 45.0

def u32(fd, req):
    b = bytearray(4); fcntl.ioctl(fd, req, b, True); return struct.unpack("<I", b)[0]

fd = os.open(RAW, os.O_RDONLY | os.O_NONBLOCK)
ln = u32(fd, 0x80044331)
m = mmap.mmap(fd, ln, mmap.MAP_SHARED, mmap.PROT_READ)
def w(off): return struct.unpack_from("<I", m, off)[0]

def st():
    return subprocess.run(["getprop", "vendor.mtk.md1.status"],
                          capture_output=True, text=True).stdout.strip()

OFFS = (0x40000, 0x40304, 0x40308, 0x40310, 0x40314, 0x4031C,
        0x40340, 0x40348, 0x4036C, 0x21000)
NAMES = "tag CAP inflight beat reason flags msgs cert lim canary".split()

def row():
    return tuple(w(o) for o in OFFS)

print("%6s %-9s %s" % ("t", "state", " ".join("%-8s" % n for n in NAMES)))
c = os.open("/dev/ccci_ioctl0", os.O_RDWR)
try:
    fcntl.ioctl(c, 0x430B, 0)
    print("%6.1f -- ioctl SEND_START_MD_REQUEST" % 0.0)
except OSError as e:
    print("start md error:", e)
os.close(c)

t0 = time.time(); prev = None; last = None; last_kick = 0.0
seen_nonzero = []
while time.time() - t0 < TOTAL:
    t = time.time() - t0
    s = st(); r = row()
    if r != prev or s != last or abs(t % 1.0) < 0.06:
        print("%6.1f %-9s %s" % (t, s, " ".join("%08x" % v for v in r)))
    if r != prev and any(r[1:]):
        seen_nonzero.append((round(t, 2), s, r))
    prev, last = r, s
    # the boot handshake needs the RIL: retry every 6 s while in bootup
    if s == "bootup" and t - last_kick > 6.0:
        subprocess.run(["setprop", "vendor.ril.mtk.restart", "1"])
        print("%6.1f -- setprop vendor.ril.mtk.restart 1" % t)
        last_kick = t
    if s == "exception":
        for _ in range(6):          # keep reading after the exception
            time.sleep(0.3)
            r2 = row()
            print("%6.1f %-9s %s" % (time.time() - t0, st(), " ".join("%08x" % v for v in r2)))
        break
    time.sleep(0.05)
print("non-zero markers seen:", seen_nonzero if seen_nonzero else "NONE")
m.close(); os.close(fd)
