/* fader_selftest.c - check of the bf_fader_* helpers (the crosspoint
 * and MIX-mode monitoring curve): the table + interpolation must
 * round-trip exactly and stay monotonic, and the calibrated anchors
 * (cap_calib.pcap 2026-08-22) must hit.
 *
 * The helpers are the driver's own, pulled out of babyfacepro-ctl.c by
 * extract_laws.py - run through selftests.sh.
 *
 * laws: bf_fader_curve bf_fader_raw_to_db2 bf_fader_db2_to_raw
 *	 BF_FADER_DB2_INF
 */
#include <stdio.h>

static int fails;

static void chk(const char *name, long got, long want)
{
	if (got != want) {
		printf("FAIL %-26s got 0x%04lx want 0x%04lx\n", name, got, want);
		fails++;
	} else {
		printf("ok   %-26s 0x%04lx\n", name, got);
	}
}

int main(void)
{
	int i;
	uint16_t r;
	int prev;

	/* round-trip through every table point */
	for (i = 0; i < ARRAY_SIZE(bf_fader_curve); i++) {
		r = bf_fader_db2_to_raw(bf_fader_curve[i].db2);
		if (r != bf_fader_curve[i].raw) {
			printf("FAIL db2_to_raw(%d) = 0x%04x want 0x%04x\n",
			       bf_fader_curve[i].db2, r, bf_fader_curve[i].raw);
			fails++;
		}
		if (bf_fader_raw_to_db2(bf_fader_curve[i].raw) !=
		    bf_fader_curve[i].db2) {
			printf("FAIL raw_to_db2(0x%04x) = %d want %d\n",
			       bf_fader_curve[i].raw,
			       bf_fader_raw_to_db2(bf_fader_curve[i].raw),
			       bf_fader_curve[i].db2);
			fails++;
		}
	}
	/* calibrated anchors (cap_calib.pcap 2026-08-22) */
	chk("db(-20dB)->raw", bf_fader_db2_to_raw(-40), 0x0243);
	chk("raw(0x0243)->db", bf_fader_raw_to_db2(0x0243), -40);
	chk("db(0dB)->raw", bf_fader_db2_to_raw(0), 0x16a0);
	chk("db(+6dB)->raw", bf_fader_db2_to_raw(12), 0x2d41);
	chk("db(-65)->raw", bf_fader_db2_to_raw(BF_FADER_DB2_INF), 0);
	chk("db(-63)->raw", bf_fader_db2_to_raw(-126), 0);
	chk("db(-62)->raw", bf_fader_db2_to_raw(-124), 0x0003);
	chk("raw(0)->db", bf_fader_raw_to_db2(0), BF_FADER_DB2_INF);
	chk("raw(0x0002)->db", bf_fader_raw_to_db2(0x0002), BF_FADER_DB2_INF);
	/* +0.5 dB (one MIX wheel click) from 0 dB = the 0..1 dB midpoint */
	chk("wheel 0dB +1click", bf_fader_db2_to_raw(1), 0x1802);
	/* monotonic across the whole raw range */
	prev = BF_FADER_DB2_INF - 1;
	for (r = 0; r <= 0x2d41; r += 0x11) {
		int d = bf_fader_raw_to_db2(r);

		if (d < (int)prev) {
			printf("FAIL non-monotonic at 0x%04x (%d < %u)\n",
			       r, d, prev);
			fails++;
		}
		prev = d;
	}

	/* The MIX wheel keeps its level in dB2, so a click below the
	 * curve's first point is no longer lost.  A muted crosspoint
	 * seeds at the first point, and one CW click leaves mute; two
	 * CCW clicks from the old -52 dB (0x000e) sticking point move.
	 */
	{
		int db2 = BF_FADER_DB2_INF;

		if (db2 < bf_fader_curve[0].db2)
			db2 = bf_fader_curve[0].db2;
		chk("mix seed db2", db2, -124);
		db2 += 1;
		chk("mix +1 leaves mute", bf_fader_db2_to_raw(db2) != 0, 1);

		db2 = -104;	/* -52 dB, the old 0x000e sticking point */
		db2 -= 1;	/* one CCW click may round to the same raw */
		db2 -= 1;
		chk("mix -2 clicks move", bf_fader_db2_to_raw(db2) <
		    bf_fader_db2_to_raw(-104), 1);
	}
	if (fails)
		printf("== %d FAILURES ==\n", fails);
	else
		printf("== ALL OK ==\n");
	return fails != 0;
}
