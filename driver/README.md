# furimodem_iq — SoapySDR driver

## Build and install

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
sudo cmake --install build          # installs libfurimodemIQ.so into .../SoapySDR/modules0.8
# or, to test without installing:
SOAPY_SDR_PLUGIN_PATH=$PWD/build SoapySDRUtil --probe=driver=furimodem_iq
```

Single dependency: SoapySDR (tested with 0.8.1).

## Device arguments

| arg | effect |
|---|---|
| `driver=furimodem_iq` | required |
| `tx_enable=1` | **creates the TX channel**. Without this argument `getNumChannels(TX)` returns 0 and `setupStream(TX)` fails |
| `lab=1` | laboratory build: no policy caps (hardware limit only) |
| `allow_risky_bands=1` | only with `lab=1`: allows transmitting in bands marked as risky |
| `ccb_node=<path>` | DL ring node (default `/dev/ccci_ccb_meta`) |
| `ccb_ctrl=<path>` | CCB control page (default `/dev/ccci_ccb_ctrl`) |
| `ring_file=<path>` | uses a **ring simulated in a file** instead of the node (tests) |
| `ccb_entry=<n>` | (advanced) control-page entry; by default the node is asked |
| `transport=0` | opens no node: the driver stays in "no transport" mode |

The **write** channel (UL ring, AP→MD) is not exposed through Soapy: it is opened by whoever uses
`CcbTransport` with `writableRing=true` (the selftest, `--send`, and any C++ client). It is
documented below, in *Write transport*.

Examples:

```sh
SoapySDRUtil --probe=driver=furimodem_iq                 # 1 Rx, 0 Tx
SoapySDRUtil --probe="driver=furimodem_iq,tx_enable=1"   # adds Tx (public build, limits)
SoapySDRUtil --probe="driver=furimodem_iq,ring_file=/tmp/ring.bin"   # simulated ring
```

## Receive transport (CCB META DL ring)

`readStream` reads from the **DL ring of the CCB_META user** that the modem writes in normal mode.
There is no thread or DMA involved: `mmap` of the node + index polling.

- Geometry: 8 pages of 66560 B (66552 usable payload bytes) at the start of the META block; the UL
  ring follows it, in the same shared block.
- Each page: 8 B header `{u32 magic, u32 len}`; `0x11111111` while being written,
  `0x22222222` once published. **A page with `0x11111111` is neither consumed nor delivered.**
- Control header: 64 B per user in `/dev/ccci_ccb_ctrl` (META = entry 19;
  it is queried from the node with `CCB_CTRL_OFFSET` and the four guards
  `0xddbbccaa`/`0xeeff0011` are validated). Indices `dl_alloc/dl_free/dl_read/dl_write` +
  `dl_page/dl_size`.
- Consume = copy the page at `dl_read_index` and advance **`dl_read_index` and then
  `dl_free_index`** (ack to the modem). No ioctl is needed; the ones used
  (`SMEM_BASE`, `SMEM_LEN`, `CCB_CTRL_OFFSET`) are read-only.
- The indices are **modular (0..n−1)**, not free-running counters: a 32-bit subtraction would give
  a huge value on every wrap. A full ring (all but one slot) is avoided because the producer always
  leaves one page free.
- The region is **Device-nGnRnE** on arm64: no `memcpy` from the ring (SIGBUS on wide unaligned
  accesses). `ccb_transport.cpp` copies with aligned 32-bit reads and a byte-by-byte tail.
- `readStream` returns at most `numElems` complex samples: the ring delivers **native CS16** and
  the driver converts to CF32 with scale `1/32768`. With `SOAPY_SDR_CS16` it copies the bytes
  unchanged. The 0..3 bytes that do not complete a sample carry over to the next call (nothing is
  lost).
- No allocations per call: the staging buffer (one page) is reserved in `attach()` and the
  conversion buffer only grows when larger blocks are requested.
- New sensors: `RX_PENDING` (pending pages), `RX_DROPPED` (resyncs due to corrupt indices),
  `RX_BAD` (pages with invalid magic/length, discarded).
- **Without a node** (missing, no permissions, or `transport=0`) the driver behaves as before:
  it enumerates, `setupStream(RX)` works and `readStream` returns `SOAPY_SDR_TIMEOUT`
  (waiting at most 10 ms so it does not spin). `getHardwareInfo()` publishes
  `rx_transport=none|ccb-meta|sim|disabled`.
- Note: consuming the DL ring **advances the META indices**. In normal mode that ring carries the
  META channel stream, not DSP IQ; the driver delivers those bytes as CS16 samples.

## Firmware IQ ring

If the firmware publishes captures in the memory ring (`iq_node`, 16 blocks of 16 KB in a fixed
window of the shared map), `IqWindow` reads them **before** the CCB transport. The 128 B header
starts with a signature that decides the format, with no arguments:

| signature | samples | size |
|---|---|---|
| `CSD1` | 5-bit I/Q in 16-bit words (I in 3..7, Q in 11..15, minus 16) | 2 B/sample |
| `CSD8` | 8-bit I/Q in 16-bit words (I in the low byte, Q in the high byte, minus 128) | 2 B/sample |
| `RF30` | interleaved CS16 (previous format) | 4 B/sample |

- The burst frequency is in `p3` (+0x68): the high 16 bits in units of 100 kHz (the low byte
  carries the band). It is exposed in `getFrequency(RX)` (the one of the last delivered block), in
  the `RX_FREQ`, `RX_RATE`, `RX_SEQ`, `RX_FORMAT` and `RX_STALE` sensors, and in
  `readSetting("rx_block")`.
- `RX_FORMAT` is `packed5`, `packed8` or `cs16`. With either packed format,
  `getSampleRate(RX)` reports 1.92 MS/s, the real rate of the window.
- `rx_format=packed8` forces reading dumps that carry the `CSD1` signature and an 8-bit payload;
  by default the signature rules and, if the `CSD1` block does not carry it, the format is inferred
  once from the RAM copy (never from the Device window).
- `rx_follow_block=0` stops `getFrequency` from following the block and returns the requested
  frequency.
- At the end of a block, `readStream` sets `SOAPY_SDR_END_BURST` in `flags`.
- The previous format behaves as always: its blocks carry no frequency.

### Tests

On a laptop (there is no `/dev/ccci_ccb_*`): ring simulated in memory and in a file.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/selftest_transport            # full suite (116 checks, includes stress)
./build/selftest_iqwindow             # ring reader: CS16, CSD1/CSD8, frequency and unpacking
ctest --test-dir build                # tx_guard + iqwindow + transport
./build/selftest_transport --mkring /tmp/ring.bin   # empty simulated ring (DL + UL)
```

