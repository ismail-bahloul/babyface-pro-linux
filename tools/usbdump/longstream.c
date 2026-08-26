// Long streaming test from the current state: full init + trigger +
// URBs + arm + 48V ON, stream 10s, print 0x17 every 2s.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/usbdevice_fs.h>
#define DEV "/dev/bus/usb/003/004"
#define LEN 14336
#define NQ 9

static int fd;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0x40; c.bRequest=req; c.wValue=val; c.wIndex=idx; c.timeout=1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl req=%02x errno=%d\n", req, errno);
}
static void v17(const char *s) {
    unsigned char b[4]={0}; struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0xC0; c.bRequest=0x17; c.wLength=4; c.timeout=1000; c.data=b;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)
        printf("  %-34s 0x17=%02X %02X %02X %02X\n", s, b[0], b[1], b[2], b[3]);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
int main(void) {
    fd = open(DEV, O_RDWR);
    struct usbdevfs_setinterface si = {5, 1};
    ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    for (int i = 0; i <= 0x3D; i++) { if (i == 0x1E || i == 0x1F) continue; ctl(0x16, 0x0000, i); }
    ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);
    ctl(0x1B, 0x8234, 0xD302); ctl(0x1B, 0x7CFF, 0xF803);
    ctl(0x10, 0x0021, 0x05FF);
    ctl(0x17, 0x000C, 0x0000); ctl(0x21, 0x0000, 0x0000);
    ctl(0x10, 0x0000, 0x3000); ctl(0x10, 0x0000, 0x3000);
    ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800);
    v17("after init");

    ctl(0x10, 0x0000, 0x8000);
    ctl(0x1D, 0x0000, 0x0000);
    struct usbdevfs_urb *outs[NQ], *ins[NQ], *do_[NQ], *di[NQ];
    for (int i = 0; i < NQ; i++) {
        outs[i] = calloc(1, sizeof(*outs[i]));
        outs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        outs[i]->endpoint = 0x01;
        outs[i]->buffer = calloc(1, LEN);
        outs[i]->buffer_length = LEN;
        for (int f = 0; f < LEN / 56; f++) {
            unsigned int *fr = (unsigned int *)((char *)outs[i]->buffer + f * 56);
            fr[4] = 0x20000000; fr[5] = 0x20000000;
        }
        ins[i] = calloc(1, sizeof(*ins[i]));
        ins[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        ins[i]->endpoint = 0x82;
        ins[i]->buffer = calloc(1, LEN);
        ins[i]->buffer_length = LEN;
        do_[i] = calloc(1, sizeof(*do_[i]) + LEN);
        di[i] = calloc(1, sizeof(*di[i]) + LEN);
        ioctl(fd, USBDEVFS_SUBMITURB, outs[i]);
        ioctl(fd, USBDEVFS_SUBMITURB, ins[i]);
    }
    ctl(0x14, 0x0000, 0xC000);
    ctl(0x17, 0x000D, 0x003F);   /* 48V ON */
    ctl(0x21, 0x0000, 0x0000);
    printf(">>> STREAMING + 48V ON — WATCH THE LEDS (10s) <<<\n");
    long long t0 = us();
    int last = 0;
    while (us() - t0 < 10000000) {
        for (int i = 0; i < NQ; i++) {
            errno = 0;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, di[i]) == 0) ioctl(fd, USBDEVFS_SUBMITURB, ins[i]);
            errno = 0;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, do_[i]) == 0) ioctl(fd, USBDEVFS_SUBMITURB, outs[i]);
        }
        if (us() - t0 >= 2000000LL * (last + 1)) { last++; v17("streaming"); }
    }
    v17("after stream (before stop)");
    ctl(0x13, 0x0000, 0xC000);   /* session stop */
    v17("after 0x13 stop");
    close(fd);
    return 0;
}
