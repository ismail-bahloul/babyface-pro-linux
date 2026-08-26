// ctrl9sound2.c — round 2 of the §9 sound validation, with the fixes
// from round 1:
//   - split A/B at equal baselines (0x1000/0x1000 both ears -> L=0x2000
//     + R muted): the +6 dB / single-ear change is obvious.
//   - STEREO tone (L=440 Hz, R=660 Hz) so width/balance tilts are
//     audible (a tilt IS audible even on an identical L/R tone, but the
//     different-per-ear tone makes it unambiguous).
//   - mic part properly set up: 48V AN1 ON + gain raw 11 (~35 dB) +
//     AN1 -> AN1/2, then LOOPBACK (ch 0/1).
// 48V/gain are LEFT ON after the test (matches your usual state).
//
// Build/run: gcc -O2 -o /tmp/ctrl9sound2 tools/usbdump/ctrl9sound2.c -lm
//            /tmp/ctrl9sound2
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
#define LEN 14336          /* 256 frames x 56 B */
#define FRAME 56
#define NQ 9

static int fd;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c, 0, sizeof(c));
    c.bRequestType = 0x40; c.bRequest = req; c.wValue = val; c.wIndex = idx; c.timeout = 1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl req=%02x errno=%d\n", req, errno);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
static void wait_ms(int ms) {
    long long end = us() + ms * 1000;
    while (us() < end) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) ioctl(fd, USBDEVFS_SUBMITURB, done);
    }
}
/* 440 Hz on ch0 (L), 660 Hz on ch1 (R) — a stereo pair so balance and
   width changes are obvious. 24-bit LE, byte 0 = frame marker. */
static void fill_stereo(unsigned char *buf) {
    double p1 = 0.0, p2 = 0.0;
    const double s1 = 2.0 * M_PI * 440.0 / 48000.0;
    const double s2 = 2.0 * M_PI * 660.0 / 48000.0;
    const int amp = 0x100000;   /* -18 dBFS */
    for (int f = 0; f < LEN / FRAME; f++) {
        int l = (int)(sin(p1) * amp);
        int r = (int)(sin(p2) * amp);
        for (int ch = 0; ch < 2; ch++) {
            int s = ch == 0 ? l : r;
            int base = f * FRAME + ch * 4;
            buf[base] = 0x00;
            buf[base + 1] = s & 0xFF;
            buf[base + 2] = (s >> 8) & 0xFF;
            buf[base + 3] = (s >> 16) & 0xFF;
        }
        p1 += s1; if (p1 >= 2.0 * M_PI) p1 -= 2.0 * M_PI;
        p2 += s2; if (p2 >= 2.0 * M_PI) p2 -= 2.0 * M_PI;
    }
}
int main(void) {
    fd = open(DEV, O_RDWR);
    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    struct usbdevfs_setinterface si = {5, 1}; ioctl(fd, USBDEVFS_SETINTERFACE, &si);
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
    fill_stereo(tone);
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
    /* PB1 -> AN1/2 + master: both ears at 0x1000 (-6 dB) */
    ctl(0x12, 0x1000, 0x0040 | 0xC000);
    ctl(0x12, 0x1000, 0x005B | 0xC000);
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, 0x2000, 0x03E0 | 0xC000);
    ctl(0x12, 0x2000, 0x03E1 | 0xC000);

    printf(">>> ctrl9 ROUND 2 — stereo tone: 440 Hz L / 660 Hz R <<<\n");
    wait_ms(3000);
    printf(">>> [3s] baseline: 440 L, 660 R, both ears -6 dB <<<\n");

    /* 1. STEREO SPLIT retest. */
    ctl(0x12, 0x2000, 0x000C); ctl(0x12, 0x0000, 0x0027);
    ctl(0x12, 0x2000, 0x0040 | 0xC000); ctl(0x12, 0x0000, 0x005B | 0xC000);
    printf(">>> [5s] SPLIT ON: only 440 in the L ear, +6 dB <<<\n");
    wait_ms(5000);
    ctl(0x12, 0x1000, 0x000C); ctl(0x12, 0x1000, 0x0027);
    ctl(0x12, 0x1000, 0x0040 | 0xC000); ctl(0x12, 0x1000, 0x005B | 0xC000);
    printf(">>> [3s] SPLIT OFF: 440 L + 660 R again <<<\n");
    wait_ms(3000);

    /* 2. WIDTH +0.75: 4 balance pairs -> 0x1C00/0x0400 (L much louder). */
    ctl(0x12, 0x1C00, 0x00AE); ctl(0x12, 0x0400, 0x00AF);
    ctl(0x12, 0x1C00, 0x00C8); ctl(0x12, 0x0400, 0x00C9);
    ctl(0x12, 0x1C00, 0x0046); ctl(0x12, 0x0400, 0x0047);
    ctl(0x12, 0x1C00, 0x0060); ctl(0x12, 0x0400, 0x0061);
    printf(">>> [5s] WIDTH +0.75: if PB1 -> 440 dominates hard <<<\n");
    wait_ms(5000);
    unsigned pairs[4][2] = {{0x00AE, 0x00AF}, {0x00C8, 0x00C9}, {0x0046, 0x0047}, {0x0060, 0x0061}};
    for (int i = 0; i < 4; i++) {
        ctl(0x12, 0x1000, pairs[i][0]); ctl(0x12, 0x1000, pairs[i][1]);
    }
    printf(">>> [3s] WIDTH neutral <<<\n");
    wait_ms(3000);

    /* ── Part 2: the mic (AN1). 48V ON + gain raw 11 (~35 dB). ── */
    memset(tone, 0, LEN); /* silence */
    ctl(0x17, 0x000D, 0x003F); ctl(0x21, 0x0000, 0x0000);   /* 48V AN1 */
    ctl(0x1A, (11 & 0x1F) | 0x20, 0x0000);                   /* gain ~35 dB */
    ctl(0x12, 0x16A0, 0x0034 | 0xC000);   /* AN1 L -> AN1/2 L */
    ctl(0x12, 0x16A0, 0x004E | 0xC000);   /* AN1 R -> AN1/2 R */
    printf(">>> [5s] 48V AN1 ON, gain 35, AN1 -> AN1/2. TALK: you should hear yourself <<<\n");
    wait_ms(5000);

    /* 3. LOOPBACK ON then OFF (ch 0/1 = the AN1/2 pair). */
    ctl(0x15, 0x0001, 0); ctl(0x15, 0x0001, 1);
    printf(">>> [5s] LOOPBACK ON: mic still/again audible in the phones? <<<\n");
    wait_ms(5000);
    ctl(0x15, 0x0000, 0); ctl(0x15, 0x0000, 1);
    printf(">>> [3s] LOOPBACK OFF (48V/gain stay on) <<<\n");
    wait_ms(3000);

    printf(">>> DONE — 48V AN1 + gain 35 left ON <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