On the phone (`--peek` does not touch the indices; `--live`/`--device` consume the DL ring; `--send`
publishes on the UL ring and rings the doorbell — see *Write transport* below):

```sh
sudo ./build/selftest_transport --peek                  # indices and headers, without consuming
sudo ./build/selftest_transport --live /dev/ccci_ccb_meta 5   # B/s and first samples
sudo ./build/selftest_transport --device 5              # through SoapySDR: kSa/s and sensors
sudo ./build/selftest_transport --send 01000000 --repeat 5     # AP->MD, with the modem state block
SOAPY_SDR_PLUGIN_PATH=$PWD/build SoapySDRUtil --probe=driver=furimodem_iq
```

`tools/crossbuild2.sh` (with the `debian:trixie` container) leaves the aarch64 binary in
`/out/selftest_transport`.

If `--peek` says "cannot open the ring", permissions on `/dev/ccci_ccb_meta` and
`/dev/ccci_ccb_ctrl` are missing, or `mdlogger` holds the node. The receive transport never writes
a page: it only reads and publishes `dl_read`/`dl_free`. Writing is optional (`writableRing`) and
goes through the UL ring.

## Write transport (CCB META UL ring, AP→MD)

`CcbRingWriter` is the symmetric half of the reader: same 64 B header and same page format, on the
UL ring that the modem has behind the DL (8 pages of 64512 B = 64504 usable bytes per message).

