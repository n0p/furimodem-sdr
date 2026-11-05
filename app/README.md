# furimodem-iq — demo app (mobile)

IQ mode demo: **real-time GPU waterfall** (Wayland + EGL + OpenGL ES 3), spectrum trace and
controls for **tuning, band, sample width and gain**. There are three sample sources —
synthetic, file and device (SoapySDR) — and the app runs on the phone with the real IQ ring.

## Contents

| File | Role |
|---|---|
| `src/source.hpp` | `SampleSource`, `SyntheticSource`, `FileSource` and `DeviceSource` (SoapySDR) |
| `src/source_device.cpp` | `DeviceSource` implementation over the Soapy module |
| `src/source_device_stub.cpp` | variant without SoapySDR, to build the app on its own |
| `src/tune_hold.hpp` | user-tune hold versus the per-block header, and the rejection verdict |
| `src/pipeline.hpp` | acquisition → FFT threads, SPSC queues, already-coloured rows |
| `src/render_egl.{hpp,cpp}` | Wayland/xdg-shell window + EGL/GLES3 context; offscreen EGL for tests |
| `src/render_gl.{hpp,cpp}` | waterfall texture, spectrum, rectangles and 5×7 font |
| `src/main.cpp` | touch UI, `--headless` and `--gpu-test` modes, drawing loop |
| `src/tune_hold_test.cpp` | unit test: header versus user values, hold expiry, notices |
| `Makefile` | build without CMake (the phone has no cmake) |
| `CMakeLists.txt` | development/cross build on a laptop |

## Dependencies

On the **FuriPhone** (verified, nothing to install): `g++`, `make`, `pkg-config`,
`wayland-scanner`, `wayland-protocols`, `egl 1.5`, `glesv2 3.2` (GLES3 headers),
`wayland-client 1.26.0` and `wayland-egl 18.1.0`. **There is no cmake**, which is why the
`Makefile` is the main path on the phone. The `libEGL` libraries present are
`libEGL_libhybris.so.0` and the Mesa one. **libdrm** and any other dependency are not used.

## Build on the phone (without CMake)

```sh
cd app
make -j1                 # nproc may be 1; leaves ./fmiq-app
make test                # unit tests + a 48-frame headless run
./fmiq-app               # inside the phosh session
```

If `wayland-protocols` is not on the pkg-config path:

```sh
make WAYLAND_PROTOCOLS_DIR=/usr/share/wayland-protocols
```

Over SSH there is no `WAYLAND_DISPLAY`; to launch it in the graphical session:

```sh
export XDG_RUNTIME_DIR=/run/user/$(id -u)
ls "$XDG_RUNTIME_DIR"/wayland-*        # e.g. wayland-0
export WAYLAND_DISPLAY=wayland-0
./fmiq-app
```

## Build on a laptop

With Make (same development dependencies) or with CMake:

```sh
make -C app -j4
# or
cmake -S app -B app/build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build app/build -j4
ctest --test-dir app/build --output-on-failure
```

Cross to aarch64 (no cross-toolchain is installed on this laptop; the CMake setup is ready):

```sh
# sudo apt install g++-aarch64-linux-gnu   # plus a sysroot with the phone EGL/GLES/Wayland
PKG_CONFIG_PATH=/path/sysroot/usr/lib/aarch64-linux-gnu/pkgconfig \
cmake -S app -B app/build-aarch64 \
      -DCMAKE_TOOLCHAIN_FILE=app/cmake/aarch64-linux-gnu.cmake \
      -DCMAKE_BUILD_TYPE=Release
```

## Usage

```sh
./fmiq-app                         # Wayland window, synthetic source
./fmiq-app --headless 200 --out wf.ppm    # 200 frames on the CPU, no GPU (2048x200)
./fmiq-app --gpu-test 300 --size 360x800 --out ui.ppm  # frames in offscreen EGL
./fmiq-app --seconds 5             # exits by itself after 5 s (smoke test)
./fmiq-app --source file --file dump.bin --check-bin 1280   # CSD1/CSD8 block file
```

Options: `--fft N` (power of 2, 256–4096), `--band 0..5`, `--freq MHZ`, `--rate MSPS`,
`--gain DB`, `--scale N` (0 = infer from `wl_output`), `--source auto|synthetic|soapy|file`,
`--file PATH` and `--check-bin N` (headless: requires the peak in that bin of the centred
spectrum).

`--source file` accepts two families and tells them apart by the file signature: the sequence of
`CSD1` (5-bit I/Q) or `CSD8` (8-bit I/Q) blocks, with the per-block frequency and what
`tools/make-iqfile.py` generates, and the CS16 dump of the previous format. The spectrum is drawn
in full, from −fs/2 to +fs/2, and the centre follows the header frequency.

### Controls

- **Touching or dragging on the spectrum** tunes: the horizontal axis is the current span around
  the centre.
- `-1M / -100K / +100K / +1M`: fine tuning inside the band (clamped to the band range).
- **BAND**: B1, B3, B7, B20, N78, ISM; changing band makes the centre jump to the band centre.
- **BW MS/S**: 1.92, 3.84, 7.68, 15.36; changes the FFT resolution and the span labels.
- **GAIN**: 0–45 dB in 1 dB steps. It is applied as a dB shift on the finished FFT row, so it works
  with the synthetic, file and packed-device sources without scaling or clipping samples; it changes
  the trace, not the receiver (the firmware AGC owns the real gain).
