#!/bin/sh
# selftests.sh — automated checks for snd-usb-babyface-pro WITHOUT the
# card: the law selftests (fader/master/display/EQ), the PCM completion
# selftest, the module build, and checkpatch.  The hardware regress (regress.sh) is separate — it needs
# the card + a free device.
#
# Usage: sh selftests.sh          (from tools/kernel or the repo root)
set -u
cd "$(dirname "$0")"
FAIL=0

echo "== law selftests =="
# Each selftest runs the driver's own law code: extract_laws.py pulls the
# definitions it names out of the driver sources into a header.
# pcm_selftest runs the URB completion handlers, which need the kernel
# stand-ins of pcm_shim.h ahead of the extracted code.
for t in fader_selftest master_selftest disp_selftest eq_selftest pcm_selftest; do
	LM=""
	[ "$t" = eq_selftest ] && LM="-lm"
	SHIM=""
	[ "$t" = pcm_selftest ] && SHIM="-include pcm_shim.h"
	if python3 extract_laws.py "$t.c" > "/tmp/$t-laws.h" &&
	   gcc -O2 -Wall -Werror $SHIM -include "/tmp/$t-laws.h" -o "/tmp/$t" "$t.c" $LM &&
	   "/tmp/$t" > /tmp/$t.out 2>&1; then
		echo "  PASS  $t"
		grep -E '^ok' /tmp/$t.out | tail -1 > /dev/null
	else
		echo "  FAIL  $t"
		tail -5 /tmp/$t.out
		FAIL=1
	fi
done

# SELFTESTS_NO_BUILD=1 stops here: what CI runs, where no kernel tree
# matches the running kernel.
if [ -n "${SELFTESTS_NO_BUILD:-}" ]; then
	echo "== RESULT: $([ $FAIL -eq 0 ] && echo ALL OK || echo FAILURES) (build and checkpatch skipped) =="
	exit $FAIL
fi

echo "== module build =="
KSRC=/lib/modules/$(uname -r)/build
if make LLVM=1 -C "$KSRC" M="$PWD" modules > /tmp/kbuild.out 2>&1; then
	echo "  PASS  make modules"
else
	echo "  FAIL  make modules"
	tail -10 /tmp/kbuild.out
	FAIL=1
fi

echo "== checkpatch =="
KP="$KSRC/scripts/checkpatch.pl"
if [ -x "$KP" ]; then
	for f in babyfacepro.c babyfacepro-ctl.c babyfacepro.h; do
		if $KP --no-tree --strict -f "$f" 2>&1 | grep -qE 'ERROR|WARNING'; then
			echo "  WARN  $f (see checkpatch below)"
			$KP --no-tree --strict -f "$f" 2>&1 | grep -E 'ERROR|WARNING'
			FAIL=1
		else
			echo "  PASS  $f"
		fi
	done
else
	echo "  SKIP  checkpatch (no $KP)"
fi

echo "== RESULT: $([ $FAIL -eq 0 ] && echo ALL OK || echo FAILURES) =="
exit $FAIL