- One message = one page: `magic 0x11111111` → payload → `len` → `magic 0x22222222` →
  `ul_alloc`/`ul_write` → `ioctl(SMEM_TX_NOTIFY)` (`AP_MD_CCB_WAKEUP` bit).
- The doorbell is **coalescing** (one doorbell drains all pending pages), so
  `post()` publishes and `flush()` notifies once per burst. `send()` = `post` + `flush`.
- Credit: one page is always left unpublished; `post()` returns `0` if the ring is full
  (retry when the modem acks) and `-1` if the message does not fit in one page.
- `post()`/`flush()` are enabled with `CcbOpenOptions::writableRing = true`, which opens the node
  with `O_RDWR` and maps it `PROT_WRITE`. Without that flag the transport remains read-only and
  `post()` returns `kTxDisabled`.
- It writes with aligned 32-bit stores + a byte-by-byte tail (Device-nGnRnE, no `memcpy`).
- Counters: `sentPages`, `stalledPosts` (full), `resyncs` (indices out of range),
  `doorbells`, `inFlightPages`, `freePages`.

**Test on the phone** (publishes, rings, waits for the ack and reads the modem state block
read-only). It does not write the modem's notification register: that register is read-only, the
CCB message is the wake-up.

```sh
sudo ./build/selftest_transport --send 01000000 --repeat 5 --gap-ms 20 --wait-ms 500
sudo ./build/selftest_transport --send 41542b43474d490d   # "AT+CGMI\r" as payload
sudo ./build/selftest_transport --send 01000000 --no-mirror
sudo ./build/selftest_transport --text hola --no-notify    # negative control
```

Criterion: `published = 5`, `doorbells = 1`, `in_flight = 0` (the modem drained: `ul_free` reached
`ul_write`) and the modem heartbeat increases. The mirror is read by default (read-only;
`--no-mirror` turns it off) and also prints the capture counter. The risks are described in the
previous section.

## TX safeguard

Three interlocks, all mandatory:
1. `tx_enable=1` in the arguments — without it the TX channel does not exist.
2. The stream must be **activated** (`activateStream`) before writing; otherwise `writeStream` is
   rejected.
3. Limits enforced in code: in the public build, 2.4 GHz only and gain ≤10 dB; cap on samples per
   write. The laboratory build removes the policy caps, but risky bands remain blocked unless
   `allow_risky_bands=1`.

Every policy decision lives in `tx_guard.hpp`, which is tested on its own: `ctest` or
`./build/tx_guard_test`.

## Status

- Enumeration, controls (frequency, band, sample rate, gain/AGC, bandwidth, antennas, sensors) and
  TX policy: **done**.
- Receive transport over the CCB META DL ring (`ccb_transport.{hpp,cpp}`): **done**, with a selftest
  of the logic over a simulated ring (`./build/selftest_transport`, 116 checks OK) and tested
  against the real modem (`--peek`/`--live`/`--device` on the phone).
- **IQ ring** (`iq_window.{hpp,cpp}`): **done and tested on the phone**. It reads the three formats
  by signature (`CSD1` 5-bit, `CSD8` 8-bit and `RF30` CS16), follows the frequency of each burst and
  publishes it in `getFrequency`/sensors. The app waterfall draws that real I/Q during a sweep.
- **Write** transport over the CCB META UL ring (`CcbRingWriter`, `writableRing=true`): **done and
  tested without hardware**; on the phone it is exercised by `--send` (above). It publishes on the
  ring and rings the doorbell, but does **not** write the modem's notification register (that
  register is read-only): the CCB message is the wake-up.
- RF TX transport: **pending**. `writeStream` validates the policy and discards the block instead of
  inventing data.
