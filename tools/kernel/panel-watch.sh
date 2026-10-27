#!/bin/sh
# panel-watch.sh — watch the front-panel selection and the six output
# masters through ALSA, one line per change, while turning the wheel.
#
#   panel-watch.sh [card] [seconds]     (default: card 0, 30 s)
#
# Useful for anything that asks what the wheel moves: set the OUT or IN
# selection on the unit and turn the wheel, and the master it touches is
# the one whose value changes.  Verified that way on 2026-10-27: with
# OUT=Opt the AS1/2 master moves and the other five do not.
set -u

C=${1:-0}
DUR=${2:-30}

master()
{
	amixer -c "$C" sget "$1" 2>/dev/null |
		grep 'Front Left:' | grep -o '[0-9]\+' | head -1
}

sel()
{
	amixer -c "$C" sget "$1" 2>/dev/null |
		sed -n "s/^ *Item0: '\(.*\)'/\1/p"
}

line()
{
	printf 'OUT=%-6s IN=%-6s  AN1/2=%-4s PH3/4=%-4s AS1/2=%-4s ADAT3/4=%-4s ADAT5/6=%-4s ADAT7/8=%-4s' \
		"$(sel 'Front Panel Out')" "$(sel 'Front Panel In')" \
		"$(master 'AN1/2',0)" "$(master 'PH3/4',1)" \
		"$(master 'AS1/2',2)" "$(master 'ADAT3/4',3)" \
		"$(master 'ADAT5/6',4)" "$(master 'ADAT7/8',5)"
}

echo "== card $C, ${DUR}s — turn the wheel now =="
prev=""
i=0
while [ "$i" -lt "$((DUR * 7))" ]; do
	now="$(line)"
	if [ "$now" != "$prev" ]; then
		printf '%s  %s\n' "$(date +%T)" "$now"
		prev="$now"
	fi
	i=$((i + 1))
	sleep 0.15
done
