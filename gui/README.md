# `furimodem-sdr` — SDR mode load window

GTK3 utility for the phone (phosh, aarch64): it shows the SDR mode state and launches the
load, activation, unload and restore-to-stock operations. **It implements nothing of the
device**: it builds the command and runs `sdr-load` as a subprocess, and presents its output
and its exit code. The activation sequence lives in one place: the console tool.

## Pieces

| path | what it is |
|---|---|
| `src/main.cpp` | GTK3 window: state, actions, confirmations, progress and log |
| `src/actions.cpp` | translation of each button into an `sdr-load` command and reading of the exit codes |
| `src/status.cpp` | parsing of the `sdr-load --status` output |
| `src/runner.cpp` | subprocess execution without a shell, with two pipes |
| `src/privileges.cpp` | who runs the command: the user, `pkexec`, or nobody (the command is shown) |
| `src/theme.cpp` | embedding of the stylesheet in the binary |
| `style.css` | dark theme of the window, independent of the desktop |
| `org.furimodem.sdr.policy` | polkit action that names this tool in the system dialogue |
| `tests/parse_test.cpp` | logic tests (99 checks), without GTK or the phone |
| `tests/privileges_test.cpp` | privilege-plan tests (74 checks), without polkit or GTK |

## Requirements

- GTK3 (already in phosh). To build, the headers: `libgtk-3-dev`.
- The `sdr-load` binary, in this search order:
  1. the `SDR_LOAD` variable, if set;
  2. the same directory as this window;
  3. `PATH`.
- `pkexec` (package `pkexec`, part of polkit) to run the operations that need root from the
  window. Without it the window still works: it shows the command instead of running it.
- No other dependencies. The stylesheet is compiled into the binary: there is no data file to
  install.

## Building and testing

```sh
make -j1            # on the phone; leaves ./furimodem-sdr
make test           # logic: state, commands, codes and the privilege plan (no GTK needed)
sudo make install   # binary + desktop launcher + polkit action
```

`make install` puts `furimodem-sdr` and its launcher under `$(PREFIX)` and
`org.furimodem.sdr.policy` in `/usr/share/polkit-1/actions` (override with
`POLKIT_DIR=`): polkit only reads the system directory, not the prefix. The package does the
same.

The cross-build of the whole package is in `tools/crossbuild2.sh`; it leaves
`out/furimodem-sdr` next to the rest of the aarch64 binaries and does not pollute the native
tree. The stylesheet is wrapped into a C string by `sed` at build time, so the cross-build needs
nothing beyond `make`, `sed` and the compiler it already uses.

## What each button does

| button | exact command | what it touches |
|---|---|---|
| Refresh state | `sdr-load --status` | reads only |
| Load image... | `sdr-load --image <path> --yes --no-reboot` | **writes the firmware partition**, verifies its sha256 and asks for a reboot |
| Enter SDR mode | `sdr-load --no-flash --yes` | enters SDR mode; writes no partition |
| Start reception | `sdr-load --activate --yes` | the nine activation steps; writes no partition |
| Stop reception | `sdr-load --deactivate --yes` | stops the loops; the rails stay powered by design |
| Restore to stock... | `sdr-load --restore --yes --no-reboot` | leaves the mode, gives the radios back and, if the modem does not come back, writes the recovery image |

The **State** tab shows what the partition holds, not what the state file remembers. The rows
are `Partition`, `Image now` (the sha256 the tool hashed, with its kind: `stock image`, `mode
image <profile>`, ...) and `Image check` (the one-line verdict). When the state file and the
partition disagree, `Image check` starts with `INCOHERENT`, a banner explains it, **Start
reception** and **Enter SDR mode** are disabled, and *Load image...* becomes **Write the image
again...**: the only way out is to write the mode image, with its restart warning.

Two vocabulary notes, because the difference matters:

- **Unloading the mode** is *Stop reception*: it stops reception and leaves the firmware
  loaded.
- **Restoring to stock** is the full way back: it leaves SDR mode and returns the phone to its
  stock firmware. It is a separate action and is marked as destructive.

## Privileges

The window never handles passwords and never builds a shell command to elevate itself. When an
operation needs root it hands the command to **`pkexec`**, and polkit's agent — the system's own
dialogue, drawn by the desktop — asks for authorisation. The window only sees "authorised" or
"not authorised".

- **Already root**: the tool is run directly and the footer says so. It is not necessary and it
  is not recommended.
