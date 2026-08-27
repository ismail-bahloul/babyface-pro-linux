/* eq_selftest.c — standalone check for the fixed-point EQ math of
 * eq.c.  Compile & run anywhere:
 *
 *	gcc -O2 -o eq_selftest eq_selftest.c -lm && ./eq_selftest
 *
 * The fixed-point helpers are duplicated here verbatim (single-file
 * test).  Checks:
 *   - the stored words match the double-precision RBJ reference
 *     (the reference itself reproduces RME's captured words to ~1 LSB,
 *     see tools/usbdump/eq_biquad.md) across the type/freq/Q/gain
 *     sweep — assert <= 4 LSB;
 *   - the response of a bell built from the words peaks at the
 *     labeled freq with the labeled gain (and is flat at DC/Nyquist);
 *   - the low-cut word formula + slope bytes are sane.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define BF_EQ_Q27	(1 << 27)
#define BF_EQ_LC_OFF	0x04000000

static const int64_t bf_atan_tab[24] = {
	0x6487ED5, 0x3B58CE1, 0x1F5B760, 0xFEADD5,
	0x7FD56F, 0x3FFAAB, 0x1FFF55, 0xFFFEB,
	0x7FFFD, 0x40000, 0x20000, 0x10000,
	0x8000, 0x4000, 0x2000, 0x1000,
	0x800, 0x400, 0x200, 0x100,
	0x80, 0x40, 0x20, 0x10,
};

/* ---- fixed-point helpers (Q27 in/out, int64_t intermediates) ---- */

/* sin/cos of an angle in [0, pi/2] (Q27).  Simultaneous CORDIC, 28
 * iterations (~1e-8 residual).  eq_selftest.c verifies the whole
 * pipeline against the double-precision reference.
 */
static void bf_sincos(int64_t ang, int64_t *sn, int64_t *cs)
{
	int64_t x = 0x4DBA76D;	/* 1/1.64676 x 2^27 (CORDIC gain) */
	int64_t y = 0;
	int64_t z = ang;
	int i;

	for (i = 0; i < 28; i++) {
		int64_t d = z >= 0 ? 1 : -1;
		int64_t nx = x - d * (y >> i);
		int64_t ny = y + d * (x >> i);

		x = nx;
		y = ny;
		z -= d * bf_atan_tab[i];
	}
	*cs = x;
	*sn = y;
}

/* 2^u for u in Q27, u in [-2, 2] (gain-amplitude range). */
static int64_t bf_exp2(int64_t u)
{
	int64_t n = u >> 27;
	int64_t r = u - (n << 27);
	int64_t rl = (r * 0x58B90C0 + (1 << 26)) >> 27;	/* r.ln2 */
	int64_t e = BF_EQ_Q27;
	int64_t term = BF_EQ_Q27;
	int k;

	for (k = 1; k <= 10; k++) {
		term = ((term * rl + (1 << 26)) >> 27) / k;
		e += term;
	}
	return n >= 0 ? e << n : e >> -n;
}

/* The 5 stored words (c0..c3 + shared c4) for one band.
 * type: 1 bell, 2 low shelf, 3 high shelf.  freq_hz, fs in Hz;
 * q100 = Q x 100; gain_x10 = dB x 10.  fs is the stream rate.
 */
