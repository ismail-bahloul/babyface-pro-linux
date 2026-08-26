/* fader_selftest.c — standalone check for the bf_fader_* helpers of
 * panel.c (the MIX-mode monitoring curve).  Compile & run anywhere:
 *
 *	gcc -O2 -o fader_selftest fader_selftest.c && ./fader_selftest
 *
 * The helpers are duplicated here verbatim (single-file test): the
 * table + interpolation must round-trip exactly and stay monotonic,
 * and the calibrated anchors (cap_calib.pcap 2026-08-22) must hit.
 */
#include <stdint.h>
#include <stdio.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BF_FADER_DB2_INF (-130)

struct bf_fader_pt {
	int16_t db2;	/* dB × 2 */
	uint16_t raw;
};

static const struct bf_fader_pt bf_fader_curve[] = {
	{ -124, 0x0003 }, { -122, 0x0004 }, { -120, 0x0005 },
	{ -118, 0x0006 }, { -116, 0x0007 }, { -114, 0x0008 },
	{ -112, 0x0009 }, { -110, 0x000a }, { -108, 0x000b },
	{ -106, 0x000d }, { -104, 0x000e }, { -102, 0x0010 },
	{ -100, 0x0012 }, {  -98, 0x0014 }, {  -96, 0x0017 },
	{  -94, 0x0019 }, {  -92, 0x001d }, {  -90, 0x0020 },
	{  -88, 0x0024 }, {  -86, 0x0029 }, {  -84, 0x002e },
	{  -82, 0x0033 }, {  -80, 0x003a }, {  -78, 0x0041 },
	{  -76, 0x0049 }, {  -74, 0x0051 }, {  -72, 0x005b },
	{  -70, 0x0067 }, {  -68, 0x0073 }, {  -66, 0x0081 },
	{  -64, 0x0091 }, {  -62, 0x00a3 }, {  -60, 0x00b7 },
	{  -58, 0x00cd }, {  -56, 0x00e6 }, {  -54, 0x0102 },
	{  -52, 0x0122 }, {  -50, 0x0145 }, {  -48, 0x016d },
	{  -46, 0x019a }, {  -44, 0x01cc }, {  -42, 0x0204 },
	{  -40, 0x0243 }, {  -38, 0x028a }, {  -36, 0x02d9 },
	{  -34, 0x0332 }, {  -32, 0x0396 }, {  -30, 0x0406 },
	{  -28, 0x0483 }, {  -26, 0x0510 }, {  -24, 0x05af },
	{  -22, 0x0660 }, {  -20, 0x0727 }, {  -18, 0x0807 },
	{  -16, 0x0902 }, {  -14, 0x0a1b }, {  -12, 0x0b57 },
	{  -10, 0x0cb9 }, {   -8, 0x0e47 }, {   -6, 0x1004 },
	{   -4, 0x11f9 }, {   -2, 0x142a }, {    0, 0x16a0 },
	{    2, 0x1963 }, {    4, 0x1c7c }, {    6, 0x1ff6 },
	{    8, 0x23dc }, {   10, 0x283d }, {   12, 0x2d41 },
};

/* Fader raw → dB×2 (linear interpolation; raw 0 = −inf). */
static int bf_fader_raw_to_db2(uint16_t raw)
{
	int i;

	if (raw == 0 || raw < bf_fader_curve[0].raw)
		return BF_FADER_DB2_INF;
	for (i = 0; i < ARRAY_SIZE(bf_fader_curve) - 1; i++) {
		if (raw <= bf_fader_curve[i + 1].raw) {
			uint32_t num = (uint32_t)(raw - bf_fader_curve[i].raw) *
				       (uint32_t)(bf_fader_curve[i + 1].db2 -
						  bf_fader_curve[i].db2);
			uint32_t den = bf_fader_curve[i + 1].raw -
				       bf_fader_curve[i].raw;

			return bf_fader_curve[i].db2 +
			       (int)((num + den / 2) / den);
		}
	}
	return bf_fader_curve[ARRAY_SIZE(bf_fader_curve) - 1].db2;
}

/* dB×2 → fader raw (linear interpolation; below −62 dB = mute 0). */
static uint16_t bf_fader_db2_to_raw(int db2)
{
	int i;

	if (db2 <= bf_fader_curve[0].db2)
		return db2 < bf_fader_curve[0].db2 ? 0 : bf_fader_curve[0].raw;
	for (i = 0; i < ARRAY_SIZE(bf_fader_curve) - 1; i++) {
		if (db2 <= bf_fader_curve[i + 1].db2) {
			uint32_t num = (uint32_t)(db2 - bf_fader_curve[i].db2) *
				       (uint32_t)(bf_fader_curve[i + 1].raw -
						  bf_fader_curve[i].raw);
			uint32_t den = bf_fader_curve[i + 1].db2 -
				       bf_fader_curve[i].db2;

			return bf_fader_curve[i].raw +
			       (uint16_t)((num + den / 2) / den);
		}
	}
	return bf_fader_curve[ARRAY_SIZE(bf_fader_curve) - 1].raw;
}

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

	if (fails)
		printf("== %d FAILURES ==\n", fails);
	else
		printf("== ALL OK ==\n");
	return fails != 0;
}
