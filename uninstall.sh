#!/bin/sh
# Remove g78k-bridge and give the controller back to games as a normal device.
# Usage: sudo ./uninstall.sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
	echo "Please run with sudo: sudo ./uninstall.sh" >&2
	exit 1
fi

# Stopping each bridge also restores the raw controller nodes.
for unit in $(systemctl list-units --plain --no-legend 'g78k-bridge@*' | awk '{print $1}'); do
	systemctl stop "$unit" || true
done

rm -f /usr/local/bin/g78k-bridge \
	/etc/udev/rules.d/71-g78k-bridge.rules \
	/etc/systemd/system/g78k-bridge@.service \
	/etc/modules-load.d/g78k-bridge.conf
rm -rf /usr/local/share/doc/g78k-bridge /etc/systemd/system/g78k-bridge@.service.d
rm -rf /run/g78k-bridge

systemctl daemon-reload
udevadm control --reload
udevadm trigger --action=change --subsystem-match=hidraw --subsystem-match=input || true
echo "Removed. The controller works as a normal device again (replug it if Steam doesn't see it)."
