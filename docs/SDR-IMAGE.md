# The SDR mode image, built from your own phone

This repository does **not** distribute a modem firmware image: the receiver needs a patched
copy of the image your phone already has, so the patch is applied on your machine and both
ends are verified by sha256. `tools/make-sdr-image.py` does exactly that.

[Versión en castellano](SDR-IMAGE.es.md)

## What you need

The stock modem image of the FLX1, E6 build:

| | |
|---|---|
| size | 63,018,064 bytes |
| sha256 | `22c1909dbf9f0455c1a93f8a8e0a141245cc9b2d979c44d0c8da75f5a3424896` |

It is the image in the phone's firmware partition (`/dev/disk/by-partlabel/md1img_a`), also
present in the vendor's modem package and in any full backup of the phone. `make-sdr-image.py`
**refuses any other image**, so a different firmware version cannot be patched by accident.

Dump it from your own phone and check the hash before going on (the partition is larger than the
image, so the size is explicit):

```sh
ssh furios@<phone> 'sudo head -c 63018064 /dev/disk/by-partlabel/md1img_a' > stock-modem.img
sha256sum stock-modem.img
# 22c1909dbf9f0455c1a93f8a8e0a141245cc9b2d979c44d0c8da75f5a3424896  stock-modem.img
```

## Build it

```sh
python3 tools/make-sdr-image.py /path/to/stock-modem.img
# stock image : /path/to/stock-modem.img (63018064 bytes)
# sha256      : 22c1909d…  OK
# SDR image   : modem-sdr-8bit.img (63018064 bytes)
# sha256      : 5a625327…  OK (profile sdr-8bit)
```

`--check` verifies the stock image and reports the result without writing anything. The output
is byte-identical to the `sdr-8bit` image `sdr-load` knows, and the script checks that its
sha256 is `5a625327a06fcacf12f611419437bc85aa54c339911a8d3427262fea0643b4d3` before handing it
over.

The patch is small and explicit: **1,694 bytes in seven runs** (the tool stores only the
ranges that change). They carry:

- the copy of the scanner's capture to the AP ring, so the waterfall has data while the phone
  sweeps;
- the 8-bit-per-component format (I/Q, origin 3), three more bits than the 5-bit tap;
- the fixed tuning at 2,115.0 MHz (band 1) for the sweep.

The modem **stays in service** with this image on purpose: the capture comes from its own
frequency scan. That is also why the receiver delivers bursts, not a continuous stream.

## Load, use, go back

The loader runs `flash_modem.sh` (published in `tools/`); copy it once next to the image:

```sh
scp modem-sdr-8bit.img tools/flash_modem.sh furios@<phone>:/home/furios/
ssh furios@<phone> chmod +x /home/furios/flash_modem.sh

sudo sdr-load --image /home/furios/modem-sdr-8bit.img   # writes and verifies; it does NOT reboot
sudo systemctl reboot                                   # the bootloader reads the new image
sudo sdr-load --resume                                  # after the boot: enters the mode and activates
sudo sdr-load --status                                  # profile, ring and next step
```

`--image` disables WiFi and Bluetooth with `rfkill` before writing and exits with **code 6** when
the write succeeds (reboot needed). Do not run it over the phone's WiFi SSH connection; use the
USB/RNDIS address or a terminal on the phone.

`--resume` already runs the activation (for `sdr-8bit`, it launches the phone's scan). Running
`--activate` afterwards is optional and only needed to launch another sweep.

From the phone's desktop the same steps are the numbered buttons of `furimodem-sdr`
(*1. Load SDR image…*, *2. Start reception*, *Open the SDR app*); the window does not reboot by
itself: it shows the `sudo systemctl reboot` command when the image has been written.

To go back to a normal phone:

```sh
sudo sdr-load --deactivate                              # stops the sweep (the firmware stays loaded)
sudo sdr-load --image /home/furios/modem-E6.img         # writes stock again
sudo systemctl reboot
# or, without writing anything: sdr-load --restore leaves the mode and gives the radios back
# (it only reflashes the recovery image if the modem does not come back)
```

## Why not ship the image

The image is derived from vendor firmware that is not ours to redistribute. A patch recipe of
1.7 kB applied to the user's own copy is enough to reproduce it exactly, and the sha256 pair
makes the result verifiable: the stock one going in, the `sdr-8bit` one coming out.
