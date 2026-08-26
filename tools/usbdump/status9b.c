// Does the 9-byte status stream flow after the FULL coldplug init burst?
// coldplug order: 0x16 clears -> 0x1B -> 0x10 0x0021 0x05FF -> 0x17 0x000C
// 0x0000 -> 0x21 -> 0x10 0x3000 x2 -> 0x10 0x0800 0x0800 x3 -> then 9B polls.
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
        printf("  %-32s 0x17=%02X %02X %02X %02X\n", s, b[0], b[1], b[2], b[3]);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
int main(void) {
    fd = open(DEV, O_RDWR);
    struct usbdevfs_setinterface si = {5, 1};
    ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    v17("before init");
    for (int i = 0; i <= 0x3D; i++) { if (i == 0x1E || i == 0x1F) continue; ctl(0x16, 0x0000, i); }
    ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);
    ctl(0x1B, 0x8234, 0xD302); ctl(0x1B, 0x7CFF, 0xF803);
    ctl(0x10, 0x0021, 0x05FF);
    ctl(0x17, 0x000C, 0x0000); ctl(0x21, 0x0000, 0x0000);
    ctl(0x10, 0x0000, 0x3000); ctl(0x10, 0x0000, 0x3000);
    ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800);
    v17("after full init burst");

    struct usbdevfs_urb *u[4], *d[4];
    for (int i = 0; i < 4; i++) {
        u[i] = calloc(1, sizeof(*u[i]));
        u[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        u[i]->endpoint = 0x82;
        u[i]->buffer = calloc(1, 9);
        u[i]->buffer_length = 9;
        d[i] = calloc(1, sizeof(*d[i]) + 9);
        ioctl(fd, USBDEVFS_SUBMITURB, u[i]);
    }
    long long t0 = us();
    int n = 0;
    while (us() - t0 < 3000000) {
        for (int i = 0; i < 4; i++) {
            errno = 0;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, d[i]) == 0) {
                unsigned char *b = (unsigned char *)d[i]->buffer;
                if (n++ < 10)
                    printf("  9B actual=%d data=%02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                           d[i]->actual_length, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8]);
                ioctl(fd, USBDEVFS_SUBMITURB, u[i]);
            }
        }
    }
    printf("  (9B completions: %d in 3s)\n", n);
    v17("after 9B polling");
    close(fd);
    return 0;
}
