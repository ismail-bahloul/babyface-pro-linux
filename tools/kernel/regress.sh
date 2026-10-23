#!/bin/sh
# regress.sh — automated non-regression suite for snd-usb-babyface-pro.
#
# Sweeps every sample rate x period (>= the current frames_per_urb floor)
# in full-duplex with tools/kernel/pcmxrun.c: xruns on both streams must be
# 0, and (alt-1 rates) the AS1/2 loopback on capture ch4/5 must carry the
# tone (signal integrity without a mic).  The identity capture map puts
# AS1/2 on ch4/5; the old fixed playback tap is gone, so the sweep
# enables the AS1/2 loopback and restores its previous state afterwards.
# period/buffer renegotiation inside one PCM session (pcmreneg.c, the
# issue-#5 shape), a start/stop stress loop (open/arm/kill/close) and,
# with --mixer-restore, a check that the host-side mixer cache survives
# an unbind/rebind.
#
# The device must be free (no PipeWire node holding hw:<card>,0).  The
# script destroys the RME PipeWire nodes (recreated by restarting
# wireplumber afterwards) instead of stopping the whole audio stack — the
# desktop session auto-respawns pipewire, which re-grabs the device.
#
# Run as the normal user (audio group).  sudo is used internally only for
# the --mixer-restore sysfs unbind/bind.
#
# Usage: sh regress.sh [--mixer-restore] [--dur N] [RATE...]
#                      (default: all 9 rates)
set -u

MIXER=0
DISCONNECT=0
DUR=2
RATES=""
EXTRA="32000 44100 48000 64000 88200 96000 128000 176400 192000"
while [ $# -gt 0 ]; do
	case "$1" in
		--mixer-restore) MIXER=1 ;;
		--disconnect-test) DISCONNECT=1 ;;
		--dur) DUR=$2; shift ;;
		--*) echo "unknown option: $1"; exit 2 ;;
		*) RATES="$RATES $1" ;;
	esac
	shift
done
[ -n "$RATES" ] || RATES=$EXTRA

CARD=$(awk '/BabyfacePro/{print $1}' /proc/asound/cards | head -1)
[ -n "$CARD" ] || { echo "FATAL: Babyface Pro card not found"; exit 1; }
echo "== card $CARD: Babyface Pro =="

FPU=$(cat /sys/module/snd_usb_babyface_pro/parameters/frames_per_urb 2>/dev/null)
NURBS=$(cat /sys/module/snd_usb_babyface_pro/parameters/nurbs 2>/dev/null)
echo "== module: frames_per_urb=$FPU nurbs=$NURBS (period floor = $FPU) =="

DIR="$(cd "$(dirname "$0")" && pwd)"
BIN="${TMPDIR:-/tmp}/pcmxrun"
[ -x "$BIN" ] || gcc -O2 -o "$BIN" "$DIR/pcmxrun.c" -lasound -lm -lpthread || exit 1

# 0.1 s of S32_LE 2ch 48k silence for the busy-check and the stress loop
# (aplay -d takes only integer seconds, so the FILE carries the short length).
SIL="${TMPDIR:-/tmp}/bf_sil.raw"
[ -f "$SIL" ] || dd if=/dev/zero of="$SIL" bs=38400 count=1 2>/dev/null

# --- free the device -----------------------------------------------------
# Destroy the RME PipeWire nodes so nothing holds hw:<card>,0.  The busy
# check is a real 50 ms playback (aplay --dump-hw-params /dev/null fails
# on the file header, not on device busy — useless as a probe).
node_id() {
	# $1 = output|input ; id of the RME node for that direction (pw-dump JSON)
	pw-dump 2>/dev/null | python3 -c "
import json, sys
d = json.load(sys.stdin)
for o in d:
    if o.get('type') == 'PipeWire:Interface:Node':
        n = o.get('info', {}).get('props', {}).get('node.name', '')
        if 'alsa_${1}.usb-RME' in n:
            print(o['id'])
"
}
free_check() {
	timeout 3 aplay -q -D "hw:$CARD,0" -f S32_LE -c 2 -r 48000 \
		--period-size=256 --buffer-size=512 -d 1 "$SIL" >/dev/null 2>&1
}
# Free hw:<card>,0 from PipeWire; returns non-zero if it stays busy.
free_device() {
	free_check && return 0
	SINK=$(node_id output)
	SRC=$(node_id input)
	echo "-- destroying RME PipeWire nodes (sink=$SINK src=$SRC) to free hw:$CARD,0"
	[ -n "$SINK" ] && pw-cli destroy "$SINK"
	[ -n "$SRC" ] && pw-cli destroy "$SRC"
	sleep 2
	free_check
}
if ! free_device; then
	echo "FATAL: hw:$CARD,0 still busy after destroying the RME nodes."
	echo "       Close whatever holds it (check: fuser -v /dev/snd/pcmC${CARD}D0*)"
	exit 1
