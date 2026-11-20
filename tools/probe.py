#!/usr/bin/env python3
"""Probe a GameSir controller when it arrives: what Linux sees, and what it sends.

Run as your normal user:   python3 tools/probe.py
It asks for sudo once to read the raw reports. Results are saved under probe-results/.

Run it once per controller mode (Xbox, PlayStation, Switch, and the default mode),
with the cable and with the dongle. It never changes controller settings; it only
sends the same "extended reports please" command SDL sends (A2 01 00).
"""
import datetime
import glob
import os
import subprocess

GAMESIR_VID = "3537"
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

READER = r'''
import os, select, sys, time
path, secs, switch = sys.argv[1], float(sys.argv[2]), sys.argv[3] == "1"
fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
if switch:
    try:
        os.write(fd, bytes([0xA2, 0x01, 0x00]) + bytes(61))
        print("  mode switch A2 01 00 sent")
    except OSError as e:
        print("  mode switch write failed:", e)
end = time.monotonic() + secs
count, sizes, samples, first = 0, {}, [], None
while time.monotonic() < end:
    r, _, _ = select.select([fd], [], [], 0.2)
    if not r:
        continue
    try:
        data = os.read(fd, 256)
    except BlockingIOError:
        continue
    now = time.monotonic()
    first = first or now
    count += 1
    sizes[len(data)] = sizes.get(len(data), 0) + 1
    if len(samples) < 6 or (count % 500 == 0 and len(samples) < 12):
        samples.append(data[:40].hex(" "))
span = (time.monotonic() - first) if first else 0
rate = count / span if span > 0 else 0
print(f"  {count} reports in {secs:.0f} s, about {rate:.0f} per second; sizes: {sizes}")
for s in samples:
    print("   ", s)
'''


def sh(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, timeout=20).stdout
    except Exception as e:  # noqa: BLE001 - report and continue
        return f"({' '.join(cmd)} failed: {e})\n"


def read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return b""


def main():
    out = []
    say = out.append
    say(f"probe at {datetime.datetime.now().isoformat(timespec='seconds')}")
    say("kernel: " + sh(["uname", "-r"]).strip())

    devices = []
    for d in glob.glob("/sys/bus/usb/devices/*"):
        if read(os.path.join(d, "idVendor")).strip().decode() == GAMESIR_VID:
            devices.append(d)
    if not devices:
        say("No GameSir USB device found (vendor 3537). Connected USB devices:")
        say(sh(["lsusb"]))
    for d in devices:
        name = os.path.basename(d)
        pid = read(os.path.join(d, "idProduct")).strip().decode()
        prod = read(os.path.join(d, "product")).strip().decode(errors="replace")
        ver = read(os.path.join(d, "bcdDevice")).strip().decode()
        say(f"\nUSB device {name}: 3537:{pid} \"{prod}\" firmware/bcdDevice {ver}")
        for itf in sorted(glob.glob(os.path.join(d, f"{name}:*"))):
            cls = read(os.path.join(itf, "bInterfaceClass")).strip().decode()
            drv = os.path.basename(os.path.realpath(os.path.join(itf, "driver"))) if os.path.exists(
                os.path.join(itf, "driver")) else "(none)"
            say(f"  interface {os.path.basename(itf)} class {cls} driver {drv}")
            for hid in glob.glob(os.path.join(itf, "*:*:*.*")):
                rd = read(os.path.join(hid, "report_descriptor"))
                say(f"    HID {os.path.basename(hid)} report descriptor ({len(rd)} bytes): {rd.hex(' ')}")
                for h in glob.glob(os.path.join(hid, "hidraw", "hidraw*")):
                    say(f"    hidraw: /dev/{os.path.basename(h)}")
                for ev in glob.glob(os.path.join(hid, "input", "input*")):
                    nm = read(os.path.join(ev, "name")).strip().decode(errors="replace")
                    nodes = [os.path.basename(x) for x in glob.glob(os.path.join(ev, "event*")) + glob.glob(os.path.join(ev, "js*"))]
                    say(f"    input: \"{nm}\" {' '.join(nodes)}")
            for ev in glob.glob(os.path.join(itf, "input", "input*")):  # xpad and friends
                nm = read(os.path.join(ev, "name")).strip().decode(errors="replace")
                nodes = [os.path.basename(x) for x in glob.glob(os.path.join(ev, "event*")) + glob.glob(os.path.join(ev, "js*"))]
                say(f"    input (non-HID driver): \"{nm}\" {' '.join(nodes)}")

    say("\nbridge service: " + (sh(["systemctl", "list-units", "--plain", "--no-legend", "g78k-bridge@*"]).strip() or "not running"))
    print("\n".join(out))

    hidraws = []
    for d in devices:
        hidraws += sorted(glob.glob(os.path.join(d, "**", "hidraw", "hidraw*"), recursive=True))
    if hidraws:
        print("\nReading raw reports (needs sudo). Move the sticks and press buttons while it runs.")
        for h in hidraws:
            node = "/dev/" + os.path.basename(h)
            for switch in ("0", "1"):
                label = "after mode switch" if switch == "1" else "as-is"
                print(f"\n{node} ({label}), 4 seconds:")
                out.append(f"\n{node} ({label}):")
                res = subprocess.run(["sudo", "/usr/bin/python3", "-I", "-c", READER, node, "4", switch],
                                     capture_output=True, text=True)
                text = (res.stdout + res.stderr).rstrip()
                print(text)
                out.append(text)

    os.makedirs(os.path.join(ROOT, "probe-results"), exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(ROOT, "probe-results", f"probe-{stamp}.txt")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"\nSaved to {path}")


if __name__ == "__main__":
    main()
