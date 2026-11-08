# Demo script

How to show the project in five minutes, with what really works today and without promising what is
not there yet. Steps 1-4 are verified; step 5 is the part that has to be explained carefully,
because the receiver already delivers signal but with two limits.

## Before you start

- Phone running FuriOS with `ssh furios@<phone>` access.
- **The SDR image already loaded** (`sdr-8bit`). If the phone is on stock, do step 0 first.
- The stock image at hand to go back (`/home/furios/modem-E6.img`).
- **Writing the image writes the firmware partition and needs a reboot**: any call in progress is
  dropped. With `sdr-8bit` the modem stays in service (the capture comes from its scan); with the
  laboratory mode images it leaves service. Announce it before doing it.
- Optional: a generator or an SDR connected to the laptop, one metre from the phone, with an
  attenuator or a load. A known CW is the most convincing proof that what is painted is radio.

## 0. The image (skip if `sdr-8bit` is already loaded)

```sh
sudo sdr-load --status        # image kind must be "mode image sdr-8bit"
# if it says stock: generate it from YOUR stock image and write it (README, step 3-4):
#   python3 tools/make-sdr-image.py stock-modem.img
#   sudo sdr-load --image /home/furios/modem-sdr-8bit.img
#   sudo systemctl reboot
#   sudo sdr-load --resume
```

## 1. What is installed

```sh
ssh furios@<phone>
SoapySDRUtil --info | grep -A2 furimodem          # the module shows up (path depends on the distro)
sudo sdr-load --status                             # image, profile, ring heartbeat
```

This shows that the Soapy module is installed and that `sdr-load` knows the phone's state.

## 2. The app, with data from the modem itself

```sh
fmiq-app --headless 60 --source soapy --out /tmp/demo.ppm
```

It must report `variety=yes`: **each waterfall row is different**, meaning there is
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
# do not interrupt that call (no Ctrl-C, no killing busctl): an interrupted scan leaves the modem
# in `exception` and it has to be recovered by flashing the image again and rebooting
```

## 3. The loader in action

```sh
sudo sdr-load --modes                            # mode table of the known profiles
sudo sdr-load --activate --dry-run --profile sdr-8bit    # the sweep plan, without touching anything
sudo sdr-load --activate --partition /nonexistent         # code 13: nothing is requested blindly
```

`--dry-run` prints a plan **even if the image cannot be identified** (it says so and uses the
first profile), because it touches nothing. Without `--dry-run`, `--activate` **refuses an image
that is not in its profile table**: a **readable** stock partition gives code **5**
(sha256/profile mismatch) and an unidentifiable partition gives **13**. The table holds the
laboratory mode images (`standard-a`, `standard-b`, not distributed) and `sdr-8bit`, which is a
**sweep image**: `--activate` asks it for no modes, it launches the phone's own scan and checks
that the ring publishes.

## 4. Going back

```sh
sudo sdr-load --deactivate                       # stops the sweep (the firmware stays loaded)
sudo sdr-load --image /home/furios/modem-E6.img  # writes the stock image again
sudo systemctl reboot
# sdr-load --restore also leaves the mode and gives the radios back, but it only reflashes the
# recovery image if the modem does not come back: it does NOT put stock back by itself.
```

## 5. The receiver's state, stated plainly

The short answer to "so, can you see anything?" is **yes: you see the noise and the signals of the
band the sweep passes through, as I/Q, not demodulated**. And the proof can be shown:

```sh
# on the laptop, a CW generator at +200 kHz from one of the plan's frequencies
# (any generator will do; with a B200mini, a 250 kS/s tone at 0 dB and one metre away)
# on the phone, dump the ring while it sweeps and open it afterwards
sudo iqring_pull 20 /tmp/iqring.bin
fmiq-app --headless 4 --fft 2048 --source file --file /tmp/iqring.bin --out /tmp/cap.ppm
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

The image, its patch and the measurements are described in `docs/SDR-IMAGE.md`.

## What NOT to say in a demo

- That the modem demodulates or provides service. No: it delivers I/Q in bursts. With `sdr-8bit`
  the modem is still a phone on purpose (its scan feeds the ring); with the mode images it is out
  of service.
- That this build can transmit: it is receive-only, with no transmit path.
- That a custom firmware image has to be flashed. No: the patch is generated on the user's machine
  from their stock image and verified by sha256.