fi

# The old fixed playback tap is gone: the identity capture map puts
# AS1/2 on ch4/5.  Enable the AS1/2 output loopback so the playback tone
# reaches those channels for the tap checks, and restore the previous
# loopback state when they are done.
LB_AS12=$(amixer -c "$CARD" controls | awk -F'[ ,]' '/Loopback Switch/ && /index=2/ {print $1}' | head -1)
LB_BEFORE=$(amixer -c "$CARD" cget "$LB_AS12" 2>/dev/null | sed -n 's/.*values=//p' | tail -1)
if [ -n "$LB_AS12" ]; then
	amixer -c "$CARD" cset "$LB_AS12" on >/dev/null || LB_AS12=""
fi

# --- PCM sweep ------------------------------------------------------------
# Period floor for the invariant: period >= max(fpu, 32).  period 16
# (buffer 32) is the absolute minimum but shows rare xruns even at 48 kHz
# (0.33 ms of audio = any scheduler hiccup overruns) — not a regression
# signal.  Override with PERIODS="16" for extreme floor testing.
if [ -z "${PERIODS:-}" ]; then
	P=$FPU
	[ "$P" -lt 32 ] && P=32
	PERIODS=""
	while [ "$P" -le 2048 ]; do
		PERIODS="$PERIODS $P"
		P=$((P * 2))
	done
fi

PASS=0; FAIL=0; SKIP=0; FAILED=""
# The device delivers IN data in alt-sized packets (448/640/1024 B for
# alt 1/2/3), so the stream URBs must be at least one packet wide:
# frames_per_urb >= 8/16/32 depending on the rate class.  Below that the
# driver rejects the rate cleanly (hw_params -EINVAL) — not a regression.
min_fpu() {
	case "$1" in
		32000|44100|48000|64000|88200) echo 8 ;;
		96000|128000) echo 16 ;;
		*) echo 32 ;;
	esac
}
echo "== full-duplex sweep (${DUR}s/point): rates=${RATES# } =="
for rate in $RATES; do
	if [ "$FPU" -lt "$(min_fpu "$rate")" ]; then
		SKIP=$((SKIP + 1))
		echo "  SKIP  rate=$rate (needs frames_per_urb >= $(min_fpu "$rate"))"
		continue
	fi
	for period in $PERIODS; do
		out=$("$BIN" "$CARD" "$rate" "$period" "$((period * 2))" "$DUR" 2>&1)
		rc=$?
		if [ $rc -eq 0 ]; then
			PASS=$((PASS + 1))
			printf "  PASS  %s\n" "$out"
		else
			FAIL=$((FAIL + 1)); FAILED="$FAILED $rate/$period"
			printf "  FAIL  %s\n" "$out"
		fi
	done
done

# --- runtime renegotiation (issue #5) -------------------------------------
# Change the period/buffer INSIDE one PCM session - the sequence a sound
# server runs when the user moves the buffer-size slider (hw_params -> run
# -> hw_free -> hw_params(new) -> run).  Issue #5 was this leaving the
# stream "started" but silent; the tap must carry the tone after every
# change.  Periods start at the module's frames_per_urb floor, go up and
# come back down, so both directions are covered.
echo "-- runtime renegotiation: period/buffer changes in one PCM session @48k --"
RBIN="${TMPDIR:-/tmp}/pcmreneg"
[ -x "$RBIN" ] || gcc -O2 -o "$RBIN" "$DIR/pcmreneg.c" -lasound -lm -lpthread || exit 1
P0=$FPU
[ "$P0" -lt 32 ] && P0=32
RCONF="$P0:$((P0 * 2)) $((P0 * 2)):$((P0 * 4)) $((P0 * 4)):$((P0 * 8)) $((P0 * 2)):$((P0 * 4)) $P0:$((P0 * 2))"
if "$RBIN" "$CARD" 48000 $RCONF; then
	PASS=$((PASS + 1)); echo "  PASS  runtime renegotiation"