- **Not root, `pkexec` installed**: the command is run as
  `pkexec /path/sdr-load <args>` (no shell, arguments one by one, the tool made absolute so
  `pkexec` does not look it up in its own `PATH`). The dialogue is the system's; the window
  shows "Waiting for the system authorisation" while it is open.
- **Cancelled or refused** (`pkexec` exits 126): nothing is run, the window goes back to idle,
  says so and shows the exact command with the **Copy command** button. It never waits forever,
  because the subprocess has exited and the window has been told.
- **`pkexec` refuses on its own** (exit 127: no agent in the session, policy not installed):
  same treatment, with the reason spelled out.
- **No `pkexec`**: the operation is not launched; the banner says why and the copyable
  `sudo sdr-load ...` command is shown. Nothing is lost: it is the behaviour of older versions.

The catalogue entry is `org.furimodem.sdr.run` in `org.furimodem.sdr.policy`:

```xml
<defaults>
  <allow_any>no</allow_any>
  <allow_inactive>no</allow_inactive>
  <allow_active>auth_admin_keep</allow_active>
</defaults>
<annotate key="org.freedesktop.policykit.exec.path">/usr/bin/sdr-load</annotate>
```

`auth_admin_keep` asks once and remembers for a few minutes, so "enter the mode, then start
reception" is one dialogue and not two. `allow_any` and `allow_inactive` are `no`: only the
person in front of the active session can authorise it.

If the tool is run from a development tree (not `/usr/bin/sdr-load`), pkexec falls back to the
generic action and the dialogue is the generic one; the operation still works.

The state read is not privileged by default: the tool answers with the saved state and says
which part it cannot see. The *State* tab then offers **Read with privileges**, which repeats
`--status` through the same dialogue, and the copyable command. It is not done at start-up: a
password prompt on every launch would be worse than the delay.

## Appearance

The window is dark on its own, whatever the desktop is using:

- `gtk-application-prefer-dark-theme` is set to `TRUE` on `GtkSettings`, so the widgets that
  still take their colours from the theme pick the dark variant;
- `style.css` is embedded in the binary and loaded at `GTK_STYLE_PROVIDER_PRIORITY_USER`, above
  the session theme, and fixes the background, the surfaces, the text, the borders, the tabs,
  the buttons, the log and the banners.

Text is light on a near-black background (every pair in the sheet is well over 7:1 contrast).
There are no images, no transitions and no animation: the stack switch is instantaneous and the
progress bar only moves with the `[i/n]` steps of the tool.

## Confirmation, progress and result

- Every operation that touches the phone asks for explicit confirmation before starting. The
  default button is **Cancel**. The text says plainly whether a partition is written or not.
- `--yes` is passed **after** the window has asked: without a terminal, `sdr-load` cannot ask
  by itself.
- `--no-reboot` keeps the tool from rebooting the phone. If a reboot is needed, the window
  shows `sudo systemctl reboot` so the user can do it.
- While the operation runs, the full output is visible and so is a bar that advances with the
  `[i/n]` steps the tool prints. Closing the window is blocked until it finishes.
- When it finishes, the result is shown and the state is re-read. Exit codes are translated
  into sentences; the two that matter most here:
  - **13** — the image is not in the profile table: activation was refused without asking for
    any mode;
  - **14** — activation was disarmed: the loops may still be armed.

### Exit codes

| code | meaning | how it is presented |
|---|---|---|
| 0 | success | success |
| 1 | bad usage | window failure; the command stays in the log |
| 2 | a precondition is missing (privileges, nodes, modem) | failure, with the reason in the log |
| 3 | radios not blocked | failure |
| 4 | flash failed | failure |
| 5 | sha256 mismatch: the partition does not hold the expected image | failure; it says the write failed or the image was written back, and offers to write it again |
| 6 | reboot needed | after loading the image it is presented as **success** with the reboot command |
| 7 | SDR mode ioctl failed | failure |
| 8 | mode not verified | failure |
| 9 | incomplete revert | failure |
| 10 | recovery image written; reboot needed | failure with the reboot command |
| 11 | rebooting | success; the tool does it as a last resort |
| 12 | mode active with no dump programmed | failure; the detail names the check window and the bursts, and asks whether the sweep is running. It no longer says "repeat the activation" |
| 13 | image without a mode profile | explained failure; no mode was requested |
| 14 | activation failed | explained failure; the loops may still be armed |

## Limits

- There is no activation logic in the window: if `sdr-load` changes, the window changes with
  it.
- There is no TX.
- No firmware image is distributed.
- The window does not read device nodes or write partitions: it only launches the tool and
  shows what it does.
