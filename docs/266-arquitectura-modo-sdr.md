# SDR mode architecture

System-level document, in terms of blocks, formats and measurements. It explains which parts exist,
how a sample travels from the receiver to the screen, what has been measured and what limits apply
today.

## What it is

SDR mode turns the phone's modem into an **I/Q receiver** without a cell, without a SIM and without
the modem providing service. It is entered by loading a firmware image **patched in binary** and left by restoring the stock image; both operations are done by `sdr-load`, which
verifies the sha256 before and after writing.

The mode is **receive-only**. The driver does not implement TX and blocks it by design.

## The parts

| part | where it runs | what it does |
|---|---|---|
| `sdr-load` | AP | powers down the radios, writes the image, verifies, activates and reverts |
| `libfurimodemIQ.so` | AP | SoapySDR module: reads the IQ ring and presents it as a stream |
| `fmiq-app` | AP | spectrum and waterfall app (Wayland + EGL/GLES) |
| image patch | modem | publishes each receiver capture into a memory ring that the AP reads |

The AP and the modem share a memory window. The modem writes the captures there; the AP maps the
window and reads them. There is no DMA and no per-sample interrupt: the AP polls indices and copies.

Everything is compiled for aarch64 with `tools/crossbuild2.sh` on a **Debian sid** base (the
phone's). With an older base the code does not compile, because Wayland has fewer fields in its
structures.

## The load window

`furimodem-sdr` is a GTK3 window that does none of the work itself: it builds one `sdr-load`
command per button, runs it as a subprocess without a shell and shows its output and its exit
code. Two decisions belong to the architecture, because they change how the mode is operated:

- **Privileges.** Reading the state works as a normal user; the tool says which part of the live
  modem it cannot see, and the window offers to repeat the read with privileges. Loading,
  entering the mode, activating, stopping and restoring need root. The window never runs `sudo`
  and never sees a password: it hands the command to `pkexec`, and polkit's agent — a service of
  the desktop — draws the dialogue. The action is `org.furimodem.sdr.run`
  (`allow_any=no`, `allow_inactive=no`, `allow_active=auth_admin_keep`, pinned to
  `/usr/bin/sdr-load`) and it is installed with the package. If the dialogue is cancelled
  (`pkexec` exits 126) or there is no pkexec, nothing is run: the window goes back to idle and
  shows the exact command to copy, so an operation is never left half-started.
- **Looks.** The window carries its own dark stylesheet, embedded in the binary and loaded above
  the session theme, so it is dark on a light desktop too. This is presentation only: it changes
  nothing of what the tool does.

## The IQ block

The ring is **16 slots of 16 KB**. Each block starts with a **128 B** header in little-endian and
continues with the samples. The header says, among other things:

- the **signature**, which identifies the payload format;
- `seq`, a counter that advances per block;
- `bytes`, the useful payload size;
- `state`, non-zero while the block is being written (it is not consumed);
- `half` (0 or 1): a capture is published in **two consecutive blocks**;
- the **burst frequency**, in 100 kHz units, and the **next** one in the sweep plan;
- `stale`: the slot may have been reassigned before it was copied; it is exposed, not discarded.

A capture is **11,904 samples** split across two blocks (8,128 + 3,776). At **1.92 MS/s** that is a
**6.2 ms** window.

## The three formats

| signature | contents | bytes/sample |
|---|---|---|
| `CSD1` | I/Q at **5 bits** per component in 16-bit words | 2 |
| `CSD8` | I/Q at **8 bits** per component in 16-bit words | 2 |
| `RF30` | interleaved **CS16** (legacy format) | 4 |

The signature decides: a single aligned 32-bit integer is read and the payload is not interpreted
blindly. The window is non-cacheable memory and only accepts aligned 32-bit accesses; a byte-by-byte
sweep causes a bus fault. Unpacking details are in `docs/261-formato-de-muestras.md`.

`CSD1` blocks published by 8-bit images older than the `CSD8` signature are detected once, on the
copy already made in RAM, and remembered; `rx_format=packed8` forces it.

## Where the samples come from

The modem receiver does not deliver a continuous stream: it delivers **bursts**. The capture engine
is triggered inside the radio scheduler, and the path that works today is the **cell searcher**:
during a frequency sweep the firmware captures one burst per candidate and leaves the page in a
slot; the patch copies each released slot into the ring, in two blocks, and the AP reads it.

Practical consequence: **no sweep, no data**. The sweep is launched from the phone itself (for
example, a network search). During the sweep the ring publishes on the order of **100 captures per
second**.

## What has been measured

- **Live waterfall on the phone**: the app opens the ring, consumes real blocks and draws the
  spectrum centred on the header frequency. In 50 s of sweeping, **213 different frequencies between
  763.0 and 2,685.0 MHz** were seen; the process uses ~9.6% CPU with the sweep running.
- **Test tone (5-bit)**: a CW from an external generator at +199.7 kHz from the centre appears in the
  correct bin in **5/5** captures, with **38.1 dB** peak/median and the image suppressed by 10.1 dB.
  It is genuinely complex I/Q: `I+jQ` gives a single peak and `I−jQ` inverts it.
- **Test tone (8-bit)**: with the 8-bit RF tap, the same CW appears in **9/9** captures, with
  **11.7 dB** peak/median and the image at −9.0 dB.
- **Local test without the phone**: the generator `tools/make-iqfile.py` writes a known tone and
  `fmiq-app --check-bin` verifies that it falls in the expected bin, for FFTs from 512 to 4,096 and
  in all three formats.

## The two limits that must be stated plainly

**1. The 8-bit path is less sensitive.** It gives three more bits of resolution (18 dB of
quantization) and removes the low-bit tie that the 5-bit one has, but **the tap arrives on the order
of 26 dB weaker**, because it does not go through the automatic gain control that the 5-bit path
does have. With the same CW: 38.1 dB peak/median in 5-bit versus 11.7 dB in 8-bit. It is not a
signal-to-noise ratio improvement; it is more resolution at a lower level. Matching the gain of the
two taps is still pending.

**2. The block frequency is a tuning command, but only through one route.** The header carries the
burst frequency according to the sweep plan, and the app puts the axis there. The oscillator follows a
radio-database entry that the scheduler builds **before** the search command is prepared, so writing
the frequency at command-preparation time **labels the blocks without tuning** — measured: with the
label pinned at 2,115.0 MHz the test tone vanished to 2.8 dB. Writing band and frequency into the
scheduler's buffer **before** that database is computed **does** tune: measured **7 of 7** captures
with the tone present at +198.8 kHz (12.8-15.2 dB over the median) and the label correct in **21 of
21** blocks. The interface's frequency controls now push `setFrequency` and compare it with the
block read-back, but they still cannot move the oscillator while the activation profile fixes it:
after two seconds the axis returns to the header and the app says so on screen
(`271-controles-de-la-app.md`). The fixed frequency comes from the activation profile.

Other, minor limits:

- Capture is tied to the sweep: there is no continuous stream or continuous recording.
- 5-bit quantization is good for detecting and locating a signal, not for demodulating or measuring
  fine amplitude.
- In closely spaced captures the header frequency may belong to the next burst; `stale` reveals it.
- **TX is not implemented** and the driver blocks it with three interlocks.

## How something is verified

Project rule, learned from false positives:

- Data being present means nothing: the monitor delivers data with the radio off.
- A counter going up means nothing.
- **The only valid evidence is RMS being monotonic with gain, or a tone that appears and shifts**,
  measured with interleaved A/B inside the same burst: drift between windows is ±46% and a naive
  comparison between different captures invents signals of +1.8 dB.
