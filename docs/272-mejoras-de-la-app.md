# App review: five fixes

The external review of `fmiq-app` measured three defects and asked for two changes in how the app
presents and scales the signal. This document says what was wrong, what was changed, how each fix
is measured and what is still to be wired into the interface.

| # | Fix | Where it lives | State |
|---|---|---|---|
| 1 | Waterfall keeps every bin (MAX per group) and the curve has peak hold | `pipeline.hpp`, `render_gl.cpp`, `waterfall.hpp` | done, measured |
| 2 | Age of the last real capture instead of the "D" counter | `capture_state.hpp`, `Pipeline::captureState()` | logic done and tested, read-out pending (see patch) |
| 3 | Three different notices for the three ways of having no samples | `driver/SoapyFurimodemIQ.cpp`, `driver/iq_window.cpp`, `ring_state.hpp` | driver done and tested, read-out pending |
| 4 | Legible on a 480x1025 phone: scale 2 and 44 px buttons | `main.cpp` | prepared and rendered, pending (see patch) |
| 5 | Gain as a dB shift on the FFT row | `pipeline.hpp`, `waterfall.hpp`, `source.hpp` | done, measured |

## 1. The waterfall no longer skips bins

### What was wrong

The texture was 2048 columns wide (one per FFT bin) and was drawn into the 456 logical px of the
waterfall with `GL_LINEAR` and no mipmaps. Minifying 4.5:1 samples two texels out of every four or
five, so a carrier one or two bins wide was picked in some rows and missed in most of them. The
spectrum curve had the same problem in another form: it drew one of every two bins
(`step = n / (r.w * 2)`).

Measured with `tools/testdata/iq8-tone.bin` (tone at fs/8, FFT 2048), 154 rows, sampling the
resulting texture the way the GPU does it:

```
before (2048 cols, GL_LINEAR to 456 px): 16/154 rows keep the carrier at full colour,
                                         135/154 drop it to the noise colour, mean red 82/255
after  (MAX per group, 456 cols):        154/154 rows at full colour, mean red 233/255
```

### What was changed

- `reduce_max()` in `waterfall.hpp` collapses a row of `n` bins into `cols` columns keeping the
  **maximum** of each group. A group that holds the carrier always carries it, and minifying a run
  of equal values cannot dilute it.
- `Pipeline::fftLoop()` does that reduction **in the FFT thread**, together with the colour mapping,
  before the row is published. `Pipeline::bins()` is now the width of the row (456 by default, the
  waterfall of a 480 px screen) and `setRowBins()` lets the app match the real panel width when the
  window is not 480 px. Rows are still buffered at FFT width in the ring, so changing the width
  before `start()` costs nothing.
- `Painter::pushRow()` sizes the circular texture from the first row it receives instead of keeping
  a 2048-column map, so the texture is drawn 1:1.
- `Painter::spectrum()` collapses with MAX when it is given more points than pixels (instead of
  decimating) and draws a **peak hold** line at 24 dB/s over the trace (`--gpu-test` still passes
  the whole FFT row, and now it looks like the app).

### Verification

`make -C app test` runs `review_test`, which reproduces the defect with the real block files and
prints the two lines above; it fails if any row loses the carrier. It also starts a real `Pipeline`
with the block file and the app's row width (threads and both rings included) and checks that the
row arrives with 456 columns and the carrier column at full colour (red 222). An offscreen frame at
480x1025 (`--gpu-test --source file --file tools/testdata/iq8-tone.bin`) shows a continuous vertical
line at 62.5 % of the waterfall: 324 of 324 textured rows have red >= 228 in that column. The same
frame from the unpatched app (whose `--gpu-test` still feeds 2048-wide rows to the texture) has a
mean red of 11 in that column and only 1.4 % of the rows above 150: that is the defect, in a frame.

## 2. Live, repeated or nothing

`SourceMeta.seq` was already there and unused. `capture_state.hpp` turns it into an age:

- `LIVE 0.1S #8123` - a new `seq` arrived within 0.5 s: the receiver is publishing now.
- `REPEAT 3.0S #8123` - samples keep arriving but `seq` does not advance (the module is repeating
  the last capture, `repeat_last`, or a recording is looping).
- `NO DATA 5.1S` - no block for more than 1 s.
- `GEN 0.0S` - the source has no block header (synthetic): every block is generated on the spot.

`Pipeline::captureState()` reads the tracker from the UI thread with atomics; `is_new_capture()`
decides whether a block is new (a new `seq`, or the block counter when the header leaves `seq` at
0), and the decision is unit-tested with explicit timestamps.

This replaces the `D` counter of dropped blocks in the status bar. The counter is still in
`Stats`/`app.log`; it grew by design and never said whether the picture was old.

## 3. Three notices, not one

Every failure used to end as `SOAPY_SDR_TIMEOUT`, so the app could only say "the ring is not
publishing". The module now answers `readSetting("rx_ring")` with one token (plus `: detail` for
the log):