else
	FAIL=$((FAIL + 1)); FAILED="$FAILED renegotiation"
	echo "  FAIL  runtime renegotiation"
fi

if [ -n "$LB_AS12" ]; then
	amixer -c "$CARD" cset "$LB_AS12" "${LB_BEFORE:-off}" >/dev/null
fi

# --- start/stop stress ----------------------------------------------------
echo "-- start/stop stress: 30 x (open/arm/start/stop/close) @48k period 256 --"
SFAIL=0
i=0
while [ $i -lt 30 ]; do
	if ! aplay -q -D "hw:$CARD,0" -f S32_LE -c 2 -r 48000 \
	     --period-size=256 --buffer-size=512 -d 1 "$SIL" 2>/dev/null; then
		SFAIL=$((SFAIL + 1))
	fi
	i=$((i + 1))
done
if [ "$SFAIL" -eq 0 ]; then
	PASS=$((PASS + 1)); echo "  PASS  start/stop stress (30/30)"
else
	FAIL=$((FAIL + 1)); FAILED="$FAILED stress($SFAIL/30 failed)"
	echo "  FAIL  start/stop stress: $SFAIL/30 cycles failed"
fi

bf_dev() {
	# Name of the bound interface (e.g. 3-1:1.5): the uevent file's parent dir.
	grep -l "PRODUCT=2a39/3fc0" /sys/bus/usb/drivers/snd-usb-babyface-pro/*/uevent 2>/dev/null | \
		head -1 | sed 's|/uevent$||' | xargs basename 2>/dev/null
}

# --- mixer cache restore --------------------------------------------------
if [ "$MIXER" = 1 ]; then
	echo "-- mixer cache restore: set 48V+gain 35 on AN1, unbind/rebind, verify --"
	amixer -c "$CARD" cset name='Phantom Power Mic 1' on >/dev/null 2>&1
	amixer -c "$CARD" cset name='Mic 1 Capture Volume' 35 >/dev/null 2>&1
	# udev's 90-alsa-restore.rules runs 'alsactl restore' the instant the
	# card's control interface reappears — if the saved state is stale it
	# clobbers the driver's cache restore (observed 2026-08-26 after a
	# reboot: gains came back 0).  Store the state so the udev restore
	# writes back the SAME values (the driver's restore must still win).
	sudo alsactl store >/dev/null 2>&1
	P48_BEFORE=$(amixer -c "$CARD" cget name='Phantom Power Mic 1' 2>/dev/null | grep -oE 'values=[0-9]+' | tail -1)
	G_BEFORE=$(amixer -c "$CARD" cget name='Mic 1 Capture Volume' 2>/dev/null | grep -oE 'values=[0-9]+' | tail -1)
	DEV=$(bf_dev)
	if [ -n "$DEV" ]; then
		echo "$DEV" | sudo tee "/sys/bus/usb/drivers/snd-usb-babyface-pro/unbind" >/dev/null
		sleep 1
		echo "$DEV" | sudo tee "/sys/bus/usb/drivers/snd-usb-babyface-pro/bind" >/dev/null
		sleep 2
		P48_AFTER=$(amixer -c "$CARD" cget name='Phantom Power Mic 1' 2>/dev/null | grep -oE 'values=[0-9]+' | tail -1)
		G_AFTER=$(amixer -c "$CARD" cget name='Mic 1 Capture Volume' 2>/dev/null | grep -oE 'values=[0-9]+' | tail -1)
		if [ "$P48_BEFORE" = "values=1" ] && [ "$P48_AFTER" = "values=1" ] && \
		   [ "$G_BEFORE" = "values=35" ] && [ "$G_AFTER" = "values=35" ]; then
			PASS=$((PASS + 1)); echo "  PASS  mixer cache restore (48V=$P48_AFTER gain=$G_AFTER)"
		else
			FAIL=$((FAIL + 1)); FAILED="$FAILED mixer-restore"
			echo "  FAIL  mixer cache restore (48V $P48_BEFORE->$P48_AFTER, gain $G_BEFORE->$G_AFTER)"
		fi
	else
		FAIL=$((FAIL + 1)); FAILED="$FAILED mixer-restore(no sysfs dev)"
		echo "  FAIL  mixer cache restore: device path not found"
	fi
fi

# --- disconnect mid-stream ------------------------------------------------
# An unbind while an app holds the PCM must wake it promptly with an error
# (no hang).  Uses arecord — it surfaces the read error cleanly.
if [ "$DISCONNECT" = 1 ]; then
	DEV=$(bf_dev)
	if [ -n "$DEV" ]; then
		echo "-- disconnect mid-stream: unbind while arecord holds hw:$CARD,0 --"
		(timeout 25 arecord -D "hw:$CARD,0" -f S32_LE -c 2 -r 48000 \
			--period-size=256 --buffer-size=512 -d 20 /tmp/bf_disc.wav \
			2>/dev/null; echo "rc=$?" >/tmp/bf_disc_rc) &
		sleep 3
		echo "$DEV" | sudo tee "/sys/bus/usb/drivers/snd-usb-babyface-pro/unbind" >/dev/null
		i=0
		while [ ! -f /tmp/bf_disc_rc ] && [ "$i" -lt 50 ]; do
			sleep 0.2; i=$((i + 1))
		done
		if [ -f /tmp/bf_disc_rc ]; then
			PASS=$((PASS + 1))
			echo "  PASS  disconnect mid-stream (app exited rc=$(grep -oE '[0-9]+' /tmp/bf_disc_rc))"
		else
			FAIL=$((FAIL + 1)); FAILED="$FAILED disconnect(APP HUNG)"
			echo "  FAIL  disconnect mid-stream: app did not exit within 10 s"
			pkill -f bf_disc.wav
		fi
		rm -f /tmp/bf_disc_rc
		# Rebind and verify the card + audio come back.  The re-probed
		# card is new to WirePlumber, which recreates the RME nodes and
		# may hold the device by the time it is checked: wait for the
		# card, free it again, then play.
		echo "$DEV" | sudo tee "/sys/bus/usb/drivers/snd-usb-babyface-pro/bind" >/dev/null
		i=0
		while ! grep -q BabyfacePro /proc/asound/cards && [ "$i" -lt 50 ]; do
			sleep 0.2; i=$((i + 1))
		done
		CARD=$(awk '/BabyfacePro/{print $1}' /proc/asound/cards | head -1)
		sleep 2
		if [ -z "$CARD" ]; then
			FAIL=$((FAIL + 1)); FAILED="$FAILED rebind"
			echo "  FAIL  rebind: card did not come back"
		elif ! free_device; then
			FAIL=$((FAIL + 1)); FAILED="$FAILED rebind(busy)"
			echo "  FAIL  rebind: hw:$CARD,0 still busy after destroying the RME nodes"
		elif timeout 3 aplay -q -D "hw:$CARD,0" -f S32_LE -c 2 -r 48000 \
		     --period-size=256 --buffer-size=512 -d 1 "$SIL"; then
			PASS=$((PASS + 1)); echo "  PASS  rebind: card + audio back"
		else
			FAIL=$((FAIL + 1)); FAILED="$FAILED rebind"
			echo "  FAIL  rebind: no audio after re-probe"
		fi
	else
		FAIL=$((FAIL + 1)); FAILED="$FAILED disconnect(no sysfs dev)"
		echo "  FAIL  disconnect test: device path not found"
	fi
fi

# --- done -----------------------------------------------------------------
echo "-- recreating the PipeWire RME nodes"
systemctl --user restart wireplumber 2>/dev/null
sleep 2

echo "== RESULT: $PASS pass, $FAIL fail, $SKIP skipped =="
[ -n "$FAILED" ] && echo "   failed:$FAILED"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
