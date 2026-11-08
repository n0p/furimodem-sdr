#!/usr/bin/env python3
"""Generate IQ block files with the packed formats used by the scanner.

The file is a sequence of blocks exactly as the modem ring publishes them:
a 0x80 B header and a payload of 16-bit words, one sample per word.

5-bit format (signature "CSD1"):

    I = bits 3..7  (minus 16)      Q = bits 11..15 (minus 16)

8-bit format (signature "CSD8"):

    I = low byte   (minus 128)     Q = high byte   (minus 128)

Every capture takes two consecutive blocks (4,064 + 1,888 words = 11,904
samples), just like on the phone. The burst frequency goes in header field
p3 (high 16 bits, units of 100 kHz).

Test signal: a complex tone offset fs/8 from the centre (240 kHz with
fs = 1.92 MS/s), which falls exactly on bin N/2 + N/8 for any power-of-two
FFT, plus quantisation noise.

Usage:
    python3 tools/make-iqfile.py --out tools/testdata/iq5-tone.bin
    python3 tools/make-iqfile.py --out tools/testdata/iq8-tone.bin --format 8
    python3 tools/make-iqfile.py --out /tmp/legacy.bin --format cs16
    # old dump: CSD1 signature with an 8-bit payload (compatibility only)
    python3 tools/make-iqfile.py --out /tmp/old.bin --format 8 --legacy-signature
"""

from __future__ import annotations

import argparse
import math
import random
import struct

FS = 1.92e6            # scanner window rate
SAMPLES = 11904        # samples per capture (6.2 ms at 1.92 MS/s)
PARTS = (4064, 1888)   # split into the two blocks, in 32-bit words
HDR = 0x80
MAGIC5 = 0x31445343    # "CSD1": 5-bit I/Q
MAGIC8 = 0x38445343    # "CSD8": 8-bit I/Q
FREQ_UNIT_HZ = 100_000  # p3 high 16 x 100 kHz

# Header offsets (u32 little-endian).
H = {
    "sig": 0x00, "seq": 0x04, "ucnt": 0x08, "bytes": 0x0C, "state": 0x10,
    "blk": 0x14, "push": 0x18, "slot": 0x1C, "start": 0x20, "end": 0x24,
    "cfg0": 0x28, "cfgm": 0x2C, "cfg14": 0x30, "cfg16": 0x34, "cfg20": 0x38,
    "cfg24": 0x3C, "cfg28": 0x40, "pllk": 0x44, "pllb": 0x48, "cfgptr": 0x4C,
    "pop": 0x50, "nz": 0x54, "cpuc": 0x58, "bm": 0x5C, "ent0": 0x60,
    "p2": 0x64, "p3": 0x68, "st": 0x6C,
}

# Codes per format: real range of each component.
CODES = {"5": (-16, 15), "8": (-128, 127)}
DEFAULT_AMP = {"5": 8.0, "8": 64.0}
DEFAULT_NOISE = {"5": 1.1, "8": 8.0}


