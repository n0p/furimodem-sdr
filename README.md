# furimodem IQ — SDR mode on the phone's modem

Turns the FuriPhone modem into an **IQ receiver** without a cell: the receiver is enabled with a
**binary patch** applied by a dedicated tool that knows how to revert it, and the data is read over
SoapySDR from an app with a waterfall.

## Components

| path | what it is |
|---|---|
| `driver/` | SoapySDR module (`driver=furimodem_iq`): tuning, width and gain controls, TX safeguard and IQ ring reads |
| `app/` | Wayland + EGL/GLES demo: waterfall, spectrum and controls; synthetic, file and device sources |
| `gui/` | GTK3 window for the phone: status, load, activation, unload and factory restore; invokes `sdr-load` as a subprocess |
| `tools/sdr-load.cpp` | loader: checks preconditions, applies the SDR-mode image, **activates the mode** and reverts |
| `tools/make-iqfile.py` | generates IQ block files with a known tone, for testing without the phone |
| `tools/test-iq5file.sh` | full local test cycle: 5-bit, 8-bit, ambiguous dump and CS16 |
| `tools/crossbuild2.sh` | reproducible cross-build of the whole package (base **Debian sid**, the phone's) |
| `tools/iqdump/` | analysis and patch-generation tooling (not shipped) |
| `docs/266-arquitectura-modo-sdr.md` | technical map: parts, formats, what is measured and what limits apply |
| `docs/261-formato-de-muestras.md` | ring contract: header, unpacking and local test |

## Getting started

```sh
# 1. Soapy module (on the phone, with the SoapySDR headers installed)
cd driver && cmake -S . -B build && cmake --build build -j
sudo cmake --install build

# 2. app
cd app && make -j && sudo make install     # installs the binary, icon and launchers

# 3. SDR mode
sudo sdr-load --image <sdr-mode-image.img>     # applies the image
                                               # (the phone restarts)
sudo sdr-load --resume                         # completes the activation, no new write
sudo sdr-load --status                         # image, PLL, rails, loops and whether the ring beats
#    ... and to go back:
sudo sdr-load --deactivate                     # stops the loops
sudo sdr-load --restore                        # restores the stock firmware
```

`sdr-load --activate` runs the receiver start-up sequence (PLL, rails and loops, nine verified
steps) **only with the images in its profile table**: today `standard-a`, `standard-b` and
`sdr-8bit`. The last one is the SDR-mode image: 8 bits per component, origin 3 and **fixed tuning at
2,115.0 MHz** (band 1), so `--activate` writes it, starts the sweep and checks that the ring
publishes. **The phone restarts when the image is written**: any call in progress is dropped and
mobile service is gone until you restore to stock.
The same happens with an image that is not in the table: it refuses and lists the ones it knows,
instead of requesting modes blindly. A new profile is passed with `--profile` (name or file).

**`--resume` is what finishes the job after the restart.** It checks that the partition holds the
image the state recorded (sha256 and size), closes the pending flag and runs the activation; the
partition is **not** written again. `--activate` is idempotent and does the same, so a half-done
activation is never left there. `--status` says it in its `next` line.

With the image loaded, capture is triggered by the **phone's own frequency sweep**:

```sh
sudo busctl --timeout=30 call org.ofono /ril_0 org.ofono.FuriLabs.AT SendCommand s "AT+CFUN=1"
sudo busctl --timeout=300 call org.ofono /ril_0 org.ofono.FuriLabs.AT SendCommand s "AT+COPS=?"
```

Without a sweep there is no data: the receiver delivers bursts, not a continuous stream.

The same sequence, from phosh: `furimodem-sdr` (see `gui/README.md`) shows the status and launches
`sdr-load` as a subprocess to load, activate, stop and restore factory settings. The operations
that need root go through `pkexec`, so the desktop asks for authorisation in its own dialogue;
without pkexec, or if the dialogue is cancelled, the window shows the exact command with a button
to copy it. The window is dark on its own, whatever the desktop theme is.

Two launchers appear on the phone: **Furimodem IQ** (device) and **Furimodem IQ (capture)**
(plays back a dump saved at `/var/lib/furimodem-iq/captura.iq`).

## Status and limits (important)

What **works today**, measured on the phone: the waterfall paints **real I/Q from the modem** on the
screen, with the axis set to the frequency carried by each block; during a sweep, **213 different
frequencies between 763.0 and 2,685.0 MHz** were followed in 50 s; and capture at **8 bits per
component** works with the correct RF tap (a test CW appears in the bin it belongs to in **9/9**
captures). The live waterfall was tested with the 5-bit format; the 8-bit one was tested on captures
already dumped and opened with the same reader and the same app.

The two limits that constrain use:

- **The 8-bit path is less sensitive.** It gains three bits of resolution (18 dB of quantization) and
  removes the low-bit tie of the 5-bit format, but it arrives on the order of **26 dB weaker**
  because it does not go through the automatic gain control. With the same tone: 38.1 dB
  peak/median in 5-bit versus 11.7 dB in 8-bit. It is more resolution at a lower level, not a
  better signal-to-noise ratio. Matching the gain of the two taps is still pending.
- **The frequency sweeps; it does not tune.** The receiver walks its sweep plan and each block
  carries the frequency of that burst in the header, which is what the app paints on the axis. The
  frequency controls in the interface **do not move the oscillator**: the app pushes the request to
  the module and holds it on screen for two seconds, but the header wins again and an amber notice
  states that tuning is controlled by the activated profile (or by the block header, for a file).
  Forcing the label in the image still **labels but does not tune** (measured: with the label fixed,
  the tone disappears). To see a given band, you have to wait for the sweep to pass through it.
  Details: `docs/271-controles-de-la-app.md`.

Other limits:

- **No sweep, no stream**: there is no continuous capture or continuous recording. With the old copy
  loop, 13.5-15 dumps/s sustained were measured (43/s with the corrected reader), on the order of
  0.3-0.5 MS/s of useful samples; the cell searcher's ring publishes ~100 captures/s during the
  sweep.
- **5-bit quantization** is good for detecting and locating a signal, not for demodulating or
  measuring fine amplitude. The 8-bit one gives three more bits, with the sensitivity limit above.
- **TX is not implemented** and the driver blocks it by design (three interlocks).
- SDR mode **disables the modem's radio**: with the image loaded the telephony service does not start
  because there is no radio HAL. That is the intended behaviour; go back with `--restore`.

Full detail in `docs/266-arquitectura-modo-sdr.md`.

## Using each tool

```sh
# app: window on the phone, or analysis without a GPU
fmiq-app                                              # live waterfall (Wayland + EGL)
fmiq-app --headless 60 --source soapy --out /tmp/demo.ppm   # no window, device source
fmiq-app --headless 12 --fft 2048 --source file --file dump.bin --check-bin 1280
fmiq-app --gpu-test 120 --fft 512 --size 480x1025 --source soapy --out frame.ppm

# GTK3 window for SDR mode: status, load, activation and factory restore
furimodem-sdr

# dump the ring to a file and open it later on the laptop
sudo ./driver/tools/iqring_pull 20 /tmp/iqring.bin     # on the phone
./app/fmiq-app --headless 4 --source file --file iqring.bin

# driver probes and tests
SOAPY_SDR_PLUGIN_PATH=$PWD/driver/build SoapySDRUtil --probe=driver=furimodem_iq
sudo ./driver/build/selftest_transport --live /dev/ccci_ccb_meta 5
```

## Verification

```sh
cd driver && ctest          # TX safeguard · ring reader · transport
cd app && make test         # FFT, performance and waterfall
cd gui && make test         # status parsing, commands and exit codes
tools/test-iq5file.sh       # 5-bit, 8-bit, ambiguous dump and CS16, without the phone
```

No firmware image is distributed: patches are generated from the stock image on the user's machine
and verified by sha256 before and after writing.

## How this was developed

The mechanism was found by testing close to **200 modified firmware images**; the result is
delivered as a binary patch that is applied on the user's own device, never as a redistributed
firmware image.

## License

This project is distributed under the **GNU General Public License v3.0** (GPLv3).
The full text is in [`LICENSE`](LICENSE).

Copyright (C) 2026 the furimodem-sdr authors.
