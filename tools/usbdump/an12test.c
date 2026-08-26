// an12test.c — validate AN 1>2 AND MS proc with the mic on AN1 only:
// AN2 is routed to the monitors, so:
//   - AN 1>2 OFF: AN2 has no signal -> silent when you talk.
//   - AN 1>2 ON:  AN1's signal appears on AN2 -> you hear the mic.
//   - MS proc ON: AN2 crosspoints are muted -> the mic cuts.
//   - MS proc OFF: mic back.
// 48V AN1 + gain raw 11 (~35 dB) are set; left ON at the end.
//
// Build/run: gcc -O2 -o /tmp/an12test tools/usbdump/an12test.c -lm
//            /tmp/an12test
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
    /* 48V AN1 + gain ~35 dB; AN2 -> AN1/2 at 0x16A0 (AN1 NOT routed) */
    ctl(0x17, 0x000D, 0x003F); ctl(0x21, 0x0000, 0x0000);
    ctl(0x1A, (11 & 0x1F) | 0x20, 0x0000);
    ctl(0x12, 0x16A0, 0x0035 | 0xC000);   /* AN2 L -> AN1/2 L */
    ctl(0x12, 0x16A0, 0x004F | 0xC000);   /* AN2 R -> AN1/2 R */
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, 0x2000, 0x03E0 | 0xC000);
    ctl(0x12, 0x2000, 0x03E1 | 0xC000);

    printf(">>> AN 1>2 + MS proc test — mic on AN1, listening via AN2 <<<\n");
    printf(">>> [4s] AN 1>2 OFF: TALK -> should be silent (AN2 has no signal) <<<\n");
    wait_ms(4000);
    ctl(0x17, 0x1400, 0x1000); ctl(0x21, 0x0000, 0x0000);
    printf(">>> [5s] AN 1>2 ON:  TALK -> you should HEAR the mic (via AN2) <<<\n");
    wait_ms(5000);
    ctl(0x12, 0x0000, 0x0001); ctl(0x12, 0x0000, 0x0035);
    ctl(0x12, 0x0000, 0x001B); ctl(0x12, 0x0000, 0x004F);
    printf(">>> [5s] MS proc ON:  TALK -> the mic should CUT (L+R) <<<\n");
    wait_ms(5000);
    ctl(0x12, 0x16A0, 0x0001); ctl(0x12, 0x16A0, 0x0035);
    ctl(0x12, 0x16A0, 0x001B); ctl(0x12, 0x16A0, 0x004F);
    printf(">>> [4s] MS proc OFF: TALK -> mic back <<<\n");
    wait_ms(4000);
    ctl(0x17, 0x0400, 0x1000); ctl(0x21, 0x0000, 0x0000);
    printf(">>> [3s] AN 1>2 OFF: silent again <<<\n");
    wait_ms(3000);
    printf(">>> DONE — 48V AN1 + gain 35 left ON <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
