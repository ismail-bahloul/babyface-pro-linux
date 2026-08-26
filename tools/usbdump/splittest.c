// splittest.c — unambiguous stereo-split A/B: a stereo tone (440 Hz L /
// 660 Hz R) alternates between the -6 dB stereo pair (both ears) and
// the split-mono pattern (0x2000 L / 0x0000 R = only 440 in the LEFT
// ear, ~6 dB louder). Three cycles so the change is unmistakable.
//
// Build/run: gcc -O2 -o /tmp/splittest tools/usbdump/splittest.c -lm
//            /tmp/splittest
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

static int fd;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c, 0, sizeof(c));
    c.bRequestType = 0x40; c.bRequest = req; c.wValue = val; c.wIndex = idx; c.timeout = 1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl(%02x) errno=%d\n", req, errno);
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
/* 440 Hz L / 660 Hz R */
static void fill_stereo(unsigned char *buf) {
    double p1 = 0.0, p2 = 0.0;
    const double s1 = 2.0 * M_PI * 440.0 / 48000.0;
    const double s2 = 2.0 * M_PI * 660.0 / 48000.0;
    const int amp = 0x100000;
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
    /* master up; the split toggles the PB1 crosspoints below */
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, 0x2000, 0x03E0 | 0xC000);
    ctl(0x12, 0x2000, 0x03E1 | 0xC000);

    printf(">>> SPLIT A/B — 3 cycles: STEREO (both ears) <-> SPLIT (only 440 in the LEFT ear) <<<\n");
    for (int cyc = 1; cyc <= 3; cyc++) {
        ctl(0x12, 0x1000, 0x000C); ctl(0x12, 0x1000, 0x0027);
        ctl(0x12, 0x1000, 0x0040 | 0xC000); ctl(0x12, 0x1000, 0x005B | 0xC000);
        printf(">>> cycle %d/3  STEREO: 440 L + 660 R, both ears <<<\n", cyc);
        wait_ms(3000);
        ctl(0x12, 0x2000, 0x000C); ctl(0x12, 0x0000, 0x0027);
        ctl(0x12, 0x2000, 0x0040 | 0xC000); ctl(0x12, 0x0000, 0x005B | 0xC000);
        printf(">>> cycle %d/3  SPLIT:  only 440 in the LEFT ear, louder <<<\n", cyc);
        wait_ms(3000);
    }
    ctl(0x12, 0x1000, 0x000C); ctl(0x12, 0x1000, 0x0027);
    ctl(0x12, 0x1000, 0x0040 | 0xC000); ctl(0x12, 0x1000, 0x005B | 0xC000);
    printf(">>> DONE — did the 660 Hz vanish and the 440 jump to the left ear? <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
