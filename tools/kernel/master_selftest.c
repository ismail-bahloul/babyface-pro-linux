/* master_selftest.c - check for bf_master_16bit / bf_master_reg8 /
 * bf_master_reg16 (the output master steps and the two registers).
 *
 * The helpers are the driver's own, pulled out of babyfacepro-ctl.c by
 * extract_laws.py - run through selftests.sh.
 *
 * laws: bf_master_16bit bf_master_reg8 bf_master_reg16 bf_dim_step
 */
#include <stdio.h>

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
	/* monotonic 16-bit */
	uint16_t prev = 0;
	for (int hd = -128; hd <= 12; hd++) {
		uint16_t v = bf_master_16bit(hd);
		if (v < prev) { printf("FAIL non-monotonic at %d\n", hd); fails++; }
		prev = v;
	}
	/* the control steps -> the 8-bit register: mute, the -64 dB bottom
	 * the register keeps below it, the DIM capture's -20 dB, 0 and +6 dB */
	{
		static const struct { unsigned int step; uint8_t want; } c8[] = {
			{ 0, 0x3b }, { 1, 0x73 }, { 52, 0x73 }, { 53, 0x73 },
			{ 54, 0x74 }, { 141, 0xcb }, { 181, 0xf3 }, { 193, 0xff },
		};
		for (i = 0; i < (int)(sizeof(c8) / sizeof(c8[0])); i++) {
			uint8_t got = bf_master_reg8(c8[i].step);
			if (got != c8[i].want) {
				printf("FAIL reg8 step %u -> 0x%02x want 0x%02x\n",
				       c8[i].step, got, c8[i].want);
				fails++;
			} else {
				printf("ok   reg8 step %3u -> 0x%02x\n", c8[i].step, got);
			}
		}
	}
	/* the control steps -> the 16-bit register: mute is 0, 0 dB and +6 dB
	 * are the calibration points, and every step is a whole half-dB */
	if (bf_master_reg16(0) != 0) { printf("FAIL reg16 mute\n"); fails++; }
	if (bf_master_reg16(181) != 0x2000) { printf("FAIL reg16 0 dB\n"); fails++; }
	if (bf_master_reg16(193) != 0x4000) { printf("FAIL reg16 +6 dB\n"); fails++; }
	for (i = 1; i <= 193; i++) {
		if (bf_master_reg16(i) != bf_master_16bit(i - 181)) {
			printf("FAIL reg16 step %d\n", i);
			fails++;
		}
	}
	/* DIM takes 20 dB off the current level: the 8-bit pairs TotalMix
	 * writes on a Phones master at 0, -10 and -22 dB, and the bottom
	 * step below that, never mute */
	{
		static const struct { unsigned int step; uint8_t want; } d8[] = {
			{ 181, 0xcb }, { 161, 0xb7 }, { 137, 0x9f },
		};
		for (i = 0; i < (int)(sizeof(d8) / sizeof(d8[0])); i++) {
			uint8_t got = bf_master_reg8(bf_dim_step(d8[i].step));
			if (got != d8[i].want) {
				printf("FAIL dim step %u -> 0x%02x want 0x%02x\n",
				       d8[i].step, got, d8[i].want);
				fails++;
			} else {
				printf("ok   dim step %3u -> 0x%02x\n", d8[i].step, got);
			}
		}
		for (i = 1; i <= 41; i++) {
			if (bf_dim_step(i) != 1) {
				printf("FAIL dim step %d not the bottom\n", i);
				fails++;
			}
		}
	}
	printf(fails ? "== %d FAILURES ==\n" : "== ALL OK ==\n", fails);
	return fails != 0;
}
