// Dump raw IN frames: print the first 3 frames (14 words each) of the
// first few URBs, plus the distribution of ch0 values.
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
#define NQ 9

static int fd;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0x40; c.bRequest=req; c.wValue=val; c.wIndex=idx; c.timeout=1000;
    ioctl(fd, USBDEVFS_CONTROL, &c);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
int main(void) {
    fd = open(DEV, O_RDWR);
    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    struct usbdevfs_setinterface si = {5, 1}; ioctl(fd, USBDEVFS_SETINTERFACE, &si);
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
    ctl(0x17, 0x000D, 0x003F);   /* 48V ON */
    ctl(0x21, 0x0000, 0x0000);
    ctl(0x1A, 0x002A, 0x0000);   /* gain raw 10 (0x0A + counter 0x20) */
    printf("streaming, 48V ON, gain 10. Raw frames:\n");
    long long t0 = us();
    int urbs = 0;
    while (us() - t0 < 3000000 && urbs < 4) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
            if (done->endpoint == 0x82 && done->actual_length >= 168) {
                unsigned char *b = (unsigned char *)done->buffer;
                printf("--- URB actual=%d, frame0 words:\n", done->actual_length);
                for (int w = 0; w < 14; w++) {
                    unsigned char *p = b + w * 4;
                    printf("  w%02d = %02X %02X %02X %02X  (u32=0x%08X)\n", w,
                           p[0], p[1], p[2], p[3],
                           (unsigned)(p[0] | p[1] << 8 | p[2] << 16 | p[3] << 24));
                }
                urbs++;
            }
            ioctl(fd, USBDEVFS_SUBMITURB, done);
        }
    }
    close(fd);
    return 0;
}
