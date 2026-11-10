#!/bin/sh
# audio-health.sh - did the stream hold while the machine was busy?
#
# Once a second, for the given number of seconds, this reads what the driver
# counts in /proc/asound/cardN/stream and PipeWire's xrun count for the card,
# and prints how much each moved.  It exits 1 if anything did.  The driver's
# "ran dry" counter is the one ALSA does not report: the card holds the whole
# stream while the playback queue is empty, so each event is a gap in both
# directions without an xrun.
#
#   sh audio-health.sh [seconds]       default 30
#
# Start a stream first (any application that plays and records on the card),
# then run a load next to it, for example:
#
#   sh audio-health.sh 120 &
#   make -j"$(nproc)" -C /usr/src/linux-headers-$(uname -r) >/dev/null
#
# The counters start again with every session, so a session that restarts
# in the middle is reported once and its new counts are taken as the base.
# PipeWire's count needs pw-top, and is left out when it is not there.

secs=${1:-30}
card=$(awk '/BabyfacePro|Babyface Pro/ { print $1; exit }' /proc/asound/cards)
[ -n "$card" ] || { echo "no Babyface Pro card found" >&2; exit 2; }
proc=/proc/asound/card$card/stream

# field <direction> <name>: a number from the /proc file, 0 when stopped.
field() {
	sed -n "s/^$1:.*$2 \([0-9][0-9]*\).*/\1/p" "$proc" 2>/dev/null | head -1
}
dry()    { v=$(field "$1" "ran dry"); echo "${v:-0}"; }
latest() { v=$(field "$1" "latest"); echo "${v:-0}"; }
state()  { [ "$(head -1 "$proc" 2>/dev/null)" = stopped ] && echo stopped || echo running; }
# pw-top's ERR column for the card's nodes, summed.
xruns() {
	command -v pw-top >/dev/null 2>&1 || { echo 0; return; }
	pw-top -b -n 2 2>/dev/null | awk '/Babyface/ { s += $9 } END { print s + 0 }'
}

if [ "$(state)" = stopped ]; then
	echo "the card is not streaming: start an application first" >&2
	exit 2
fi

pdry=$(dry playback); cdry=$(dry capture); px=$(xruns)
bad=0; maxlate=0
echo "second  playback_dry  capture_dry  pw_xruns  latest_us(play/cap)"
i=0
while [ "$i" -lt "$secs" ]; do
	sleep 1
	i=$((i + 1))
	if [ "$(state)" = stopped ]; then
		echo "$i  the session stopped"; bad=1; break
	fi
	d1=$(dry playback); d2=$(dry capture); x=$(xruns)
	l1=$(latest playback); l2=$(latest capture)
	# A smaller count means a new session: take it as the new base.
	[ "$d1" -lt "$pdry" ] && pdry=0
	[ "$d2" -lt "$cdry" ] && cdry=0
	[ "$x" -lt "$px" ] && px=0
	a=$((d1 - pdry)); b=$((d2 - cdry)); c=$((x - px))
	pdry=$d1; cdry=$d2; px=$x
	[ "$l1" -gt "$maxlate" ] && maxlate=$l1
	[ "$l2" -gt "$maxlate" ] && maxlate=$l2
	mark=
	if [ "$a" -ne 0 ] || [ "$b" -ne 0 ] || [ "$c" -ne 0 ]; then
		bad=1; mark=" <-"
	fi
	printf '%6d  %12d  %11d  %8d  %s/%s%s\n' "$i" "$a" "$b" "$c" "$l1" "$l2" "$mark"
done

echo "---"
echo "latest completion seen: ${maxlate} us"
sed -n 3,4p "$proc" 2>/dev/null
if [ "$bad" -eq 0 ]; then
	echo "no gap, no xrun in ${secs} s"
else
	echo "something moved: see the lines above"
fi
exit "$bad"
