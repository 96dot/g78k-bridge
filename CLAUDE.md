# g78k-bridge: project notes

GameSir G7 Pro 8K (USB 3537:10B8) -> virtual Xbox Elite 2 (uinput, default) or DualSense Edge
(uhid, --output dualsense-edge) so Steam Input can bind L4/R4/L5/R5. C, libc + pthreads. See README.md.

Hardware facts (tested 2026-11-20, firmware 1.35, dongle):
- On Linux the controller tries Xbox mode 3537:1035 (dongle; 1034 wired) first; the kernel's first
  config request times out, so it falls back to 3537:10B8 (plain HID). Kernel quirk
  usbcore.quirks=3537:1034:r,3537:1035:r keeps Xbox mode (Deadband's manual).
- 10B8 ends up at USB Full Speed: 1000 reports/s max, whatever the poll-rate setting. Its interface 0
  carries report 0x01 (10 bytes with ID), the vendor 0x43 extended report (37 bytes) and the 0xA2
  command channel (37 bytes). Interface 1 is keyboard/mouse/media keys plus GameSir's settings channel.
- Xbox mode runs at High Speed with an 8 kHz Xbox-360 endpoint, but L4/R4/L5/R5 are not in that
  report; extras + gyro live on interface 1 (1 kHz, needs Deadband's keep-alive).
- Adding a usbcore "r" quirk for 10B8 itself makes it enumerate at High Speed but never bind.

Working notes:
- Build: `make` (optional `make sdl_check` needs SDL3). Tests: `make test` (translation, no root),
  `tests/fake-test.sh` (sudo; no install needed).
- Protocol source of truth: SDL `src/joystick/hidapi/SDL_hidapi_gamesir.c`.
- Xbox Elite identity: Linux `drivers/input/joystick/xpad.c`. DualSense Edge emulation reference:
  InputPlumber `src/input/target/dualsense.rs`.
- `tools/probe.py` records what a controller sends (for new editions / firmware).
- Performance: up to 1000 reports/s in 10B8 mode; no per-packet allocation or logging in the hot path.
- Design: hiding is a static udev rule (Steam opens the raw pad at plug-in, before any daemon could
  hide it). Give-back = per-node flag /run/g78k-bridge/<node> + uevent "change", done by
  `--restore`/`--hide` from systemd ExecStopPost=+/ExecStartPre=+, so the sandboxed daemon never
  writes /sys. Writes to the controller (rumble, mode switch) block for a USB round trip, so they
  run on a writer thread.
- Review bookkeeping (last reviewed commit, known lows) lives in CLAUDE.local.md (not published).
