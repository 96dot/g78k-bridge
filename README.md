# g78k-bridge

Makes **every button on a GameSir G7 Pro 8K bindable in Steam Input on Linux**, including the two
back paddles (L4/R4) and the two extra top buttons (L5/R5).

On Linux, Steam only sees the G7 Pro 8K's standard buttons. The extra buttons are only sent in
GameSir's "extended" report, which nothing on Linux asks the controller for. This bridge asks for
it, reads the controller directly, and presents it to Steam as a controller Steam fully supports.

## What Steam sees

**By default, an Xbox Elite Series 2**, so games only ever see an Xbox controller:

| GameSir G7 Pro 8K | Xbox Elite in Steam |
|---|---|
| R5 (top, right extra) | P1 |
| R4 (back, right) | P2 |
| L5 (top, left extra) | P3 |
| L4 (back, left) | P4 |
| A B X Y, LB RB, LT RT, View, Menu, Home, sticks, D-pad | the same Xbox controls |
| Capture / Share, gyro | not available (an Elite has neither) |

**Optionally, a DualSense Edge** (see [Options](#options)), which keeps the gyro: L4/R4 become
the back paddles, L5/R5 the Fn buttons, Capture the touchpad click. Games that support PlayStation
controllers directly may then show PlayStation prompts or expect a touchpad.

Rumble from Steam and games is passed back to the controller in both modes.

## How it works

- Starts automatically at boot and whenever the controller is plugged in (cable or 2.4 GHz dongle).
- Runs as a system service, so it should behave the same in Desktop Mode and Game Mode.
- Hides the controller's own gamepad from Steam and games, so it doesn't show up twice. If the
  bridge stops, crashes or can't understand the controller, the controller is handed back as a
  normal gamepad, so you're never left without a working controller.
- Only USB connections (cable or dongle) are bridged; Bluetooth hasn't been tested.

## Status

Tested on one G7 Pro 8K (firmware 1.35, 2.4 GHz dongle, CachyOS, kernel 7.3, November 2026):

- Every button, including L4/R4/L5/R5, both sticks and both triggers reach the virtual Xbox Elite;
  Steam lists it as an "Xbox One Elite Controller" with bindable paddles.
- In DualSense Edge mode the gyro works too.
- The installed service starts on its own and hides the raw controller.

Not yet tested: rumble in games, Game Mode, starting after a reboot, the wired connection, and
other controller editions or firmware versions.

## Requirements

- Linux with systemd and udev
- A C compiler and `make` (for example `sudo pacman -S base-devel` on Arch/CachyOS)
- Steam
- `sudo`
- Optional, for the test tools only: SDL3 development files (`sdl_check`) and Python 3 (`tools/probe.py`)

On SteamOS the system folders are read-only by default; `install.sh` needs to write to
`/usr/local`, `/etc/udev` and `/etc/systemd`. This hasn't been tested on SteamOS.

## Install

```
git clone https://github.com/96dot/g78k-bridge.git
cd g78k-bridge
make
sudo ./install.sh
```

Then **unplug and replug the controller** (or restart Steam). Steam may keep using a controller it
had already opened before the install.

Check it's running:

```
journalctl -b -u 'g78k-bridge@*'
```

You should see `extended reports active` and `virtual Xbox Elite 2 created`.

## Uninstall

```
sudo ./uninstall.sh
```

This stops the bridge, gives the controller back as a normal gamepad, and removes everything the
installer added (including any settings drop-in from [Options](#options)).

## Options

To present a **DualSense Edge** instead of an Xbox Elite:

```
sudo systemctl edit g78k-bridge@.service
```

and add:

```
[Service]
ExecStart=
ExecStart=/usr/local/bin/g78k-bridge --output dualsense-edge --usb %i
```

(The empty `ExecStart=` line is needed: it clears the default before setting the new one.)
Then replug the controller.

## Troubleshooting

- **Steam shows both "GameSir-G7 Pro 8K" and the Xbox Elite.** Replug the controller or restart
  Steam: Steam kept the controller it opened before the bridge hid it.
- **No Xbox Elite appears.** Check the log (`journalctl -b -u 'g78k-bridge@*'`). The bridge only
  handles the controller's normal mode, USB ID `3537:10b8` (check with `lsusb | grep 3537`). If it
  shows `3537:1034` or `3537:1035` instead, the controller is in Xbox mode; see the next point.
- **Xbox mode (`3537:1034` / `1035`).** On Linux the controller tries Xbox mode first, fails, and
  falls back to `10b8` by itself. Tools such as Deadband keep it in Xbox mode with the kernel
  setting `usbcore.quirks=3537:1034:r,3537:1035:r`; with that setting active the bridge doesn't
  run. Remove the setting and replug to use the bridge.
- **`cannot open /dev/uinput`** in the log: load the module with `sudo modprobe uinput` (the
  installer also sets it to load at boot).

## 8000 Hz polling

In the mode the bridge uses (`3537:10b8`) the controller connects at USB Full Speed, which allows
at most 1000 reports a second whatever its poll-rate setting. 8000 Hz is only available in its Xbox
mode, and only for the standard Xbox buttons and sticks: the paddles and gyro aren't in that fast
report. So the bridge runs at 1000 Hz. See `CLAUDE.md` for the measurements.

## Testing without a controller

```
make test                       # translation checks, no root needed
make sdl_check                  # optional, needs SDL3 development files
tests/fake-test.sh              # fake controller -> bridge -> SDL; asks for sudo; prints CPU use
tests/fake-test.sh --steam      # keeps pressing buttons until Ctrl-C, to watch it in Steam
OUTPUT=dualsense-edge tests/fake-test.sh   # same for the DualSense Edge mode
```

## Files

- `src/g78k-bridge.c`: the bridge
- `src/gamesir.h`, `src/ds5.h`, `src/ds5_edge_descriptor.h`, `src/uhid_util.h`: protocol details
- `src/g78k-fake.c`: a pretend controller for testing
- `system/71-g78k-bridge.rules`: starts the bridge and hides the raw controller
- `system/g78k-bridge@.service`: the service, one per plugged-in controller
- `install.sh`, `uninstall.sh`, `Makefile`
- `tests/`: unit tests (`map_test.c`), the SDL checker (`sdl_check.c`) and the fake-controller test
- `tools/probe.py`: records what a controller looks like and sends, for checking new editions
- `tools/sniff-xbox-mode.sh`: records the controller's Xbox-mode report stream (needs sudo)
- `docs/steam-forum-post.md`: a request to Valve for native support

## Credits

- Controller protocol: SDL's GameSir driver, `src/joystick/hidapi/SDL_hidapi_gamesir.c` (zlib licence).
- DualSense Edge descriptor and feature report contents: InputPlumber by ShadowBlip (GPL-3.0).
- DualSense report layout: Linux `hid-playstation` driver. Xbox Elite identity: Linux `xpad` driver.
- Xbox-mode details and the kernel setting: Deadband (gamesir-linux-tools).

Not affiliated with GameSir, Valve, Microsoft or Sony.

Licence: GPL-3.0-or-later.
