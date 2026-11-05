# tools — device utilities

Two programs, with no dependencies beyond POSIX and libstdc++ (no external
libraries, no Python on the phone):

| program | what for |
|---|---|
| `sdr-load` | **full** SDR mode load: turns every radio off, flashes and verifies the image, checks that the mode is active, **activates it** (PLL, rails and loops, step by step) and reverts (with recovery and reboot if needed) |
| `iqmode` | minimal version: `--check`, `--enter` (ioctl) and `--exit`; no flashing and no state |

`sdr-load` is the one to use. `iqmode` stays around as a diagnostic aid.

## Building

```sh
make -C tools                              # native (development bench)
make -C tools CXX=aarch64-linux-gnu-g++    # cross, if the toolchain is installed
make -C tools cross                        # cross in a debian:trixie container -> dist/
make -C tools check                        # self-test without a device (68 checks)
```

`make cross` uses the same pattern as continuous integration: `debian:trixie` image,
`crossbuild-essential-arm64`, and leaves the aarch64 binaries in `dist/`.

On the phone:

```sh
scp dist/sdr-load furios@<phone>:/tmp/
ssh furios@<phone> 'sudo -n /tmp/sdr-load --check'
```

**root** is required for the ioctls, `rfkill` and flashing. `--check`, `--status`, `--modes`,
`--selftest` and `--dry-run` do not need root (unless reading the partition is requested).

---

# sdr-load

## What it does, in order

1. **Turns every radio off.** WiFi and Bluetooth through `rfkill` (writing
   `/sys/class/rfkill/*/soft`, because the `rfkill` binary **is not installed on the
   phone**) and the modem through the enter-SDR-mode ioctl on the control node. Then it
   checks that **no `ccmni*` or `wlan*` interface is left up** by reading `IFF_UP` in
   `/sys/class/net/*/flags`.
2. **Loads the mode.** Either it flashes an image (`--image`, calling the phone's flash
   script, without reimplementing it) or takes the one already there as good (`--no-flash`,
   with an optional `--sha256`). After flashing, the **partition is read back and its
   sha256 compared** with the image one.
3. **Verifies.** It samples the three link ring areas twice and requires **zero changed
   words**; it reads the **dump engine mark** in the working window and counts the data
   words. If something is missing, it says so and exits with its code.
4. **Activates** with `--activate`: waits for the modem, arms the dump, seeds the PLL,
   powers the rails and starts both control loops, **verifying every step** against the
   firmware markers. `--deactivate` stops the loops (the rails are **not** powered down in
   software).
5. **Reverts** with `--restore`: clean exit from SDR mode, `rfkill unblock` of whatever was
   blocked, ofono up again and, if the modem does not come back, **recovery image and
   reboot** as a last resort.

It never emits RF: it only blocks radios, writes the firmware partition and reads the
working window.

## Usage

```
sdr-load                          # read-only report (= --check)
sdr-load --image <path>           # flash, verify sha256 and ask for a reboot
sdr-load --no-flash               # enter SDR mode with the image already loaded
sdr-load --activate               # activate: PLL, rails and loops (verified step by step)
sdr-load --resume                 # finish what a reboot interrupted; never writes the image
sdr-load --deactivate             # stop the loops; the rails stay powered
sdr-load --restore                # go back (clean -> recovery -> reboot)
sdr-load --status                 # persisted state + what is seen now
sdr-load --modes [--profile P]    # mode table -> meaning for that image
sdr-load --verify-boot            # close or diagnose after a reboot
sdr-load --selftest               # tests without a device
```

### Full cycle

```sh
# 1) new image (blocks radios, writes, verifies and reboots if you ask for it)
sudo sdr-load --image /home/furios/modem-sdr-standard-a.img
sudo systemctl reboot

# 2) after boot: check the boot, enter SDR mode and activate, in one go
sudo sdr-load --resume

# 3) back to the normal phone
sudo sdr-load --deactivate        # optional: stop the loops first
sudo sdr-load --restore
```

