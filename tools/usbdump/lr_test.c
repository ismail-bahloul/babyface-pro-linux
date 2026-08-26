// lr_test.c — is the PHONES (PH3/4) output bus stereo or mono?
// RE-style usbfs session (mimics TotalMix): streams L=440 Hz / R=880 Hz
// on PB1 (words 0/1), then reports the 440/880 content of the record
// words 0/1 (AN1/2 bus) and 2/3 (Phones bus) with each loopback on.
// Usage: sudo ./lr_test
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/usbdevice_fs.h>
#define DEV "/dev/bus/usb/003/002"
#define LEN 14336
#define FRAME 56
#define NQ 9
#define FULL 8388608.0
#define RATE 48000

static int fd;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c, 0, sizeof(c));
    c.bRequestType = 0x40; c.bRequest = req; c.wValue = val; c.wIndex = idx; c.timeout = 1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl(%02x) errno=%d\n", req, errno);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000000LL + tv.tv_usec;
}
/* Goertzel power of word `word` at `freq`, over ~1 s of capture. */
static double level_word(int word, double freq, int ms) {
    long long end = us() + ms * 1000;
    double s0 = 0, s1 = 0, cw = 2 * cos(2 * M_PI * freq / RATE);
    long n = 0;
    while (us() < end) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
            if (done->endpoint == 0x82) {
                unsigned char *b = done->buffer;
                int len = done->actual_length;
                for (int f = 0; f < len / FRAME; f++) {
                    int base = f * FRAME + word * 4;
                    int v = (int)b[base + 1] | ((int)b[base + 2] << 8) | ((int)b[base + 3] << 16);
                    if (v & 0x800000) v -= 1 << 24;
                    double t = v + cw * s0 - s1;
                    s1 = s0; s0 = t;
                    n++;
                }
            }
            ioctl(fd, USBDEVFS_SUBMITURB, done);
        }
    }
    double p = s1 * s1 + s0 * s0 - cw * s1 * s0;
    double amp = n ? sqrt(p * 2) / n : 0;
    return amp > 0 ? 20 * log10(amp / FULL) : -200;
}
static void fill_tone(unsigned char *buf) {
    double ph1 = 0, ph2 = 0;
    const double st1 = 2.0 * M_PI * 440.0 / RATE;
    const double st2 = 2.0 * M_PI * 880.0 / RATE;
    const int amp = 0x100000;
    for (int f = 0; f < LEN / FRAME; f++) {
        int l = (int)(sin(ph1) * amp);
        int r = (int)(sin(ph2) * amp);
        int base = f * FRAME;
        buf[base] = 0; buf[base + 1] = l & 0xFF; buf[base + 2] = (l >> 8) & 0xFF; buf[base + 3] = (l >> 16) & 0xFF;
        buf[base + 4] = 0; buf[base + 5] = r & 0xFF; buf[base + 6] = (r >> 8) & 0xFF; buf[base + 7] = (r >> 16) & 0xFF;
        ph1 += st1; if (ph1 >= 2 * M_PI) ph1 -= 2 * M_PI;
        ph2 += st2; if (ph2 >= 2 * M_PI) ph2 -= 2 * M_PI;
    }
}
static void init_session(void) {
    for (int i = 0; i <= 0x3D; i++) { if (i == 0x1E || i == 0x1F) continue; ctl(0x16, 0x0000, i); }
    ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);
    ctl(0x1B, 0x8234, 0xD302); ctl(0x1B, 0x7CFF, 0xF803);
    ctl(0x10, 0x0021, 0x05FF);
    ctl(0x17, 0x000C, 0x0000); ctl(0x21, 0x0000, 0x0000);
    ctl(0x10, 0x0000, 0x3000); ctl(0x10, 0x0000, 0x3000);
    ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800);
    ctl(0x10, 0x0000, 0x8000);
    ctl(0x1D, 0x0000, 0x0000);
    unsigned char *tone = calloc(1, LEN);
    fill_tone(tone);
    struct usbdevfs_urb *outs[NQ], *ins[NQ];
    for (int i = 0; i < NQ; i++) {
        outs[i] = calloc(1, sizeof(*outs[i]));
        outs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        outs[i]->endpoint = 0x01;
        outs[i]->buffer = tone;
        outs[i]->buffer_length = LEN;
        ioctl(fd, USBDEVFS_SUBMITURB, outs[i]);
    }
    for (int i = 0; i < NQ; i++) {
        ins[i] = calloc(1, sizeof(*ins[i]));
        ins[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        ins[i]->endpoint = 0x82;
        ins[i]->buffer = calloc(1, LEN);
        ins[i]->buffer_length = LEN;
        ioctl(fd, USBDEVFS_SUBMITURB, ins[i]);
    }
    ctl(0x14, 0x0000, 0xC000);
    ctl(0x12, 0x16A0, 0x0040 | 0xC000); ctl(0x12, 0x16A0, 0x005B | 0xC000);  /* PB1->Phones */
    ctl(0x12, 0x16A0, 0x0074 | 0xC000); ctl(0x12, 0x16A0, 0x008F | 0xC000);  /* PB1->AN1/2 */
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x1A, 0x00F3, 0x0006); ctl(0x1A, 0x00F3, 0x0007);
    ctl(0x12, 0x2000, 0x03E0 | 0xC000); ctl(0x12, 0x2000, 0x03E1 | 0xC000);
    ctl(0x12, 0x2000, 0x03E2 | 0xC000); ctl(0x12, 0x2000, 0x03E3 | 0xC000);
}
static void report(const char *tag, int w0, int w1) {
    printf("  %-22s w%d: 440=%6.1f 880=%6.1f | w%d: 440=%6.1f 880=%6.1f\n",
           tag, w0, level_word(w0, 440, 900), level_word(w0, 880, 900),
                w1, level_word(w1, 440, 900), level_word(w1, 880, 900));
}
int main(void) {
    fd = open(DEV, O_RDWR);
    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    struct usbdevfs_setinterface si = {5, 1}; ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    init_session();
    printf(">>> L=440/R=880 on PB1 — record words per loopback <<<\n");
    printf("  AN1/2 loopback (wIdx 0/1) -> words 0/1:\n");
    ctl(0x15, 0x0001, 0); ctl(0x15, 0x0001, 1);
    report("AN1/2 bus", 0, 1);
    ctl(0x15, 0x0000, 0); ctl(0x15, 0x0000, 1);
    printf("  Phones loopback (wIdx 2/3) -> words 2/3:\n");
    ctl(0x15, 0x0001, 2); ctl(0x15, 0x0001, 3);
    report("Phones bus", 2, 3);
    ctl(0x15, 0x0000, 2); ctl(0x15, 0x0000, 3);
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
