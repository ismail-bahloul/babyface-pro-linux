/* eq_selftest.c - check of the fixed-point EQ math.  Checks:
 *   - the stored words match the double-precision RBJ reference
 *     (the reference itself reproduces RME's captured words to ~1 LSB,
 *     see tools/usbdump/eq_biquad.md) across the type/freq/Q/gain
 *     sweep;
 *   - the response of a bell built from the words peaks at the
 *     labeled freq with the labeled gain (and is flat at DC/Nyquist);
 *   - an inactive band is the identity with a defined shared scale;
 *   - the low-cut word formula + slope bytes are sane.
 *
 * The helpers are the driver's own, pulled out of babyfacepro-ctl.c by
 * extract_laws.py - run through selftests.sh (links with -lm).
 *
 * laws: bf_eq_band_words bf_eq_lc_freq_raw bf_eq_lc_slope_byte
 *	 BF_EQ_Q27 BF_EQ_LC_OFF
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* ---- double-precision reference (the Rust/RME-verified formula) ---- */

static void ref_words(double *w, int type, int32_t freq_hz, int32_t q100,
		      int32_t gain_x10, int32_t fs)
{
	double g = gain_x10 / 10.0;
	double q = q100 / 100.0;
	double w0 = 2.0 * M_PI * freq_hz / fs;
	double A = pow(10.0, g / 40.0);
	/* alpha = sin(w0 / (2Q)) as TotalMix computes it, the angle held to
	 * pi/2 as in the driver */
	double u = w0 / (2.0 * q);
	double al = sin(u > M_PI / 2 ? M_PI / 2 : u);
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
						       qs[qi], (long long)maxerr);
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

	/* effective Q at high frequency, against what RME's unit did: the
	 * cap_eq10 sweep of a +6 dB bell at 48 kHz, in
	 * tools/usbdump/eq_biquad.md.  The cookbook Q that has the same
	 * alpha as the stored words; alpha comes back from a2' = (1 - a/A) /
	 * (1 + a/A).  The 4 kHz row of that table is only "0.70 and below"
	 * and the 20 kHz row at Q 0.7 sits in the fit's residual floor, so
	 * neither is checked. */
	{
		static const struct {
			int32_t f, q100;
			double qeff;
		} cap[] = {
			{ 8000, 70, 0.635 }, { 10000, 70, 0.60 },
			{ 12000, 70, 0.555 }, { 16000, 70, 0.435 },
			{ 8000, 500, 4.17 }, { 16000, 500, 2.10 },
			{ 20000, 500, 0.97 },
		};
		double A = pow(10.0, 6.0 / 40.0);
		int n;

		for (n = 0; n < (int)(sizeof(cap) / sizeof(cap[0])); n++) {
			int32_t w[5];
			double w0 = 2 * M_PI * cap[n].f / fs;
			double a2, al, qeff;

			bf_eq_band_words(w, 1, cap[n].f, cap[n].q100, 60, fs);
			a2 = w[1] / (double)BF_EQ_Q27;
			al = A * (1 - a2) / (1 + a2);
			qeff = sin(w0) / (2 * al);
			printf("effective Q %5d Hz Q %.1f: %.3f (RME %.3f)\n",
			       cap[n].f, cap[n].q100 / 100.0, qeff, cap[n].qeff);
			if (fabs(qeff - cap[n].qeff) > 0.02 * cap[n].qeff) {
				printf("ERR effective Q off RME's by more than 2%%\n");
				fails++;
			}
		}
	}

	/* inactive band (gain 0 or no Q): identity, shared scale 1.0 */
	{
		int32_t w[5] = { -1, -1, -1, -1, -1 };
		int32_t v[5] = { -1, -1, -1, -1, -1 };

		bf_eq_band_words(w, 1, 1000, 70, 0, fs);
		bf_eq_band_words(v, 1, 1000, 0, 60, fs);
		printf("inactive band: %d %d %d %d 0x%X\n",
		       w[0], w[1], w[2], w[3], w[4]);
		for (i = 0; i < 5; i++) {
			int32_t want = i == 4 ? BF_EQ_Q27 : 0;

			if (w[i] != want || v[i] != want) {
				printf("ERR inactive band word %d\n", i);
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