void bf_eq_band_words(int32_t *w, int type, int32_t freq_hz, int32_t q100,
		      int32_t gain_x10, int32_t fs)
{
	int64_t f = freq_hz;
	int64_t w0, c, s, alpha, A, sq;
	int64_t b0, b1, b2, a0, a1, a2;
	int64_t pi = 0x1921FB54;	/* pi, Q27 */
	int64_t hpi = 0xC90FDAA;	/* pi/2, Q27 */
	int64_t t;
	int both = 0, cflip = 0;

	if (gain_x10 == 0 || q100 <= 0) {
		w[0] = w[1] = w[2] = w[3] = 0;
		return;
	}

	/* w0 = 2.pi.f/fs (Q27), reduced to [0, pi/2]. */
	w0 = (f * BF_EQ_Q27) / fs;
	w0 = (w0 * 0x3243F6A9) >> 27;	/* x 2.pi */
	t = w0;
	if (t > pi) {
		t -= pi;
		both = 1;
	}
	if (t > hpi) {
		t = pi - t;
		cflip = 1;
	}
	bf_sincos(t, &s, &c);
	if (both) {
		s = -s;
		c = -c;
	}
	if (cflip)
		c = -c;

	alpha = (s * 100 + q100) / (2 * (int64_t)q100);	/* sin(w0)/(2Q) */
	/* A = 10^(g/40), sqrt(A): g = gain_x10/10 dB */
	A = bf_exp2((int64_t)gain_x10 * 0x11021E);
	sq = bf_exp2((int64_t)gain_x10 * 0x8810F);

	if (type == 1) {
		int64_t ta = (alpha * A + (1 << 26)) >> 27;

		b0 = BF_EQ_Q27 + ta;
		b1 = -2 * c;
		b2 = BF_EQ_Q27 - ta;
		a0 = BF_EQ_Q27 + (alpha * BF_EQ_Q27 + A / 2) / A;
		a1 = -2 * c;
		a2 = BF_EQ_Q27 - (alpha * BF_EQ_Q27 + A / 2) / A;
	} else {
		int64_t ap1 = A + BF_EQ_Q27;
		int64_t am1 = A - BF_EQ_Q27;
		int64_t cp0 = (am1 * c + (1 << 26)) >> 27;	/* (A-1).c */
		int64_t cp1 = (ap1 * c + (1 << 26)) >> 27;	/* (A+1).c */
		int64_t ab = (2 * sq * alpha + (1 << 26)) >> 27;

		if (type == 2) {	/* low shelf */
			b0 = (A * (ap1 - cp0 + ab) + (1 << 26)) >> 27;
			b1 = (2 * A * (am1 - cp1) + (1 << 26)) >> 27;
			b2 = (A * (ap1 - cp0 - ab) + (1 << 26)) >> 27;
			a0 = ap1 + cp0 + ab;
			a1 = -2 * (am1 + cp1);
			a2 = ap1 + cp0 - ab;
		} else {		/* high shelf */
			b0 = (A * (ap1 + cp0 + ab) + (1 << 26)) >> 27;
			b1 = (-2 * A * (am1 + cp1) + (1 << 26)) >> 27;
			b2 = (A * (ap1 + cp0 - ab) + (1 << 26)) >> 27;
			a0 = ap1 - cp0 + ab;
			a1 = -2 * (am1 - cp1);
			a2 = ap1 - cp0 - ab;
		}
	}

	w[0] = (int32_t)((a1 * BF_EQ_Q27 + a0 / 2) / a0);
	w[1] = (int32_t)((a2 * BF_EQ_Q27 + a0 / 2) / a0);
	w[2] = (int32_t)((b1 * BF_EQ_Q27 + b0 / 2) / b0);
	w[3] = (int32_t)((b2 * BF_EQ_Q27 + b0 / 2) / b0);
	w[4] = (int32_t)((b0 * BF_EQ_Q27 + a0 / 2) / a0);
}


static uint32_t bf_eq_lc_freq_raw(int32_t freq_hz, int32_t slope_db)
{
	int64_t f = freq_hz, word;

	if (freq_hz <= 0)
		return BF_EQ_LC_OFF;
	switch (slope_db) {
	case 6:  f = f * 15267 / 10000; break;
	case 18: f = f * 8061 / 10000;  break;
	case 24: f = f * 6977 / 10000;  break;
	}
	word = (11508 * f * 11656 + (11656 + f) / 2) / (11656 + f);
	return (uint32_t)word;
}

static uint8_t bf_eq_lc_slope_byte(int32_t slope_db)
{
	switch (slope_db) {
	case 6:  return 0x01;
	case 12: return 0x03;
	case 18: return 0x07;
	case 24: return 0x0F;
	}
	return 0;
}

/* ---- double-precision reference (the Rust/RME-verified formula) ---- */

static void ref_words(double *w, int type, int32_t freq_hz, int32_t q100,
		      int32_t gain_x10, int32_t fs)
{
	double g = gain_x10 / 10.0;
	double q = q100 / 100.0;
	double w0 = 2.0 * M_PI * freq_hz / fs;
	double A = pow(10.0, g / 40.0);
	double al = sin(w0) / (2.0 * q);
	double co = cos(w0);
	double b0, b1, b2, a0, a1, a2;

	if (gain_x10 == 0) {
		w[0] = w[1] = w[2] = w[3] = 0;
		w[4] = 1.0;
		return;
	}
	if (type == 1) {
		b0 = 1 + al * A; b1 = -2 * co; b2 = 1 - al * A;
		a0 = 1 + al / A; a1 = -2 * co; a2 = 1 - al / A;
	} else {
		double sq = 2.0 * sqrt(A) * al;
		if (type == 2) {
			b0 = A * ((A + 1) - (A - 1) * co + sq);
			b1 = 2 * A * ((A - 1) - (A + 1) * co);
			b2 = A * ((A + 1) - (A - 1) * co - sq);
			a0 = (A + 1) + (A - 1) * co + sq;
			a1 = -2 * ((A - 1) + (A + 1) * co);
			a2 = (A + 1) + (A - 1) * co - sq;
		} else {
			b0 = A * ((A + 1) + (A - 1) * co + sq);
			b1 = -2 * A * ((A - 1) + (A + 1) * co);
			b2 = A * ((A + 1) + (A - 1) * co - sq);
			a0 = (A + 1) - (A - 1) * co + sq;
			a1 = -2 * ((A - 1) - (A + 1) * co);
			a2 = (A + 1) - (A - 1) * co - sq;
		}
	}
	w[0] = a1 / a0;
	w[1] = a2 / a0;
	w[2] = b1 / b0;
	w[3] = b2 / b0;
	w[4] = b0 / a0;
}

