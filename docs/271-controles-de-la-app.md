# App controls: user tuning versus the block header

Why the band, ±1M/±100K, width and gain buttons of `fmiq-app` looked dead, what was changed and
what the interface now says when the receiver cannot follow a request. This is the app-side half of
the tuning limit described in `266-arquitectura-modo-sdr.md`.

## The symptom

With the device connected, pressing a band (B1/B3/B7/B20/N78/ISM), a fine-tuning button, a width or
the gain did not change the picture: the readout and the axis stayed where they were. The controls
were writing the values, but nothing visible survived the next block.

## The three layers that caused it

1. **The block header overwrote the user.** `followSourceMeta()` copied `m.center_hz` from every
   block into `ParamStore`. A packed-format source publishes 50-100 blocks/s, so a value set by a
   button was reverted 10-20 ms later. The button worked; the header erased it.
2. **The driver did not push the request.** For `packed5`/`packed8`, `DeviceSource::apply()` skipped
   `setFrequency()` (and `setSampleRate()`) on purpose, because the real frequency comes from the
   firmware. That is correct as a description of the firmware, but it left the controls with no
   route to the module at all.
3. **The firmware owns the receiver.** The activated profile fixes 2,115.0 MHz (band 1) and does not
   accept host tuning yet. That is not fixed here; it is now stated on screen instead of being
   silently ignored.

## What the app does now

### A value set by the user is held, then the block wins again

`app/src/tune_hold.hpp` keeps a `TuneHold` with a **2 s per-field hold** (`kUserHoldSeconds`) for the
centre and for the sample rate. `applyAction()` and `tuneAt()` call `noteUser()` after writing the
store, so any band, fine-tuning, width or touch-tuning change starts a hold.

While the hold is active, `followSourceMeta()` does **not** apply the header's centre/rate. If the
header arrives already carrying the requested value (the module took the command), the hold is
released immediately and no notice is raised. When the hold expires without agreement:

- the header wins again, so the axis is never left lying about where the receiver is;
- `tune_rejected` / `rate_rejected` are set, and the interface says so.

Gain is not held: no block header carries a gain.

### The interface says when a request cannot be followed

The amber notice band now has room for two independent notices (source problems and tuning
problems). When a request was ignored:

- device / `soapy` source:
  `TUNING IS CONTROLLED BY THE ACTIVATED PROFILE; RECEIVER AT 2115.000 MHZ`
- file source (blocks carry their own header):
  `TUNING COMES FROM THE BLOCK HEADER; STREAM CENTRED AT 2115.000 MHZ`
- if the width was also refused:
  `... | WIDTH FIXED AT 1.92 MS/S`

The frequency in the text is the last header value, not the request, so it stays true for a fixed
profile and for a sweep. The notice is cleared when a new request is made (while it is evaluated)
and automatically if the source later reaches the requested frequency.

This is deliberately different from **NO DEVICE**: that notice means no module was opened and the
synthetic generator is running; the tuning notice means the module is delivering data but its
frequency is not the one asked for.

### Band and fine tuning

The band buttons still set the centre to `kBands[i].center_hz` and clamp fine tuning to the band
range. With the hold, that value is now visible for two seconds (readout, band label and both ends of
the spectrum) even when the firmware ignores it. Afterwards the axis returns to the header value and
the amber notice explains why.

### Width

`setSampleRate()` is now pushed for packed formats too. The driver reports the real window rate
(1.92 MS/s) from `getSampleRate(RX)`, so the app compares request and read-back: if they differ, the
width is held for 2 s for the user to see it, then the axis returns to 1.92 MS/s and the notice adds
`WIDTH FIXED AT 1.92 MS/S`.

### Gain

The packed path has no receiver gain the host can move: the firmware AGC owns it and the module
does not apply `setGain()` to the delivered samples. `setGain()` is still pushed so a module that
implements it can use it, and the control is made visible by applying the gain **once** as a dB
shift on the finished FFT row (`pipeline.hpp`), referenced to the 12 dB default so the picture does
not jump on start. Scaling the samples was rejected: it counts the gain twice if the pipeline also
shifts, and clipping at full scale creates in-band spurs.

The shift is done on the row, not on the receiver: the GAIN control changes the trace and the
waterfall, not the radio. Synthetic signals get the same shift (their generator no longer applies a
second one), so all three sources behave alike.

## The driver side

`DeviceSource::apply()` no longer skips `setFrequency()`/`setSampleRate()` when the format is packed.
The request is pushed and the read-back is the verdict:

- `getFrequency(RX)` returns the last block header while the module follows the block. If it returns
  something other than the request, the app knows the command was ignored.
- `getSampleRate(RX)` returns the real packed rate, so the same comparison works for the width.

Values that the UI copied *from* the header are not pushed back: the store is written every block,
and re-commanding the frequency the firmware is already delivering would be noise (one
`setFrequency` per block during a sweep).

The old CS16 format is untouched: with no block header, `follows_freq` is false, `getFrequency()`
returns the requested value and the controls keep behaving exactly as before.

## Verification without the phone

```sh
make -C app test                 # unit tests + a 48-frame headless run
tools/test-iq5file.sh            # 5-bit, 8-bit, old signature and CS16 tone files
```

`tune_hold_test` drives the exact reported case with the recorded profile (2,115.0 MHz, 1.92 MS/s):

- a band or +1M value survives headers during the hold;
- after the hold the header wins and the rejection flag is raised;
- a source that follows the request releases the hold without a notice;
- a fixed width is held, then reported;
- without a `TuneHold` the old "header always wins" behaviour is preserved (headless/gpu-test);
- a new request clears the previous verdict, and a sweep that reaches the request clears the notice.

The two-notice layout can be rendered offscreen, without a device:

```sh
app/fmiq-app --gpu-test 8 --size 420x860 --source file --file tools/testdata/iq5-tone.bin \
  --notice "NO DEVICE: ..." \
  --notice2 "TUNING IS CONTROLLED BY THE ACTIVATED PROFILE; RECEIVER AT 2115.000 MHZ"
```

## What is still missing

The firmware still does not take the frequency from the host. Real tuning needs the band and
frequency written into the scheduler's buffer **before** the radio database is built, as measured in
`266-arquitectura-modo-sdr.md`. When an image does that, no app change is needed: the header will
carry the requested value, the hold will be released on the first matching block and the notice will
not appear.
