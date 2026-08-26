// Exact cap_audio session-start order from the current state (0x40):
// trigger -> 9x OUT URBs -> 9x IN URBs -> 0x14 arm. Watch byte 2 for 0x80.
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
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl req=%02x errno=%d\n", req, errno);
}
static void v17(const char *s) {
    unsigned char b[4]={0}; struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0xC0; c.bRequest=0x17; c.wLength=4; c.timeout=1000; c.data=b;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)
        printf("  %-38s 0x17=%02X %02X %02X %02X\n", s, b[0], b[1], b[2], b[3]);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
int main(void) {
    fd = open(DEV, O_RDWR);
    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    struct usbdevfs_setinterface si = {5, 1}; ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    v17("start");

    ctl(0x10, 0x0000, 0x8000);   /* trigger */
    ctl(0x1D, 0x0000, 0x0000);
    v17("after trigger");

    struct usbdevfs_urb *outs[NQ], *ins[NQ];
    for (int i = 0; i < NQ; i++) {
        outs[i] = calloc(1, sizeof(*outs[i]));
        outs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        outs[i]->endpoint = 0x01;
        outs[i]->buffer = calloc(1, LEN);
        outs[i]->buffer_length = LEN;
        for (int f = 0; f < LEN / 56; f++) {
            unsigned int *fr = (unsigned int *)((char *)outs[i]->buffer + f * 56);
            fr[4] = 0x00000020; fr[5] = 0x00000020;
        }
        ioctl(fd, USBDEVFS_SUBMITURB, outs[i]);   /* 9x OUT first */
    }
    for (int i = 0; i < NQ; i++) {
        ins[i] = calloc(1, sizeof(*ins[i]));
        ins[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        ins[i]->endpoint = 0x82;
        ins[i]->buffer = calloc(1, LEN);
        ins[i]->buffer_length = LEN;
        ioctl(fd, USBDEVFS_SUBMITURB, ins[i]);    /* then 9x IN */
    }
    ctl(0x14, 0x0000, 0xC000);   /* arm */
    ctl(0x17, 0x000D, 0x003F);   /* 48V ON */
    ctl(0x21, 0x0000, 0x0000);
    v17("URBs + arm + 48V");

    long long t0 = us();
    int last = 0;
    while (us() - t0 < 8000000) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) ioctl(fd, USBDEVFS_SUBMITURB, done);
        if (us() - t0 >= 2000000LL * (last + 1)) { last++; v17("streaming"); }
    }
    close(fd);
    return 0;
}
