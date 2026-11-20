# Steam forum post (draft)

Where to post:
- Steam Community, Steam Input discussions: https://steamcommunity.com/app/353370/discussions/
  (check this is still the right forum before posting)
- Valve's Linux bug tracker on GitHub: https://github.com/ValveSoftware/steam-for-linux/issues

---

**Title:** Feature request: bind the GameSir G7 Pro 8K's extra buttons (L4/R4/L5/R5) and gyro in Steam Input on Linux

Hi Valve team,

The GameSir G7 Pro 8K is a popular PC controller with four extra buttons, two back paddles (L4/R4)
and two extra top buttons (L5/R5), plus a gyro. On Linux, Steam Input can't bind any of them.

**What happens today (Linux, firmware 1.35, 2.4 GHz dongle)**

- On Linux the controller first tries its Xbox mode (USB `3537:1035` on the dongle, `3537:1034`
  wired). The kernel's first configuration request times out (`can't set config #1, error -110`),
  so the controller falls back to a plain HID mode, USB `3537:10B8`.
- In `3537:10B8` mode Steam lists it as "GameSir-G7 Pro 8K" with only the standard buttons. The
  normal HID report has no L4/R4/L5/R5 and no gyro.
- In its Xbox mode the extra buttons aren't in the Xbox report either.

**The protocol is already public and works**

SDL 3 has a dedicated driver for this controller: `src/joystick/hidapi/SDL_hidapi_gamesir.c` in
https://github.com/libsdl-org/SDL

- USB ID: vendor `0x3537`, product `0x10B8` (cable and 2.4 GHz dongle).
- Sending output report `A2 01 00` (report ID `0xA2`, 37 bytes on firmware 1.35) switches the
  controller to an extended input report, `43 A1 C8 <payload>`. Counting from the first byte after
  `A1 C8`: L4/R4 are payload byte 2 bits `0x40`/`0x80`, L5/R5 are payload byte 3 bits `0x01`/`0x02`,
  the triggers are payload bytes 12-13, and accelerometer + gyro are payload bytes 14-25.
- Rumble is output report `A2 03 <low> <high>`.

I've confirmed this on my controller with a small open-source bridge that sends that command and
re-presents the controller as an Xbox Elite Series 2 (or a DualSense Edge, to keep the gyro):
https://github.com/96dot/g78k-bridge. All four extra buttons then work in Steam Input. Native
support would make the bridge unnecessary.

**Request**

Please support the G7 Pro 8K (`3537:10B8`) in Steam Input as a controller with four back buttons
and gyro, using the extended report above, so L4/R4/L5/R5 can be bound like the Steam Deck's back
grips or the DualSense Edge's paddles and Fn buttons.

**My setup**

- OS: CachyOS, Linux kernel 7.3, Steam desktop client
- Controller: GameSir G7 Pro 8K, firmware 1.35, 2.4 GHz dongle

Thanks!