`--resume` reads the state the flash left behind (`pending=reboot-to-enter`), hashes the
image bytes in the partition and compares them with the sha256 recorded at flash time. Only
if they are the same does it close the pending flag, enter SDR mode and run the activation
sequence: **the partition is not written again**. The same step by step, if you prefer:
`--verify-boot`, `--no-flash --sha256 <hex> --size <bytes>` and `--activate`.

### If you already know which image is running

`--no-flash --sha256 <hex> --size <bytes>` hashes **only the image bytes** inside the
partition and compares them. The size comes from `--size`, or from the one stored when
flashing; without either of them the program refuses (hashing the whole partition would give
a different hash, because the partition is larger than the image).

## SDR mode activation (`--activate`)

Entering SDR mode (`--no-flash`) leaves the modem out of service, but **does not power the
receiver up**. That is what `--activate` does: a sequence of modes from the patched firmware,
with a check after every step. If a step is not verified, it **is undone** (the loops are
stopped) and it exits with code 14. `--activate` enters the mode first when the modem is not
there yet (the `--no-flash` steps), so it is safe to repeat: after a reboot it finishes the
job without writing the partition.

| step | what it asks for | what is required to take it as good |
|---|---|---|
| 1 | nothing: reads the sha256 of the image bytes in the partition | that sha256 must be in the profile table (otherwise it **refuses**: code 13) |
| 2 | nothing: `getprop vendor.mtk.md1.status`, with retries | `ready` before `--md1-timeout-ms` (60 s; the modem takes >20 s to answer) |
| 3 | write mode (6): writes the dump engine arming mark | the readback returns the same value (dump armed; the rails are still off) |
| 4 | PLL seed (3) | the channel table ends at channel 4, band 77, 4,070,010 kHz |
| 5 | rails (27, 28, 29), one at a time | the rail's `CON0` becomes `0x1` and the guard does not trip; the modem stays `ready` |
| 6 | rail keepalive (30), period 100 | the loop arm marker holds `KPL1`, with at least one pass and period 100 |
| 7 | timing table (32) | the arm marker holds `KPL2`, with at least one pass, and both table timings end at 256/656 and 256 ×3 (read through mode 5; if it does not match, the loop is re-armed once) |
| 8 | nothing | `seq` and the ring copy counter advance within `--beat-ms`; the PLL and the three `CON0` stay the same |
| 9 | nothing: state | `sdr-active`, `loop_armed=true`, `rails_on=true` |

**Golden rule:** `CON0` of VRF18/VFE28/VRFCK is only ever written with `|= 1`, **never 0**:
writing 0 takes the modem down. That is why `--deactivate` **does not** power the rails down:
it stops both loops (mode 31, which does not touch the PMIC) and says so. Powering them down
is a job for the modem restart or the power supply.

### The mode table goes by image, not in the code

The same mode number does different things in each image: the patched firmware dispatches it.
That is why the sequence is resolved from a **profile** chosen by the image sha256:

| profile | image | modes it uses |
|---|---|---|
| `standard-a` | `modem-sdr-standard-a.img` (`f2c84f9f…022e3`) | 3 PLL, 27/28/29 rails, 30 keepalive, 31 stop, 32 table |
| `standard-b` | `modem-sdr-standard-b.img` (`93ad9291…6352`) | standard-a's + 33/34 (descriptor window) |
| `sdr-8bit` | `modem-sdr-8bit.img` (`aaf7a622…6cc53a`) | none: **passive image** (8 bits per component, origin 3, no frequency cave) |

`sdr-load --modes` prints the full table of every known profile and `--modes --profile
standard-a` the one of a given image. If the partition image is not in the table,
`--activate` **refuses** (code 13) and lists the known profiles: modes are never requested
blindly.

A **passive image** dispatches no modes: its modem copies the capture the scanner already
makes, without the AP writing anything. It is loaded with `--image` (or `--no-flash` if it is
already there), the sweep is started and the ring is read; `--activate` and `--deactivate`
refuse with code 13 before touching anything. In a profile file it is marked with
`passive = 1`.

For a new image, a profile file (`key = value`, `#` to comment) and `--profile`:

