# IQ ring sample format

How the data the modem publishes is packed, how the driver unpacks it and how it is checked locally,
without a phone. It is the contract shared by the publisher (the modem image), the Soapy driver and
the app.

## 1. The block

The ring is **16 slots of 16 KB**. Each block starts with a **128 B** header in little-endian and
continues with `bytes` bytes of samples. The signature in the first 32-bit integer gives the format:

| signature | integer | contents | bytes/sample |
|---|---|---|---|
| `CSD1` | `0x31445343` | 5-bit I/Q, one sample per 16-bit word | 2 |
| `CSD8` | `0x38445343` | 8-bit I/Q, one sample per 16-bit word | 2 |
| `RF30` | `0x30334652` | interleaved CS16 (legacy format) | 4 |

Header fields used (byte offsets):

```
+0x00 signature  +0x04 seq       +0x08 ucnt     +0x0C bytes    +0x10 state
+0x14 half       +0x18 push      +0x1C slot
+0x64 p2                         +0x68 p3                       +0x6C stale
```

- `state != 0`: the block is being written; it is not copied.
- `half` is 0 or 1: a capture is published in **two consecutive blocks**.
- `p3`: the upper 16 bits are the **burst frequency in 100 kHz units** (21150 → 2,115.0 MHz); the low
  byte carries the band. `p2` is the next frequency in the plan.
- `stale`: the slot may have been reassigned before it was copied. It is exposed, not discarded.
- `seq` advances per block; `push` counts the pages the publisher has released.

A capture is **11,904 samples** split across two blocks: 8,128 + 3,776 samples (16,256 + 7,552
bytes). At **1.92 MS/s** that is a **6.2 ms** window.

## 2. The three sample formats

### 2.1 Five bits (`CSD1`)

Each 16-bit word carries **two 5-bit fields**:

```
 bit  15 14 13 12 11 | 10  9  8 |  7  6  5  4  3 |  2  1  0
      \___________/            \_____________/
            Q                        I
```

- `I = ((w >> 3) & 0x1F) - 16`
- `Q = ((w >> 11) & 0x1F) - 16`
- Bits 0-2 and 8-10 are quantization noise (< 2%); they are not used.
- Actual range: −16..15. Normalized by dividing by 16 → **−1.0 .. +0.9375** in `complex float`.
- For CS16 the code is multiplied by 2048 (−32768..30720, without overflowing).

### 2.2 Eight bits (`CSD8`)

The sample is still 2 B, but all 16 bits are data: **there are no reserved fields**.

```
 bit 15 14 13 12 11 10  9  8 |  7  6  5  4  3  2  1  0
 \________________________/   \_____________________/
              Q                            I
          (high byte)                  (low byte)
```

- `I = (w & 0xFF) - 128`, `Q = ((w >> 8) & 0xFF) - 128`.
- It is **binary offset**, not two's complement: `0x80` is zero, `0x00` is −128 and `0xFF` is +127.
  With a zero-mean signal the byte mean is around 128 (measured: 111-125).
- Normalized: `I/128`, `Q/128` → **−1.0 .. +0.9921875** in `complex float`.
- To CS16: `code × 256` → −32768..32512, without overflowing.

The geometry does not change: 1.92 MS/s, 11,904 samples and two blocks per capture. Only how the
words are read changes.

### 2.3 CS16 (`RF30`)

Two signed 16-bit integers per sample, interleaved I/Q. This is the legacy format: its blocks carry
no frequency and the axis stays at the requested frequency.

## 3. Driver (`driver/`)

- **Signature detection, not argument-based**: `IqWindow::parse_block` accepts `CSD1`, `CSD8` and
  `RF30` and fills an `IqBlockInfo` (seq, bytes, state, half, push, slot, p2, p3, stale, format and
  `freq_hz` already decoded).
- The signature is read with an aligned 32-bit load, because the window is non-cacheable memory. The
  payload is never interpreted over the window: it is first copied to RAM.
- `read()`/`read_next()` accept an optional `IqBlockInfo*` with the header of the delivered block;
  `last_block()` stores the last one.
- `readStream()` converts according to the format of the current block: 2 B/sample (unpacks to CF32
  or CS16) or 4 B/sample (CS16 direct). If a packed block carries more samples than requested, the
  remainder stays at the start of the buffer for the next call. When a block is exhausted it sets
  `SOAPY_SDR_END_BURST` in `flags`.
- **Frequency exposed**:
  - `getFrequency(SOAPY_SDR_RX, 0)` returns the frequency of the last delivered block
    (`rx_follow_block=1`, by default; with `0` it returns the one requested in `setFrequency`). The
    second half of a capture inherits the frequency of the first.
  - `getSampleRate(SOAPY_SDR_RX, 0)` returns 1.92 MS/s while the format is packed.
  - Sensors: `RX_FREQ`, `RX_RATE`, `RX_SEQ`, `RX_FORMAT`, `RX_STALE`.
  - `readSetting("rx_block")`: `seq, blk, bytes, freq_hz, next_hz, push, slot, stale, format, rate`.
  - `readSetting("rx_format")`: `packed5` | `packed8` | `cs16` | `unknown`.