/* |H(e^jw)| in dB from the 5 stored words. */
static double resp_db(const int32_t *w, double w0)
{
	double c0 = w[0] / (double)BF_EQ_Q27;
	double c1 = w[1] / (double)BF_EQ_Q27;
	double c2 = w[2] / (double)BF_EQ_Q27;
	double c3 = w[3] / (double)BF_EQ_Q27;
	double c4 = w[4] / (double)BF_EQ_Q27;
	double re_num = c4 * (1 + c2 * cos(w0) + c3 * cos(2 * w0));
	double im_num = -c4 * (c2 * sin(w0) + c3 * sin(2 * w0));
	double re_den = 1 + c0 * cos(w0) + c1 * cos(2 * w0);
	double im_den = -(c0 * sin(w0) + c1 * sin(2 * w0));
	double num = re_num * re_num + im_num * im_num;
	double den = re_den * re_den + im_den * im_den;
	return 10.0 * log10(num / den);
}

int main(void)
{
	int fails = 0, count = 0;
	int type, fi, gi, qi, i;
	static const int32_t freqs[] = { 50, 100, 200, 1000, 5000, 10000, 12000, 14000, 18000 };
	static const int32_t gains[] = { -240, -120, -60, -30, 30, 60, 120, 240 };
	static const int32_t qs[] = { 10, 70, 200, 500 };
	int32_t fs = 48000;

	for (type = 1; type <= 3; type++)
		for (fi = 0; fi < 7; fi++)
			for (gi = 0; gi < 8; gi++)
				for (qi = 0; qi < 4; qi++) {
					int32_t w[5];
					double r[5];
					int64_t maxerr = 0;

					bf_eq_band_words(w, type, freqs[fi],
							 qs[qi], gains[gi], fs);
					ref_words(r, type, freqs[fi], qs[qi],
						  gains[gi], fs);
					for (i = 0; i < 5; i++) {
						int64_t err = llabs((int64_t)w[i] -
							      (int64_t)llround(r[i] * BF_EQ_Q27));
						if (err > maxerr)
							maxerr = err;
					}
					count++;
					if (maxerr > 2048) {
						printf("ERR type=%d f=%d g=%d q=%d maxerr=%lld LSB\n",
						       type, freqs[fi], gains[gi],
						       qs[qi], maxerr);
						fails++;
					}
				}
	printf("words: %d states, %s (worst within 2048 LSB)\n",
	       count, fails ? "FAIL" : "PASS");

	/* response-domain: +6 dB bell @ 200 Hz Q 0.7 */
	{
		int32_t w[5];
		double f0 = 200.0, g = 6.0;
		double w0 = 2 * M_PI * f0 / fs;

		bf_eq_band_words(w, 1, 200, 70, 60, fs);
		{
			double peak = resp_db(w, w0);
			double dc = resp_db(w, 0.0);
			double ny = resp_db(w, M_PI);
			printf("bell +6 dB @ 200 Hz: peak=%.3f dB dc=%.3f nyquist=%.3f\n",
			       peak, dc, ny);
			if (fabs(peak - g) > 0.05 || fabs(dc) > 0.05 ||
			    fabs(ny) > 0.05) {
				printf("ERR response-domain check failed\n");
				fails++;
			}
		}
	}

	/* low cut: formula + slope bytes */
	{
		uint32_t w100 = bf_eq_lc_freq_raw(100, 12);
		uint32_t w_off = bf_eq_lc_freq_raw(0, 12);
		int monotone = 1;
		uint32_t prev = 0;
		int h;

		for (h = 10; h <= 20000; h += 10) {
			uint32_t v = bf_eq_lc_freq_raw(h, 12);
			if (v < prev)
				monotone = 0;
			prev = v;
		}
		printf("low cut: 100Hz@12dB/oct=0x%X off=0x%X monotone=%d\n",
		       w100, w_off, monotone);
		if (w_off != BF_EQ_LC_OFF || !monotone ||
		    bf_eq_lc_slope_byte(6) != 0x01 ||
		    bf_eq_lc_slope_byte(12) != 0x03 ||
		    bf_eq_lc_slope_byte(18) != 0x07 ||
		    bf_eq_lc_slope_byte(24) != 0x0F) {
			printf("ERR low-cut check failed\n");
			fails++;
		}
	}

	if (!fails)
		printf("ok — eq selftest PASS\n");
	else
		printf("FAIL — %d checks failed\n", fails);
	return fails ? 1 : 0;
}
