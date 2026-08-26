// pitchsweep.c — automated validation of the Rust `set_pitch` formula
// (tuxmix-usb/src/protocol.rs) at several pitch values, by measuring the
// IN byte rate at each one. The quad is computed with the SAME f32 math
// as the Rust fn (roundf, float division) so what we send is byte-for-byte
// what `BabyfaceProUsb::set_pitch` would send.
//
// Expected: the rate moves with the pitch — +5% ≈ 1.05× baseline,
// -5% ≈ 0.95×. The device clock is restored to 0% before exit.
//
// Build/run (non-root since the udev rule):  gcc -O2 -o /tmp/pitchsweep pitchsweep.c
//                                           /tmp/pitchsweep
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
#define NQ 9

static int fd;
static long long t0;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c, 0, sizeof(c));
    c.bRequestType = 0x40; c.bRequest = req; c.wValue = val; c.wIndex = idx; c.timeout = 1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) {
        fprintf(stderr, "ctl(%02x,%04x,%04x) failed: %s\n", req, val, idx, strerror(errno));
        exit(1);
    }
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
/* Same math as protocol.rs::set_pitch (f32), then send the quad. */
static void set_pitch(float p) {
    float dds24f = roundf(50000.0f * 256.0f / (1.0f + p / 100.0f));
    unsigned dds24 = (unsigned)dds24f;
    unsigned dds16 = dds24 >> 8;
    unsigned frac = dds24 & 0xFF;
    unsigned b1 = (unsigned)roundf((float)dds16 * 0.72562f);
    unsigned b2 = (unsigned)roundf((float)dds16 * 2.0f / 3.0f);
    printf("  [%5.1fs] set_pitch(%+.1f%%) -> 0x%04X/0x%02X  0x%04X/0x00  0x%04X/0x00  0x7CFF/0x00\n",
           (us() - t0) / 1e6, p, dds16, frac, b1, b2);
    ctl(0x1B, dds16, (frac << 8) | 0);
    ctl(0x1B, b1, 0x0001);
    ctl(0x1B, b2, 0x0002);
    ctl(0x1B, 0x7CFF, 0x0003);
    ctl(0x10, 0x0001, 0x05CF); /* clock keepalive (required) */
}
/* pump `ms`, measure the IN byte rate */
static void measure(int ms, const char *label) {
    long long end = us() + ms * 1000;
    long long bytes = 0;
    while (us() < end) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
            if (done->endpoint == 0x82) bytes += done->actual_length;
            ioctl(fd, USBDEVFS_SUBMITURB, done);
        }
    }
    double bps = bytes / (ms / 1000.0);
    printf("  [%5.1fs] %-22s %10.0f B/s  (%lld B in %d ms)\n",
           (us() - t0) / 1e6, label, bps, bytes, ms);
}
int main(void) {
    fd = open(DEV, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    struct usbdevfs_setinterface si = {5, 1}; ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    /* identical init sequence to pitchformula.c (validated on hardware) */
    for (int i = 0; i <= 0x3D; i++) { if (i == 0x1E || i == 0x1F) continue; ctl(0x16, 0x0000, i); }
    ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);
    ctl(0x1B, 0x8234, 0xD302); ctl(0x1B, 0x7CFF, 0xF803);
    ctl(0x10, 0x0021, 0x05FF);
    ctl(0x17, 0x000C, 0x0000); ctl(0x21, 0x0000, 0x0000);
    ctl(0x10, 0x0000, 0x3000); ctl(0x10, 0x0000, 0x3000);
    ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800);
    ctl(0x10, 0x0000, 0x8000);
    ctl(0x1D, 0x0000, 0x0000);
    struct usbdevfs_urb *outs[NQ], *ins[NQ];
    for (int i = 0; i < NQ; i++) {
        outs[i] = calloc(1, sizeof(*outs[i]));
        outs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        outs[i]->endpoint = 0x01;
        outs[i]->buffer = calloc(1, LEN);
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
    printf(">>> pitchsweep — Rust set_pitch formula vs hardware IN rate <<<\n");
    t0 = us();
    measure(3000, "0% (baseline)");
    set_pitch(4.0f);  measure(4000, "+4%");
    set_pitch(-2.0f); measure(4000, "-2%");
    set_pitch(5.0f);  measure(4000, "+5%");
    set_pitch(-5.0f); measure(4000, "-5%");
    set_pitch(0.0f);  measure(3000, "0% (restored)");
    printf(">>> DONE — rates should scale with the pitch <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