- **The ambiguous case**: a `CSD1` block with an 8-bit payload (images older than the `CSD8`
  signature) is detected once on the RAM copy — if bits 0-2 and 8-10 are not zero — and remembered
  for the following blocks. `rx_format=packed8` forces it from the first one.

## 4. App (`app/`)

- `FileSource` looks at the **signature of the file's first block**:
  - `CSD1` → 5-bit; `CSD8` → 8-bit; anything else → CS16 dump in the legacy format.
  - A file with a `CSD1` signature and an 8-bit payload is detected when loaded into RAM.
- The header frequency and sample rate set the axis: the centre is set to `p3` and the span to
  1.92 MHz. The tuning controls are still there, but the next block sets the real centre of the data
  again.
- The spectrum is drawn **complete and centred** (−fs/2 on the left, +fs/2 on the right). It used to
  stretch the positive half across the whole width, which left a +240 kHz tone drawn to the left of
  the centre.
- `--check-bin N` (`--headless` only): requires the most repeated peak to fall in bin N of the
  centred spectrum and exits with an error if it does not.

## 5. Local test, without a phone

The generator writes a known signal: a complex tone at **fs/8 (+240 kHz)** with noise, which falls
exactly in bin `N/8` for any power-of-two FFT, plus the centre offset `N/2`.

```sh
make -C app                                            # native app
python3 tools/make-iqfile.py --out /tmp/iq5.bin         # 10 captures of 5-bit, 235 KB
python3 tools/make-iqfile.py --out /tmp/iq8.bin --format 8
./app/fmiq-app --headless 24 --fft 2048 --source file --file /tmp/iq5.bin \
               --check-bin 1280 --out /tmp/wf.ppm
./app/fmiq-app --headless 12 --fft 2048 --source file --file /tmp/iq8.bin \
               --check-bin 1280 --out /tmp/iq8.ppm
tools/test-iq5file.sh                                   # full cycle: 5-bit, 8-bit, ambiguous and CS16
```

Generator options:

```
--out PATH           output file
--freq-mhz F         header frequency (default 2115.0)
--offset-khz K       tone relative to the centre (default fs/8 = 240 kHz)
--amp CODES          tone amplitude (default 8 out of 16)
--noise CODES        noise deviation (default 1.1)
--captures N         captures of 11,904 samples (default 10)
--format {5,8,cs16}  payload format (default 5)
--band N             band in the low byte of the header (default 1 in 8-bit, 0 in 5-bit)
--legacy             alias of --format cs16
--legacy-signature   with --format 8, writes a CSD1 signature (tests the ambiguous case)
```

Results measured with the files in `tools/testdata/` (header at 2,115.0 MHz):

| FFT | expected bin | 5-bit | 8-bit | peak frequency | level |
|---|---|---|---|---|---|
| 512 | 320 | 320 (12/12) | 320 (12/12) | 2115.240 MHz (+240.000 kHz) | −6.0 dBFS |
| 1024 | 640 | 640 (12/12) | 640 (12/12) | 2115.240 MHz (+240.000 kHz) | −6.1 dBFS |
| 2048 | 1280 | 1280 (11/12) | 1280 (11/12) | 2115.240 MHz (+240.000 kHz) | −6.4 dBFS |
| 4096 | 2560 | 2560 (11/12) | 2560 (11/12) | 2115.240 MHz (+240.000 kHz) | −6.5 dBFS |

The PPM waterfall lights column 1279/2048 (1280 is the peak; the neighbour is almost the same
because of the Hann window). The axis ends up as: bin 0 = 2114.040 MHz and bin 2047 = 2115.959 MHz,
with the centre at 2115.000 MHz, which is what the header says. Frames 11/12 rather than 12/12 are
the ones that straddle the cut between two captures (different phase): the modal bin is still the
right one.

The files in `tools/testdata/` are kept in the repository so that the check does not depend on
generating anything.

## 6. Verified on the phone, and what remains

Verified: the app opens the real ring, consumes blocks with headers, follows the frequency of each
burst (213 different frequencies between 763.0 and 2,685.0 MHz in 50 s of sweeping) and a dump of 19
captures opens on the laptop with `--source file` without converting it.

Known limits of the format:

- 5-bit quantization is good for detecting and locating a signal, not for demodulating or measuring
  fine amplitude. The 8-bit one gives three more bits, but it arrives at a lower level (see
  `docs/266-arquitectura-modo-sdr.md`).
- The frequency is the sweep plan's: it serves to label the burst, not to tune.
- In closely spaced captures the frequency may belong to the next burst; `stale` reveals it.