| token | meaning |
|---|---|
| `ok` | mapped and delivering blocks with a known signature |
| `no-device` | the memory window does not exist (patched image not loaded?) |
| `no-permission` | it exists but this user cannot open it |
| `no-map` / `no-layout` | open() worked but the window cannot be mapped / the ring does not fit |
| `no-blocks` | mapped, but nothing has been published: the firmware is not capturing |
| `no-format` | blocks arrive with a signature the driver does not know |
| `not-opened` | `setupStream(RX)` has not run yet |

`IqWindow::probe()` inspects the ring read-only (a slot with `bytes == 0` is "nothing yet", not a
format error). `ring_state.hpp` maps the token to one of the three messages the amber band shows:

- `RX RING NOT MAPPED: THE MEMORY WINDOW IS MISSING (PATCHED IMAGE?)` (and the other three open
  failures),
- `RX RING MAPPED BUT NOTHING IS PUBLISHED: NO CAPTURE`,
- `BLOCK SIGNATURE UNKNOWN: IMAGE AND DRIVER DISAGREE`.

They are short on purpose: the band is two lines at scale 2, about 38 characters per line. The
detail (node path, errno, signature) goes to `app.log`, not to the screen.

`driver/selftest_ring_state.cpp` builds a real `SoapyFurimodemIQ` over a simulated ring file and
checks all five states (`not-opened`, `no-device` with the node in the detail, `no-blocks`, `ok`,
`no-format` with the signature). `ctest` in `driver/` runs it.

## 4. Readable on the phone

The 5x7 font was drawn at scale 1 (7 logical px) for the amber notice and every axis label, and the
buttons were 30-36 px tall. On a 480x1025 screen that is unreadable with dirty hands.

The prepared change (see `272-main-cpp.patch`):

- status bar 44 px, two rows at scale 2 (source and capture age are no longer the small print);
- amber band 42 px per notice, two lines at scale 2, wrapped with `textWidth(..., 2)`;
- dB labels, frequency ends, `BAND`/`BW MS/S`/`GAIN` and the hint at scale 2;
- buttons 46 px (frequency) and 44 px (band, width, gain, which also widens +/- to 76 px);
- control panel 290 px; the waterfall gives up ~90 px with a notice on screen and still has ~415 px
  on the phone (the review said there were about 500 px of margin to give up).

Verified by rendering `--gpu-test 200 --size 480x1025` and reading the frame: no text cut with
`...`, no label outside its button, and all of it legible at 480 px wide.

## 5. Gain as a dB shift

The control used to scale samples and clamp them to [-1, 1]. That counts the gain twice if anything
else also applies it, and clipping a strong signal creates in-band spurs exactly when the control
goes up; it also could not lift the 8-bit path, which arrives about 26 dB below the 5-bit one
(`266-arquitectura-modo-sdr.md`), without crushing it against the bottom of the scale.

Now `apply_db_shift()` adds `gain_db - kGainReferenceDb` dB to the row already computed by the FFT,
in the FFT thread: no per-sample cost, no saturation, and the whole trace (noise floor included)
moves as one. The reference is the 12 dB default, so the picture does not jump on start.
`review_test` checks that a 28 dB shift moves every bin by exactly 28 dB (worst deviation
0.00e+00). The sample-domain scaling was removed from `source.hpp` (synthetic generator included);
`DeviceSource` no longer applies it either.

## Verification (commands)

```
make -C app                 # builds fmiq-app + tests
make -C app test            # fft, waterfall, tune_hold, review_test, headless
./review_test               # the table of section 1, gain, capture age and ring messages
fmiq-app --headless 32 --fft 2048 --source file --file ../tools/testdata/iq8-tone.bin --check-bin 1280
fmiq-app --gpu-test 200 --size 480x1025 --source file --file ../tools/testdata/iq8-tone.bin --out f.ppm
cmake -S driver -B build && cmake --build build && ctest --test-dir build
```

## What is still to wire up

`main.cpp` and `source_device.cpp` were being edited by another agent while this was written, so
they are untouched here. `docs/272-main-cpp.patch` is the exact change (it applies cleanly to the
tree as of this writing) and does four things:

1. `pipe.setRowBins(win.width() - 24)` before `start()`: the texture matches the panel exactly.
2. draws `capture_label(pipe.captureState())` in the status bar where `D%llu` was (colour: green
   live, amber repeated, red stopped) and drops `SceneModel::dropped` from the text.
3. keeps `pick.src->setting("rx_ring")` read once at start and uses `ring_notice()` when no row
   arrives in 3 s; the detail goes to `app.log`.
4. the readability numbers of section 4, and makes `--gpu-test` reduce its rows so the offscreen
   frame matches the live pipeline.

The patch also adds `DeviceSource::setting()` (`source.hpp` declaration and `source_device.cpp`
implementation, three lines each) - without it the app has no way to read `rx_ring` from the
module. `SampleSource::setting()` already exists in `source.hpp` with an empty default.
