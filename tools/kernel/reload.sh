#!/bin/sh
# reload.sh - reload snd-usb-babyface-pro, and say who holds it when it
# refuses to go.
#
# modprobe -r fails with "Module is in use" while anything has the card
# open: a sound server's ALSA nodes first, but also a mixer that kept
# /dev/snd/controlC<n> open after the unit was unplugged.  This tries the
# plain reload, and on failure prints the module's reference count and the
# processes holding the card's nodes.
#
# With -a, the user's sound services (PipeWire, PipeWire-Pulse, WirePlumber)
# are stopped for the reload and started again afterwards, which is the
# usual cure.  Anything else still holding the card has to be closed by hand.
#
# Run as the normal user; sudo is used for modprobe.
#
# Usage: sh reload.sh [-a]
set -u

MOD=snd_usb_babyface_pro
SERVICES="pipewire.socket pipewire-pulse.socket pipewire.service pipewire-pulse.service wireplumber.service"
STOP_AUDIO=0

case "${1:-}" in
-a) STOP_AUDIO=1 ;;
"") ;;
*) echo "usage: sh reload.sh [-a]" >&2; exit 2 ;;
esac

holders() {
	echo "  refcnt: $(cat /sys/module/$MOD/refcnt 2>/dev/null || echo '?')" >&2
	out=""
	if command -v lsof >/dev/null 2>&1; then
		out=$(lsof /dev/snd/* 2>/dev/null)
	elif command -v fuser >/dev/null 2>&1; then
		out=$(fuser -v /dev/snd/* 2>&1)
	fi
	if [ -n "$out" ]; then
		echo "$out" >&2
	else
		echo "  no process holds /dev/snd/*" >&2
	fi
}

if [ "$STOP_AUDIO" = 1 ]; then
	# Started again whatever happens below.
	trap 'systemctl --user start $SERVICES' EXIT
	systemctl --user stop $SERVICES
fi

if lsmod | grep -q "^$MOD "; then
	if ! sudo modprobe -r "$MOD"; then
		echo "$MOD is in use:" >&2
		holders
		[ "$STOP_AUDIO" = 1 ] || echo "Try 'sh reload.sh -a' to stop the sound services first." >&2
		exit 1
	fi
fi

sudo modprobe "$MOD" || exit 1
sleep 2
grep -i babyface /proc/asound/cards || echo "no Babyface card yet: is it plugged in?" >&2
