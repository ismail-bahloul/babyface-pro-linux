// an2sweep.c — isolate why AN2 is silent: sweep the AN2 gain (0, 35,
// 65 dB) then toggle 48V OFF/ON at max gain while measuring IN ch1
// (AN2). If the level follows the gain, the preamp works and the mic is
// just quiet; if it collapses when 48V is turned OFF, the phantom was
// powering the mic/FetHead; if it never moves, the connection is bad.
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
static void measure(int ms, const char *label) {
    long long end = us() + ms * 1000;
    double p1 = 0.0;
    while (us() < end) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
            if (done->endpoint == 0x82) {
                unsigned char *b = done->buffer;
                int len = done->actual_length;
                for (int f = 0; f < len / FRAME; f++) {
                    unsigned word = (unsigned)b[f * FRAME + 5]
                                  | ((unsigned)b[f * FRAME + 6] << 8)
                                  | ((unsigned)b[f * FRAME + 7] << 16);
                    double mag = fabs((int)(word << 8) >> 8) / FULL;
                    if (mag > p1) p1 = mag;
                }
            }
            ioctl(fd, USBDEVFS_SUBMITURB, done);
        }
    }
    printf("  %-34s ch1(AN2): %6.1f dBFS\n", label, 20 * log10(p1 + 1e-9));
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
    ctl(0x17, 0x000E, 0x003F); ctl(0x21, 0x0000, 0x0000);  /* 48V AN2 ON */

    printf(">>> AN2 gain/48V sweep — RUB the mic on AN2 continuously <<<\n");
    ctl(0x1A, 0x20, 0x0001);                     /* gain 0 dB */
    measure(2500, "gain 0 dB   (48V ON)");
    ctl(0x1A, (11 & 0x1F) | 0x20, 0x0001);       /* gain ~35 dB */
    measure(2500, "gain 35 dB  (48V ON)");
    ctl(0x1A, (20 & 0x1F) | 0x20, 0x0001);       /* gain ~65 dB */
    measure(2500, "gain 65 dB  (48V ON)");
    ctl(0x17, 0x000C, 0x003F); ctl(0x21, 0x0000, 0x0000);  /* 48V AN2 OFF */
    measure(2500, "gain 65 dB  (48V OFF!)");
    ctl(0x17, 0x000E, 0x003F); ctl(0x21, 0x0000, 0x0000);  /* 48V AN2 ON */
    measure(2500, "gain 65 dB  (48V ON again)");
    printf(">>> DONE <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
