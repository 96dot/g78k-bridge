#!/bin/sh
# Install g78k-bridge system-wide so it starts at boot and works in Desktop and Game Mode.
# Usage: sudo ./install.sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "Please run with sudo: sudo ./install.sh" >&2
	exit 1
fi
cd "$(dirname "$0")"

if [ ! -x build/g78k-bridge ]; then
	echo "Build first (as your normal user): make" >&2
	exit 1
fi

install -D -m 0755 build/g78k-bridge /usr/local/bin/g78k-bridge
install -D -m 0644 system/71-g78k-bridge.rules /etc/udev/rules.d/71-g78k-bridge.rules
install -D -m 0644 system/g78k-bridge@.service /etc/systemd/system/g78k-bridge@.service
install -D -m 0644 README.md /usr/local/share/doc/g78k-bridge/README.md

# uinput (virtual Xbox Elite, the default) and uhid (virtual DualSense Edge) must be
# loaded for the virtual controller; usually they are, this makes sure at boot.
printf 'uinput\nuhid\n' > /etc/modules-load.d/g78k-bridge.conf
modprobe uinput || true
modprobe uhid || true

systemctl daemon-reload
udevadm control --reload
# Apply to a controller that's already plugged in (no-op otherwise).
udevadm trigger --action=add --subsystem-match=usb --attr-match=idVendor=3537 --attr-match=idProduct=10b8 || true
udevadm trigger --action=change --subsystem-match=hidraw --subsystem-match=input || true

echo "Installed. Plug in the GameSir G7 Pro 8K (cable or dongle); Steam will show an Xbox Elite controller."
echo "Logs:   journalctl -b -u 'g78k-bridge@*'"