```ini
name = dev-profile
sha256 = 0123…cdef        # optional; if present, it must match the partition
note = standard-b + mode 35
cave_min = 13             # first mode the cave dispatches
cave_max = 35             # mode ceiling of the image
pll_seed = 3
rail_v18 = 27
rail_v28 = 28
rail_vck = 29
keepalive = 30
loop_stop = 31
timing = 32
peek_mode = 5
poke_mode = 6
arm_addr = 0x00000000     # arming mark address: defined by the image (0 = not declared)
arm_value = 0x100
pll_channel = 4
pll_band = 77
pll_khz = 4070010
loop_period = 100
cdf0 = 256
cdf1 = 656
bsi = 256
mode = 3: seeds the PLL and queues RX ON
mode = 31: stops both loops
```

The file is **strict**: an unknown key is an error with its line number (a typo must not
activate the wrong modes). Without the sequence keys (`pll_seed`, `rail_*`, `keepalive`,
`loop_stop`, `timing`, `peek_mode`, `poke_mode`) it does not activate, unless the profile
declares `passive = 1` (an image that captures on its own and has no modes to ask for).

### `--deactivate`

It stops both loops with the profile stop mode (31 in standard-a/standard-b), requires both
arm markers to be zero, and records in the state (`loop_armed=false`) and on screen that **the
rails stay powered**. It does not leave SDR mode: that is what `--restore` is for.

### What it adds to `--status`

An `SDR mode` block with the partition image and its sha256, the profile it gets, the PLL, the
three `CON0`, both loops (`ARMED`/`stopped`, passes, writes, negatives) and whether the ring
advances (`seq` and copies within `--beat-ms`). It is still read-only.

## States and state file

Every destructive step is written **before** it runs into `/var/lib/sdr-load/state.json`.
That file is the only state: root writes it (`0644`) and an unprivileged `--status` reads the
same one, so what the window shows is what the tool recorded. `--state <path>` and the
`SDR_LOAD_STATE` variable change it for a test bench.

`--status` distinguishes the three cases and never invents a state:

| what it finds | what it prints |
|---|---|
| readable state file | `state`, `pending`, image, sha256, radios, profile, loops, rails and the next step |
| no file at all | `state : none (no state file at /var/lib/sdr-load/state.json)` |
| a file it cannot read | `state : unreadable (Permission denied)` and a `next` line saying to run it with privileges |

Older versions kept a second copy in `~/.local/state/sdr-load/state.json` when `/var/lib` was
not writable, which made the window report "factory image" while the phone was in SDR mode.
That path is now **only read** when the canonical file does not exist, `--status` names it in a
`legacy` line, and the next privileged write creates the canonical file, which wins from then
on: nothing has to be migrated by hand.

```json
{
  "version": 1,
  "tool": "sdr-load 1.0",
  "state": "sdr-active",
  "pending": "",
  "updated": "2026-11-01T01:20:00Z",
  "image": "/home/furios/modem-cellcfg.img",
  "image_sha256": "db29dd7a…",
  "image_size": 63018064,
  "partition": "/dev/disk/by-partlabel/…",
  "flash_script": "/home/furios/flash_modem.sh",
  "wifi_blocked_by_tool": true,
  "bt_blocked_by_tool": true,
  "mode_profile": "standard-a",
  "loop_armed": true,
  "rails_on": true,
  "last_error": "",
  "boot_id": "…"
}
```

`state` is `ok`, `entering`, `sdr-active`, `flashing`, `flash-failed`, `activating`,
`activate-failed`, `restoring`, `recovering` or `restore-failed`. `pending` is
`reboot-to-enter`, `reboot-to-recover`, `exit-mode`, `unblock-radios` or empty.
`mode_profile`, `loop_armed` and `rails_on` are the fingerprint of the last
`--activate`/`--deactivate`.

The write is atomic (`state.json.tmp` + `fsync` + `rename`).

## Resuming after a reboot (`--resume`)

A flash ends with `pending=reboot-to-enter`. `--resume` (and `--activate`, which is
idempotent) completes that activation instead of leaving it half done:

