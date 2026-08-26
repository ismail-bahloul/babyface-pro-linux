/* master_selftest.c — round-trip check for bf_master_half_db /
 * bf_master_16bit / bf_master_8bit (the front-panel OUT wheel law).
 * Compile & run: gcc -O2 -o master_selftest master_selftest.c && ./master_selftest
 */
#include <stdint.h>
#include <stdio.h>

#define BF_MASTER_MUTE 0x3b
#define BF_MASTER_8_MIN 0x73
#define BF_MASTER_8_MAX 0xff

static const uint8_t bf_lg2_frac[256] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3,
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5,
	5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8,
	8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
	8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
	9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10,
	10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
	10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
	11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
	11, 11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
};

static int bf_master_half_db(uint16_t vol16)
{
	unsigned int k, frac;

	if (vol16 == 0)
		vol16 = 1;
	if (vol16 > 0x4000)
		vol16 = 0x4000;
	k = 31 - __builtin_clz(vol16);
	frac = ((vol16 - (1u << k)) << 8) >> k;
	return 12 * (int)k - 156 + bf_lg2_frac[frac];
}

static const uint16_t bf_twelfth[12] = {
	0x1000, 0x10f4, 0x11f6, 0x1307, 0x1429, 0x155c,
	0x16a1, 0x17f9, 0x1966, 0x1ae9, 0x1c82, 0x1e34,
};

static uint16_t bf_master_16bit(int half_db)
{
	int k = half_db / 12;
	int n = half_db % 12;
	unsigned v;

	if (n < 0) {
		n += 12;
		k--;
	}
	v = (unsigned)bf_twelfth[n] << 1;
	if (k >= 0) {
		v <<= k;
	} else {
		v += 1u << (-k - 1);	/* round-half-up */
		v >>= -k;
	}
	if (v < 1) v = 1;
	if (v > 0x4000) v = 0x4000;
	return (uint16_t)v;
}

static uint8_t bf_master_8bit(uint16_t vol16)
{
	int half_db;

	if (vol16 == 0)
		return BF_MASTER_MUTE;
	half_db = bf_master_half_db(vol16);
	if (0xf3 + half_db < BF_MASTER_8_MIN) return BF_MASTER_8_MIN;
	if (0xf3 + half_db > BF_MASTER_8_MAX) return BF_MASTER_8_MAX;
	return (uint8_t)(0xf3 + half_db);
}

static int fails;

static void chk16(const char *name, int half_db, uint16_t want)
{
	uint16_t got = bf_master_16bit(half_db);
	if (got != want) {
		printf("FAIL %-24s half_db=%4d -> 0x%04x want 0x%04x\n",
		       name, half_db, got, want);
		fails++;
	} else {
		printf("ok   %-24s half_db=%4d -> 0x%04x\n", name, half_db, got);
	}
}

int main(void)
{
	int i;

	/* anchors */
	chk16("0 dB", 0, 0x2000);
	chk16("+6 dB", 12, 0x4000);
	chk16("-6 dB", -12, 0x1000);
	chk16("-12 dB", -24, 0x0800);
	chk16("+0.5 dB", 1, 0x21e8);	/* 8192*2^(1/12) = 8679 -> 0x21e7? */
	chk16("-0.5 dB", -1, 0x1e34);	/* 8192/2^(1/12) = 7732 -> 0x1e34 */
	chk16("-64 dB floor", -128, 5);
	/* round-trip over the audible span (>= -59 dB; below that the
	 * 16-bit integer grid is coarser than a half-dB step — inaudible)
	 * half_db -> 16 -> half_db, ±1 tolerance */
	for (i = -118; i <= 12; i++) {
		int back = bf_master_half_db(bf_master_16bit(i));
		if (back < i - 1 || back > i + 1) {
			printf("FAIL round-trip half_db %d -> %d\n", i, back);
			fails++;
		}
	}
	/* the 8-bit companion: 0 dB = 0xf3, -64 dB = 0x73, +6 = 0xff */
	if (bf_master_8bit(0x2000) != 0xf3) { printf("FAIL 8bit 0dB\n"); fails++; }
	if (bf_master_8bit(bf_master_16bit(-128)) != 0x73) { printf("FAIL 8bit -64\n"); fails++; }
	if (bf_master_8bit(0x4000) != 0xff) { printf("FAIL 8bit +6\n"); fails++; }
	/* monotonic 16-bit */
	uint16_t prev = 0;
	for (int hd = -128; hd <= 12; hd++) {
		uint16_t v = bf_master_16bit(hd);
		if (v < prev) { printf("FAIL non-monotonic at %d\n", hd); fails++; }
		prev = v;
	}
	/* the wheel: from 0 dB, +1 click then back */
	{
		int hd = bf_master_half_db(0x2000) + 1;	/* +0.5 dB */
		uint16_t v = bf_master_16bit(hd);
		hd = bf_master_half_db(v) - 1;
		v = bf_master_16bit(hd);
		if (v != 0x2000) { printf("FAIL wheel roundtrip -> 0x%04x\n", v); fails++; }
		else printf("ok   wheel +1/-1 click returns to 0x2000\n");
	}
	printf(fails ? "== %d FAILURES ==\n" : "== ALL OK ==\n", fails);
	return fails != 0;
}
