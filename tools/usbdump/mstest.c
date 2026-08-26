// mstest.c — definitive MS-proc ear test with the mic PHYSICALLY on AN2
// (the path MS proc mutes). 48V AN2 + gain ~35 dB, AN2 -> AN1/2 out.
// MS proc ON is tried BARE (as the capture shows) then FLAGGED (0xC000,
// like every fader write) — talk through the mic and hear whether the
// AN2 path actually cuts.
//
// Build/run: gcc -O2 -o /tmp/mstest tools/usbdump/mstest.c -lm && /tmp/mstest
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
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
/* the 4 AN2 crosspoints (low + standard, L+R) */
static void an2_x(int val, int flag) {
    ctl(0x12, val, 0x0001 | flag);
    ctl(0x12, val, 0x001B | flag);
    ctl(0x12, val, 0x0035 | flag);
    ctl(0x12, val, 0x004F | flag);
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
    /* 48V AN2 (0x0E) + gain raw 11 (~35 dB) on reg 1; AN2 -> AN1/2 out */
    ctl(0x17, 0x000E, 0x003F); ctl(0x21, 0x0000, 0x0000);
    ctl(0x1A, (11 & 0x1F) | 0x20, 0x0001);
    an2_x(0x16A0, 0xC000);   /* route at ~0 dB (flagged, like TotalMix) */
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, 0x2000, 0x03E0 | 0xC000);
    ctl(0x12, 0x2000, 0x03E1 | 0xC000);

    printf(">>> MS proc ear test — mic on AN2, 48V + gain 35, AN2 -> AN1/2 <<<\n");
    printf(">>> [4s] talk: you should hear yourself <<<\n");
    wait_ms(4000);
    an2_x(0x0000, 0);   /* MS proc ON, BARE (as the capture shows) */
    printf(">>> [5s] MS proc ON (BARE write)   — does the mic CUT? <<<\n");
    wait_ms(5000);
    an2_x(0x16A0, 0xC000);
    printf(">>> [3s] MS proc OFF — mic back? <<<\n");
    wait_ms(3000);
    an2_x(0x0000, 0xC000);   /* MS proc ON, FLAGGED */
    printf(">>> [5s] MS proc ON (FLAGGED 0xC000) — does the mic CUT? <<<\n");
    wait_ms(5000);
    an2_x(0x16A0, 0xC000);
    printf(">>> [3s] MS proc OFF — mic back? <<<\n");
    wait_ms(3000);
    printf(">>> DONE — 48V AN2 + gain 35 left ON <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