1. if the reboot has not happened yet, it stops with code 6 and asks for it;
2. it hashes the image bytes in the partition and compares them with the sha256 and size the
   state recorded at flash time — a different image stops it with code 5 and the pending flag
   is kept;
3. if they match, it closes the pending flag and says on screen that the image is **not**
   written again;
4. it enters SDR mode if the modem is not there yet (the same steps as `--no-flash`: block the
   radios, check the interfaces, ioctl `0x430E` and require quiet rings) and runs the
   activation sequence.

It is also the way back from a half-done activation (`state=activating` or `activate-failed`):
it finishes the sequence from the beginning, and every step is verified as usual. A pending
revert (`reboot-to-recover`) is delegated to `--verify-boot`, which is the diagnosis.

## Staged revert (`--restore`)

| level | what it tries | result |
|---|---|---|
| 1 | SDR mode exit ioctl, `rfkill unblock` of what we blocked, `systemctl restart ofono` and wait for the modem (`/ril_0` object in ofono + `AT`) | `ok`, code 0 |
| 2 | reflashes a good image (`--recover-image`, `/home/furios/modem-E6.img` by default) with the flash script and verifies the partition sha256 | if the modem already answers, `ok`; if not, a reboot is needed |
| 3 | reboots the phone as a last resort, announcing it | code 11 (or 10 if the reboot was not confirmed) |

The modem is considered alive if it **moves its rings** (a hardware sign: the stack is
running) or if it **answers `AT`** through ofono. If it cannot be observed (no working window
and no `busctl`), the program **does not touch the firmware**: it leaves the state as it was
and exits with 9.

The level 3 reboot is requested with confirmation; `--reboot` authorises it in advance and
`--no-reboot` forbids it (exits with 10 and instructions). Without an interactive terminal and
without `--reboot`, it never reboots on its own.

### Automatic verification on the next boot

`--verify-boot` is meant to be called at login or at boot. It closes the pending state
(`reboot-to-enter`, `reboot-to-recover`) and diagnoses:

```sh
sudo sdr-load --verify-boot            # by hand
```

`--verify-boot` only closes the state and diagnoses: it does **not** enter SDR mode. To finish
the activation in the same step, without writing the image again, the command is
`sudo sdr-load --resume`; with a state file and nothing pending it says so and exits with 0, and
with no state file at all it exits with 2 (nothing to resume).

To make it automatic, a user (or system) unit at boot:

```ini
# ~/.config/systemd/user/sdr-load-verify.service
[Unit]
Description=sdr-load: verify the modem after boot
After=ofono.service

[Service]
Type=oneshot
ExecStart=/usr/bin/sudo -n /usr/local/bin/sdr-load --verify-boot
```

```sh
systemctl --user enable --now sdr-load-verify.service
journalctl --user -u sdr-load-verify -n 30
```

If you would rather install nothing, adding the line to `~/.profile` is enough; it prints the
result and does not block.

## Exit codes

| code | meaning |
|---|---|
| 0 | success |
| 1 | bad usage (options, missing `--size`, cancelled by the user) |
| 2 | precondition not met (radios on, data interface up, no root, node missing) |
| 3 | the radios could not be blocked |
| 4 | the flash failed (script or image) |
| 5 | sha256 mismatch (image or partition) |
| 6 | flash fine but a **reboot is needed** before entering |
| 7 | the SDR mode enter/exit ioctl failed |
| 8 | the mode could not be verified (rings moving or window not available) |
| 9 | incomplete revert: manual intervention needed |
| 10 | recovery image flashed; reboot needed (`--no-reboot`) |
| 11 | rebooting the phone (last resort) |
| 12 | SDR mode active but **no dump engine** (mark missing) |
| 13 | the partition image **has no mode profile**, the profile does not declare the sequence, or the image is **passive**: `--activate` refuses |
| 14 | **activation failed**: a step was not verified; the loops have been stopped (the rails stay as they were) |

## Options

