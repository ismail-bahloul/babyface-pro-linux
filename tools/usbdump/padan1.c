// AN1 PAD test (bit 4), mic on AN1: 48V AN1 (0x0D), PAD on = 0x1D.
// Measure ch0 RMS pad off/on/off; the user listens for the relay click.
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
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0x40; c.bRequest=req; c.wValue=val; c.wIndex=idx; c.timeout=1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl req=%02x errno=%d\n", req, errno);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
static double rms(int ms) {
    long long t0 = us();
    long long sum = 0, n = 0;
    while (us() - t0 < ms * 1000) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
            if (done->endpoint == 0x82 && done->actual_length >= 56) {
                unsigned int *fr = (unsigned int *)done->buffer;
                for (int f = 0; f < done->actual_length / 56; f++) {
                    int v = ((int)fr[f * 14]) >> 8;   /* ch0 = AN1 */
                    sum += (long long)v * v;
                    n++;
                }
            }
            ioctl(fd, USBDEVFS_SUBMITURB, done);
        }
    }
    return n ? sqrt((double)sum / n) : 0;
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
    /* 48V AN1 (bit 0), gain raw 17, routing AN1 -> AN1/2, master 0x4000 */
    ctl(0x17, 0x000D, 0x003F); ctl(0x21, 0x0000, 0x0000);
    ctl(0x1A, (17 & 0x1F) | 0x20, 0x0000);
    ctl(0x12, 0x4000, 0x0034 | 0xC000);
    ctl(0x12, 0x4000, 0x004E | 0xC000);
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, 0x4000, 0x03E0 | 0xC000);
    ctl(0x12, 0x4000, 0x03E1 | 0xC000);

    printf("streaming, 48V AN1, gain 17. KEEP QUIET (noise-floor measurement)!\n");
    printf("  PAD off:   RMS=%9.1f\n", rms(3000));
    ctl(0x17, 0x000D | 0x10, 0x003F);   /* + PAD AN1 (bit 4) = 0x1D */
    ctl(0x21, 0x0000, 0x0000);
    printf(">>> PAD AN1 ON (0x1D) — LISTEN FOR THE CLICK <<<\n");
    printf("  PAD on:    RMS=%9.1f\n", rms(3000));
    ctl(0x17, 0x000D, 0x003F);          /* PAD off */
    ctl(0x21, 0x0000, 0x0000);
    printf(">>> PAD AN1 OFF (0x0D) — LISTEN FOR THE CLICK <<<\n");
    printf("  PAD off 2: RMS=%9.1f\n", rms(3000));
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