- `Esc`/`Q` close the window. On the phone it is closed from phosh.

A value set from the controls is held for **two seconds** before the block header may take over
again. If the source never delivers the request, the axis returns to the real frequency and an amber
notice says so (`TUNING IS CONTROLLED BY THE ACTIVATED PROFILE...`, or `...BLOCK HEADER` for a
file). See `docs/271-controles-de-la-app.md`.

On HiDPI screens `wl_surface.set_buffer_scale` is used with the `wl_output` scale; if phosh uses a
fractional scale and the result looks blurry, try `--scale 2` or `--scale 3`.

## Architecture

```
SyntheticSource/DeviceSource
        │  (acquisition thread, block of N samples)
        ▼
   BlockRing (SPSC, lock-free; if the consumer is slow it keeps the newest block)
        ▼
   FFT thread: fmiq::Fft (Ne10 if present; fallback today) + magnitudesDb + color_for_db
        ▼
   RowRing (SPSC) → UI thread: glTexSubImage2D of ONE row + draw
```

- The **waterfall** is a `bins × 256` ring texture. Each new row is uploaded with
  `glTexSubImage2D(..., y=head, height=1)`; the fragment shader computes which row corresponds to
  each height (`fract((head + 0.5 - v*rows) / rows)`). The whole image is never redrawn or resent
  from the CPU.
- The **spectrum** is drawn as fill + line (triangles with thickness), from the dB values of the
  last row. The UI only composes geometry and text; it does no FFT and does not colour rows.
- The **UI** uses its own 5×7 font in an R8 texture; zero font dependencies.
- The GL context lives in the UI thread. The FFT thread does not touch GL: it delivers RGB rows
  ready to use. Each row is uploaded to the texture in the drawing thread, which is the only one
  with a context.

### Synthetic source (important)

`SyntheticSource` **does not generate fs samples per second**: it produces a block of N samples at
60 blocks/s and maps the frequencies with the configured `fs`, so the FFT axis is correct without
burning the phone CPU. `DeviceSource` replaces it without touching the pipeline: its `read()`
returns blocks at the hardware pace and `BlockRing` drops the old ones if the FFT cannot keep up.

`DeviceSource` opens the `driver=furimodem_iq` module, activates the receive stream and copies the
samples as `CF32`. With a packed format (`packed5`/`packed8`) it stops pushing `setFrequency`: the
frequency comes from each block header.

## Verification performed

- Builds with **CMake and with Make** on the laptop (GCC 16.2.1, EGL 1.5, GLES 3.2 Mesa 26.2.5).
- `fft_test`: direct DFT OK, known tone OK, 4096 points at ≈0.4–0.65 ms/block
  (≈1500–2400 blocks/s, depending on laptop load).
- `waterfall_test`: 512×96, colour variety OK.
- `--headless 200`: 1024×200 PPM correct, FFT fallback 2048.
- `--gpu-test 300 --size 360x800`: real rendering on **Mesa Intel (LNL)** through offscreen EGL,
  readback and PPM; shaders, ring texture, spectrum and UI without GL errors.
- GUI on the laptop Wayland for 3 s: 181 blocks/181 rows, FFT 0.20–0.23 ms, 0 drops,
  `420×860` window at scale 1.
- `make -j1` (the mode the phone uses) builds without warnings and passes `make test`.
- **On the phone**: 480×1025 window at scale 3 on GL Mali-G68, `soapy` source, consuming real
  blocks from the ring during a sweep; ~9.6 % CPU with the sweep running and ~7 % without it.
  Screenshot capture with `grim` does not work in that build (the compositor does not allow
  copying the framebuffer), so reference frames are produced with `--gpu-test`, which uses the same
  renderer.
- **Formats**: `tools/testdata/iq5-tone.bin` and `iq8-tone.bin` give the peak at the expected bin
  with `--check-bin` (5-bit, 8-bit and an ambiguous dump), and a real 8-bit dump opens and centres
  on its header frequency.

## Known limitations

- Changing `BW` or the band does not restart any stream: the synthetic source applies it on the
  next block. With `DeviceSource` the stream will have to be restarted when `sample_rate` changes.
- `--gpu-test` uses EGL surfaceless (Mesa). If the phone EGL does not support it, use the normal
  window.
- No waterfall zoom, no channel markers and no demodulation: this is a capability demo.
- With the device, **tuning is still driven by the firmware and there is no fine tuning**: the
  receiver sweeps its plan and each block carries its frequency in the header, which is what is
  drawn on the axis. The frequency and band controls now **push the request to the module and keep
  it visible for two seconds**; if the activated profile does not follow it, the axis returns to the
  real frequency and an amber notice says so. Forcing the frequency in the image still only changes
  the block label, not the oscillator. To see a particular band you have to wait for the sweep to
  pass through it, until an image accepts host tuning.
- **Width and gain on the packed path**: `setSampleRate` is pushed and its read-back tells the app
  the real window rate (1.92 MS/s), so a refused width is reported instead of faked. The firmware
  AGC owns the receiver gain and the module does not apply `setGain`, so the GAIN control is applied
  as a dB shift on the FFT row; it changes the trace, not the radio. On the CS16 path the old
  tuning behaviour is unchanged.