| option | effect |
|---|---|
| `--check` | read-only report (default action) |
| `--image <path>` | firmware image to flash |
| `--no-flash` | do not flash: the image is already loaded |
| `--activate` | activates SDR mode: PLL, rails and loops, verified after every step |
| `--resume` | finishes a flash or an activation a reboot interrupted; never writes the image |
| `--deactivate` | stops the loops; records that the rails are not powered down in software |
| `--modes` | prints the "mode number → meaning" table of the known profiles |
| `--profile <name\|path>` | mode profile (`standard-a`, `standard-b`, `sdr-8bit` or a `key = value` file) |
| `--sha256 <hex>` | expected sha256 (of the image when flashing, of the partition when loading/activating) |
| `--size <bytes>` | usable bytes of the partition (the image ones) |
| `--restore` | revert |
| `--recover-image <path>` | good image for the recovery (def. `/home/furios/modem-E6.img`) |
| `--flash-script <path>` | flash script (def. `/home/furios/flash_modem.sh`) |
| `--partition <path>` | firmware partition (def. the one the script declares with `PART=`) |
| `--state <path>` | state file |
| `--reboot` / `--no-reboot` | allow / forbid the reboot |
| `--yes` | do not ask |
| `--dry-run` | print the plan and run nothing |
| `--json` | final summary in JSON |
| `--quiet` | errors only |
| `--settle-ms`, `--gap-ms`, `--samples` | wait after entering and ring sampling (1500 / 1000 / 2) |
| `--wait-ms` | maximum wait for the modem when reverting (20000) |
| `--md1-timeout-ms` | wait for `md1.status = ready` on activation (60000) |
| `--mode-timeout-ms` | wait for each mode requested from the modem (90000) |
| `--beat-ms` | separation between the two ring samples (2000) |

## Details that matter

- **The partition is not named in the code**: it is inferred from the `PART=` line of the
  flash script, which is the only source of truth. `--partition` overrides it.
- **The sha256 covers only the image**, not the whole partition (the partition is bigger). It
  is verified twice: the one the script does and the one `sdr-load` does when reading back.
- **The rings** are sampled with aligned 32-bit reads over the window (`/dev/ccci_raw_mdm`),
  in three areas; zero changes between samples are required. The window is non-cacheable
  memory: no `memcpy`, no wide accesses.
- **rfkill through sysfs**: the `rfkill` binary does not exist on the phone; writing to
  `/sys/class/rfkill/*/soft` is the same thing the utility does.
- **No block device is touched by hand**: the only thing that writes is the flash script, with
  its own read-back verification.
- `--restore` unblocks **only what it blocked** (it is stored in the state). With no state, it
  unblocks WiFi and Bluetooth and warns about it.
- **The mode numbers are not in the code**: they come from the image profile (table by sha256
  or `--profile` file). If the image is not in the table, `--activate` runs nothing and exits
  with 13.
- **`CON0` is never written with 0**: the rail modes do `CON0 |= 1` with a guard (only if it
  holds 0 or 1) and `--deactivate` does not touch the PMIC. Powering the rails down is a job
  for a reboot.

## Tests run without changing the phone state

- `--selftest`: 68 checks (SHA-256 with FIPS vectors, ring diffs, state round-trip in JSON,
  the canonical state path and its three read outcomes, the image/partition check that lets a
  flash be resumed without writing it again, plans of the destructive actions, profiles by
  image and profile file).
- `--modes`, `--modes --profile standard-a` and `--activate/--deactivate --dry-run` on the
  laptop: full table, nine-step plan and refusal with code 13 if the profile does not exist.
- `--check` on the phone: 1.1 s, detects moving rings (A quiet, B ~52,000 words changed per
  second), missing mark and the right verdict.
- `--check --partition … --size …`: the sha256 of the 63,018,064 B of the partition matches
  coreutils `head -c … | sha256sum`.
- `--no-flash --dry-run --sha256 …`: right path (0), wrong hash (5) and missing size (1).
  Without writing the state file.
- `--restore --dry-run`: prints the three levels and runs no ioctl or reboot.

# iqmode

Minimal version, no flashing and no persisted state:

```sh
sudo iqmode --check     # precondition: no cccni, WiFi blocked, BT blocked
sudo iqmode --enter     # enter SDR mode ioctl
sudo iqmode --exit      # exit ioctl + ofono restart
```