def make_header(seq: int, nbytes: int, blk: int, push: int, slot: int,
                freq_hz: int, next_hz: int, total_samples: int, magic: int,
                band: int = 0) -> bytes:
    h = bytearray(HDR)
    fields = {
        "sig": magic, "seq": seq, "ucnt": 0, "bytes": nbytes, "state": 0,
        "blk": blk, "push": push, "slot": slot, "start": 0x03000180,
        "end": 0x03000180 + nbytes, "cfg0": 0, "cfgm": 0, "cfg14": 0,
        "cfg16": 0, "cfg20": 0, "cfg24": 0, "cfg28": 0,
        "pllk": freq_hz // 1000, "pllb": band, "cfgptr": 0,
        "pop": push - 1, "nz": total_samples, "cpuc": 0, "bm": 0, "ent0": 0,
        "p2": (next_hz // FREQ_UNIT_HZ) << 16,
        # p3: frequency in the high 16 bits (100 kHz) and band in the low byte, like the real header.
        "p3": ((freq_hz // FREQ_UNIT_HZ) << 16) | (band & 0xFF),
        "st": 0,
    }
    for k, v in fields.items():
        struct.pack_into("<I", h, H[k], v & 0xFFFFFFFF)
    return bytes(h)


def pack5(i_codes: list[int], q_codes: list[int]) -> bytes:
    out = bytearray(2 * len(i_codes))
    for n, (i, q) in enumerate(zip(i_codes, q_codes)):
        w = (((i + 16) & 0x1F) << 3) | (((q + 16) & 0x1F) << 11)
        struct.pack_into("<H", out, 2 * n, w)
    return bytes(out)


def pack8(i_codes: list[int], q_codes: list[int]) -> bytes:
    """I in the low byte, Q in the high byte, binary offset 128."""
    out = bytearray(2 * len(i_codes))
    for n, (i, q) in enumerate(zip(i_codes, q_codes)):
        out[2 * n] = (i + 128) & 0xFF
        out[2 * n + 1] = (q + 128) & 0xFF
    return bytes(out)


def capture_samples(n: int, offset_hz: float, amp_codes: float, noise_codes: float,
                    rng: random.Random, lo: int, hi: int) -> tuple[list[int], list[int]]:
    """Complex tone at offset_hz + white noise, quantised to the format codes."""
    w = 2.0 * math.pi * offset_hz / FS
    ph0 = rng.random() * 2.0 * math.pi
    i_c, q_c = [], []
    for k in range(n):
        ph = ph0 + w * k
        i = amp_codes * math.cos(ph) + rng.gauss(0.0, noise_codes)
        q = amp_codes * math.sin(ph) + rng.gauss(0.0, noise_codes)
        i_c.append(max(lo, min(hi, int(round(i)))))
        q_c.append(max(lo, min(hi, int(round(q)))))
    return i_c, q_c


def write_packed(path: str, freq_hz: int, offset_hz: float, amp: float, noise: float,
                 captures: int, seed: int, fmt: str, magic: int, band: int) -> None:
    rng = random.Random(seed)
    lo, hi = CODES[fmt]
    pack = pack8 if fmt == "8" else pack5
    push = 100
    seq = 1
    with open(path, "wb") as f:
        for c in range(captures):
            i_c, q_c = capture_samples(SAMPLES, offset_hz, amp, noise, rng, lo, hi)
            raw = pack(i_c, q_c)
            pos = 0
            for blk, words in enumerate(PARTS):
                nbytes = 4 * words  # words are 32-bit words of the ring
                chunk = raw[pos:pos + nbytes]
                pos += nbytes
                f.write(make_header(seq, nbytes, blk, push + c, c % 14,
                                    freq_hz, freq_hz, SAMPLES, magic, band))
                f.write(chunk)
                seq += 1


def write_legacy(path: str, freq_hz: int, offset_hz: float, amp: float,
                 noise: float, captures: int, seed: int) -> None:
    """Interleaved CS16 dump of the old format, for the regression test."""
    rng = random.Random(seed)
    with open(path, "wb") as f:
        for _ in range(captures):
            i_c, q_c = capture_samples(SAMPLES, offset_hz, amp * 32767.0 / 16.0,
                                       noise * 32767.0 / 16.0, rng, -32768, 32767)
            for i, q in zip(i_c, q_c):
                f.write(struct.pack("<hh", i, q))


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate 5- or 8-bit IQ blocks (CSD1/CSD8)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--format", choices=("5", "8", "cs16"), default="5",
                    help="5 = CSD1 (default), 8 = CSD8, cs16 = old dump")
    ap.add_argument("--legacy", action="store_true", help="alias of --format cs16")
    ap.add_argument("--legacy-signature", action="store_true",
                    help="with --format 8, write the CSD1 signature (ambiguous old dump)")
    ap.add_argument("--freq-mhz", type=float, default=2115.0,
                    help="burst frequency in the header (MHz)")
    ap.add_argument("--offset-khz", type=float, default=None,
                    help="tone offset from the centre; by default fs/8 = 240 kHz")
    ap.add_argument("--amp", type=float, default=None,
                    help="tone amplitude in codes (8 of 16 by default; 64 of 128 in 8-bit)")
    ap.add_argument("--noise", type=float, default=None,
                    help="noise deviation in codes (1.1 in 5-bit; 8 in 8-bit)")
    ap.add_argument("--captures", type=int, default=10)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--band", type=int, default=None,
                    help="band written in the low byte of p3 (1 in 8-bit by default; 0 in 5-bit)")
    a = ap.parse_args()

    fmt = "cs16" if a.legacy else a.format
    offset = FS / 8.0 if a.offset_khz is None else a.offset_khz * 1e3
    freq = int(round(a.freq_mhz * 1e6 / FREQ_UNIT_HZ)) * FREQ_UNIT_HZ
    if freq <= 0:
        ap.error("--freq-mhz must be positive")
    if fmt == "cs16":
        if a.legacy_signature:
            ap.error("--legacy-signature only applies with --format 8")
        amp = 8.0 if a.amp is None else a.amp
        noise = 1.1 if a.noise is None else a.noise
        write_legacy(a.out, freq, offset, amp, noise, a.captures, a.seed)
        print(f"{a.out}: {a.captures} captures, CS16, tone {offset/1e3:+.1f} kHz")
        return 0

    amp = DEFAULT_AMP[fmt] if a.amp is None else a.amp
    noise = DEFAULT_NOISE[fmt] if a.noise is None else a.noise
    if freq % FREQ_UNIT_HZ:
        print(f"warning: {a.freq_mhz} MHz is not a multiple of 100 kHz; storing {freq/1e6} MHz")
    magic = MAGIC5 if (fmt == "5" or a.legacy_signature) else MAGIC8
    band = (1 if fmt == "8" else 0) if a.band is None else a.band
    write_packed(a.out, freq, offset, amp, noise, a.captures, a.seed, fmt, magic, band)
    signature = "CSD1" if magic == MAGIC5 else "CSD8"
    note = " (CSD1 signature, old dump)" if fmt == "8" and magic == MAGIC5 else ""
    print(f"{a.out}: {a.captures} captures, {freq/1e6:.3f} MHz, "
          f"tone {offset/1e3:+.1f} kHz, {signature} {fmt} bits{note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
