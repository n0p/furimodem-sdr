# Demo script

How to show the project in five minutes, with what really works today and without promising what is
not there yet. Steps 1-4 are verified; step 5 is the part that has to be explained carefully,
because the receiver already delivers signal but with two limits.

## Before you start

- Phone running FuriOS with `ssh furios@<phone>` access.
- The stock image at hand to go back.
- **The activation writes the firmware partition and the phone restarts**: any call in progress is
  dropped and mobile service is gone until you restore to stock. Announce it before doing it.
- Optional: a generator or a B200mini connected to the laptop, one metre from the phone, with an
  attenuator or a load. A known CW is the most convincing proof that what is painted is radio.

## 1. What is installed

```sh
ssh furios@<phone>
ls -la /usr/lib/aarch64-linux-gnu/SoapySDR/modules0.8/libfurimodemIQ.so
SoapySDRUtil --info | grep -A2 furimodem          # the module shows up
sdr-load --status                                  # image, PLL, rails, loops, ring
```

This shows that the Soapy module is installed and that `sdr-load` knows the phone's state.

## 2. The app, with data from the modem itself

```sh
fmiq-app --headless 60 --source soapy --out /tmp/demo.ppm
```

It must report `variedad=si` (*variety=yes*): **each waterfall row is different**, meaning there is
live data moving through the ring. This proves that the modem → ring → driver → app path works end
to end.

The windowed version, to show on screen:

```sh
fmiq-app            # Wayland + EGL: live waterfall with the controls
```

With the app open, launch the sweep from another session and the waterfall moves on its own: the
frequency in each block's header changes and the axis follows it.

```sh
sudo busctl --timeout=30 call org.ofono /ril_0 org.ofono.FuriLabs.AT SendCommand s "AT+CFUN=1"
sudo busctl --timeout=300 call org.ofono /ril_0 org.ofono.FuriLabs.AT SendCommand s "AT+COPS=?"
```

## 3. The loader in action

```sh
sdr-load --modes                            # mode table of the known profiles
sdr-load --activate --dry-run --profile standard-a   # the 9 steps, without touching anything
```

`--activate` **refuses if the partition image is not in its profile table** (code 13): that refusal
is part of the demo, because it shows the tool does not request modes blindly. The table holds the
images that dispatch modes (`standard-a`, `standard-b`) and the current `sdr-8bit`, which is
**passive**: it captures during the sweep and has nothing to activate, so it also refuses with 13.

## 4. Going back

```sh
sdr-load --deactivate     # stops the loops (the rails are NOT switched off in software, on purpose)
sdr-load --restore        # stock firmware, verified by sha256
```

## 5. The receiver's state, stated plainly

The short answer to "so, can you see anything?" is **yes: you see the noise and the signals of the
band the sweep passes through, as I/Q, not demodulated**. And the proof can be shown:

```sh
# on the laptop, a CW generator at +200 kHz from one of the plan's frequencies
# (any generator will do; with a B200mini, a 250 kS/s tone at 0 dB and one metre away)
# on the phone, dump the ring while it sweeps and open it afterwards
sudo ./iqring_pull 20 /tmp/iqring.bin
./fmiq-app --headless 4 --fft 2048 --source file --file iqring.bin --out /tmp/cap.ppm
```

The tone appears in the bin it belongs to (measured: +199.7 kHz, 38.1 dB peak/median in 5-bit; 9/9
captures in 8-bit). What to say next, without embellishment:

- **The waterfall paints real I/Q from the modem**, with the frequency of each burst on the axis.
  Verified on the phone screen during a sweep: 213 different frequencies in 50 s.
- **8-bit capture works** (three more bits of resolution), but that tap arrives on the order of
  26 dB weaker than the 5-bit one because it does not go through the automatic gain control. It is
  more resolution, not more sensitivity.
- **There is no tuning**: the receiver sweeps its plan and the interface controls do not move the
  oscillator. Forcing the frequency in the image only changes the label; measured, the tone
  disappears.
- **There is no continuous stream**: captures are tied to the sweep.

The full map, in terms of blocks and measurements, is in `docs/266-arquitectura-modo-sdr.md`.

## What NOT to say in a demo

- That the modem demodulates or provides service. No: it delivers I/Q in bursts and the modem's radio
  is switched off on purpose.
- That the driver can transmit. It cannot, by design, and it is blocked with three interlocks.
- That a custom firmware image has to be flashed. No: the patch is generated on the user's machine
  from their stock image and verified by sha256.
