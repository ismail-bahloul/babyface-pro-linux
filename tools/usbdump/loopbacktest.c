// loopbacktest.c — definitive loopback validation: play a steady 440 Hz
// tone on PB1 -> AN1/2 OUT, then compare the IN-stream channels 0/1
// (AN1/2 input) with loopback OFF vs ON. RME loopback feeds a hardware
// OUTPUT back into the matching INPUT channels of the recording stream,
// so with loopback ON the IN channels should show the tone (≈ -18 dBFS)
// instead of the mic/analog level. Fully automated — no ears needed.
//
// Build/run: gcc -O2 -o /tmp/loopbacktest tools/usbdump/loopbacktest.c -lm
//            /tmp/loopbacktest
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
#define FULL 8388608.0     /* 2^23 = 24-bit full scale */

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
/* pump `ms`, folding IN ch0-3 peaks into `peak[]` */
static void measure(int ms, double peak[4]) {
    long long end = us() + ms * 1000;
    while (us() < end) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
            if (done->endpoint == 0x82) {
                unsigned char *b = done->buffer;
                int len = done->actual_length;
                for (int f = 0; f < len / FRAME; f++) {
                    for (int c = 0; c < 4; c++) {
                        unsigned word = (unsigned)b[f * FRAME + c * 4 + 1]
                                      | ((unsigned)b[f * FRAME + c * 4 + 2] << 8)
                                      | ((unsigned)b[f * FRAME + c * 4 + 3] << 16);
                        double mag = fabs((int)(word << 8) >> 8) / FULL;
                        if (mag > peak[c]) peak[c] = mag;
                    }
                }
            }
            ioctl(fd, USBDEVFS_SUBMITURB, done);
        }
    }
    printf("  IN peaks (dBFS)  ch0(AN1):%6.1f  ch1(AN2):%6.1f  ch2(AN3):%6.1f  ch3(AN4):%6.1f\n",
           20 * log10(peak[0] + 1e-9), 20 * log10(peak[1] + 1e-9),
           20 * log10(peak[2] + 1e-9), 20 * log10(peak[3] + 1e-9));
}
/* 440 Hz sine on OUT ch0+ch1 (PB1), 24-bit LE, byte 0 = frame marker */
static void fill_tone(unsigned char *buf) {
    double phase = 0.0;
    const double step = 2.0 * M_PI * 440.0 / 48000.0;
    const int amp = 0x100000;   /* -18 dBFS */
    for (int f = 0; f < LEN / FRAME; f++) {
        int s = (int)(sin(phase) * amp);
        for (int ch = 0; ch < 2; ch++) {
            int base = f * FRAME + ch * 4;
            buf[base] = 0x00;
            buf[base + 1] = s & 0xFF;
            buf[base + 2] = (s >> 8) & 0xFF;
            buf[base + 3] = (s >> 16) & 0xFF;
        }
        phase += step;
        if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
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
    /* PB1 -> AN1/2 out + master (the tone comes out of the AN1/2 DAC) */
    ctl(0x12, 0x16A0, 0x0040 | 0xC000);
    ctl(0x12, 0x16A0, 0x005B | 0xC000);
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, 0x2000, 0x03E0 | 0xC000);
    ctl(0x12, 0x2000, 0x03E1 | 0xC000);

    printf(">>> loopback test — tone on AN1/2 OUT, watch the IN channels <<<\n");
    double p[4];
    memset(p, 0, sizeof(p));
    measure(3000, p);   /* loopback OFF (no 0x15 writes yet) */
    printf(">>> loopback ON (ch 0/1) — expect the tone on IN ch0/1 <<<\n");
    ctl(0x15, 0x0001, 0); ctl(0x15, 0x0001, 1);
    memset(p, 0, sizeof(p));
    measure(3000, p);
    printf(">>> loopback OFF (ch 0/1) <<<\n");
    ctl(0x15, 0x0000, 0); ctl(0x15, 0x0000, 1);
    memset(p, 0, sizeof(p));
    measure(3000, p);
    printf(">>> clear ALL 30 channels + 2 s settle <<<\n");
    for (int i = 0; i < 30; i++) ctl(0x15, 0x0000, i);
    usleep(2000000);
    memset(p, 0, sizeof(p));
    measure(3000, p);
    printf(">>> DONE — does the tone vanish when the map is cleared? <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
