#!/bin/sh
# End-to-end test without the real controller (does NOT need ./install.sh):
#   fake GameSir (uhid) -> g78k-bridge -> virtual Xbox Elite (or DualSense Edge) -> SDL, as your user
#
#   tests/fake-test.sh [rate-hz]          one pass through every button, then a summary
#   OUTPUT=dualsense-edge tests/fake-test.sh   test the DualSense Edge mode instead of the Xbox Elite
#   tests/fake-test.sh --steam [rate-hz]  keeps pressing buttons until Ctrl-C, so you can
#                                         open Steam > Settings > Controller and try binding
#
# Asks for your sudo password once (creating virtual devices needs root).
# Your user opens the virtual controller through the usual rules (the Elite is a normal
# joystick; the DualSense Edge is covered by Steam's steam-devices rules).
set -eu
cd "$(dirname "$0")/.."
STEAM=0
if [ "${1:-}" = "--steam" ]; then STEAM=1; shift; fi
RATE=${1:-1000}
LOG=$(mktemp -d "${TMPDIR:-/tmp}/g78k-test.XXXXXX")

make -s all sdl_check
sudo -v

STEPS=$(build/g78k-fake --print-steps)
if [ "$STEAM" = 1 ]; then LOOPS=0; else LOOPS=1; fi
STEP_MS=700
sudo build/g78k-fake --rate "$RATE" --loops "$LOOPS" --step-ms "$STEP_MS" >"$LOG/fake.log" 2>&1 &
FAKE=$!
BRIDGE=
cleanup() {
	[ -n "$BRIDGE" ] && sudo kill "$BRIDGE" 2>/dev/null || true
	sudo kill "$FAKE" 2>/dev/null || true
	wait 2>/dev/null || true
	rm -rf "$LOG"
}
trap cleanup EXIT INT TERM

# Find the fake's hidraw node.
NODE=
for _ in $(seq 50); do
	for h in /sys/class/hidraw/hidraw*; do
		if grep -qs 'g78k test fake' "$h/device/uevent"; then
			NODE=/dev/$(basename "$h")
		fi
	done
	[ -n "$NODE" ] && break
	sleep 0.1
done
[ -n "$NODE" ] || { echo "fake controller did not appear"; cat "$LOG/fake.log"; exit 1; }
echo "fake controller: $NODE (sending $RATE reports a second)"

OUTPUT=${OUTPUT:-elite}
case "$OUTPUT" in
elite) WANT=045e:0b00 ;;
dualsense-edge) WANT=054c:0df2 ;;
*) echo "OUTPUT must be elite or dualsense-edge"; exit 2 ;;
esac
sudo build/g78k-bridge --output "$OUTPUT" --hidraw "$NODE" --any-device >"$LOG/bridge.log" 2>&1 &
BRIDGE=$!

if [ "$STEAM" = 1 ]; then
	echo "Open Steam > Settings > Controller: the virtual controller should be pressing buttons by itself."
	echo "Press Ctrl-C here when done."
	tail -f "$LOG/fake.log" || true
	exit 0
fi

SECS=$((STEPS * STEP_MS / 1000 + 4))
START=$(date +%s)
build/sdl_check "$SECS" "$WANT" || RESULT=$?

# CPU used by the bridge during the run (works for any user via /proc).
PID=$(pgrep -P "$BRIDGE" -x g78k-bridge || true)  # the test bridge, child of its sudo
if [ -n "$PID" ] && [ -r "/proc/$PID/stat" ]; then
	TICKS=$(getconf CLK_TCK)
	CPU=$(awk -v t="$TICKS" '{print ($14 + $15) / t}' "/proc/$PID/stat")
	ELAPSED=$(( $(date +%s) - START ))
	[ "$ELAPSED" -gt 0 ] || ELAPSED=1
	echo
	awk -v c="$CPU" -v e="$ELAPSED" 'BEGIN { printf "bridge CPU: %.2f s over %d s = %.2f%% of one core\n", c, e, 100 * c / e }'
fi
echo
echo "--- bridge log"; cat "$LOG/bridge.log"
echo "--- fake log (last lines)"; tail -n 5 "$LOG/fake.log"
exit "${RESULT:-0}"
