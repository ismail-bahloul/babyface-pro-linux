#!/bin/sh
# delay-sweep.sh - does the delay the driver reports match the real round trip?
#
# An application compensates a recording with the delay ALSA reports for its
# playback and capture streams.  This runs looplat --delay at several
# periods and prints, for each: the delay the driver reports (playback plus
# capture), the delay measured (an impulse written, then seen coming back),
# and the difference, the latency the report leaves out.  Near 0 the report
# is accurate.  If it grows with the period, it falls short for larger URBs.
#
# The default detection is the AS1/2 loopback tap, which is digital: it
# carries no converter delay.  For the full analog path, cable PH3/4 into IN3
# and run with DETECT=2.
#
# Before you run it:
#   - no stream may be open on the card.  A sound server that is not playing
#     to it is fine (check: cat /proc/asound/cardN/pcm0p/sub0/status says
#     "closed").  If the run says the device is busy, stop the sound server:
#       systemctl --user stop pipewire.socket pipewire.service wireplumber.service
#   - turn the monitors and the headphones down: the test sends a click on
#     playback channels 1/2 every half second, through the mixer's routing.
#
# Usage: sh delay-sweep.sh [-y] [seconds]        (default 10 s per period)
#   CARD=<number|id>   the card (default: the first "Babyface" in /proc/asound/cards)
#   PERIODS="32 64 128 256"   RATE=48000   DETECT=4   PB_CH=2
#
# Paste the whole output in a report.

ASK=1
[ "$1" = "-y" ] && { ASK=0; shift; }
DUR=${1:-10}
RATE=${RATE:-48000}
PERIODS=${PERIODS:-"32 64 128 256"}
DETECT=${DETECT:-4}
PB_CH=${PB_CH:-2}
CARD=${CARD:-$(awk '/Babyface/ {print $1; exit}' /proc/asound/cards 2>/dev/null)}
SRC="$(dirname "$0")/looplat.c"
BIN="${TMPDIR:-/tmp}/looplat"

[ -n "$CARD" ] || { echo "no Babyface card found: set CARD="; exit 1; }
[ -x "$BIN" ] || gcc -O2 -o "$BIN" "$SRC" -lasound -lpthread || exit 1

if [ "$ASK" = 1 ]; then
	echo "The card must have no stream open, and the test plays a click on its"
	echo "outputs: turn the monitors and headphones down."
	printf "Press Enter to start, Ctrl-C to cancel. "
	read -r _
fi

LBID=
if [ "$DETECT" = 4 ]; then
	# The AS1/2 loopback, as latency-test.sh finds it.
	LBID=$(amixer -c "$CARD" controls | awk -F'[ ,]' '/Loopback Switch/ && /index=2/ {print $1}' | head -1)
	[ -n "$LBID" ] || {
		echo "AS1/2 loopback control not found: is the proprietary driver loaded?"
		exit 1
	}
	LB_BEFORE=$(amixer -c "$CARD" cget "$LBID" 2>/dev/null | sed -n 's/.*values=//p' | tail -1)
	trap 'amixer -c "$CARD" cset "$LBID" "${LB_BEFORE:-off}" >/dev/null 2>&1' EXIT
	amixer -c "$CARD" cset "$LBID" on >/dev/null
fi

echo "== reported delay against the measured round trip @ ${RATE} Hz, ${DUR}s per period =="
echo "   card=$CARD detect=ch$DETECT playback=${PB_CH}ch, buffer = 2 periods"
echo "   driver: $(cat /sys/module/snd_usb_babyface_pro/parameters/urb_frames_min 2>/dev/null | sed 's/^/urb_frames_min=/') nurbs=$(cat /sys/module/snd_usb_babyface_pro/parameters/nurbs 2>/dev/null) frames_per_urb=$(cat /sys/module/snd_usb_babyface_pro/parameters/frames_per_urb 2>/dev/null)"
echo "   frames (and ms); extra = measured - reported"
printf "%8s %16s %16s %16s %6s\n" period reported measured extra xruns
for p in $PERIODS; do
	out=$("$BIN" -D -r "$RATE" -d "$DETECT" "$CARD" "$PB_CH" "$p" "$((p * 2))" "$DUR" 2>&1 | tail -1)
	echo "$out" | awk -v p="$p" -v rate="$RATE" '
	{
		for (i = 1; i <= NF; i++) {
			split($i, kv, "=")
			v[kv[1]] = kv[2]
		}
		if (!("measured_avg" in v)) {
			printf "%8s  %s\n", p, $0
			next
		}
		printf "%8s %9.1f (%4.2f) %9.1f (%4.2f) %9.1f (%5.2f) %6s\n", p,
			v["reported_avg"], v["reported_avg"] * 1000 / rate,
			v["measured_avg"], v["measured_avg"] * 1000 / rate,
			v["extra_avg"], v["extra_avg"] * 1000 / rate, v["xruns"]
	}'
done
