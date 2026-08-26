// From 0x47: send single 0x10 writes and watch the byte2 counter.
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
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
        printf("  %-36s 0x17=%02X %02X %02X %02X\n", s, b[0], b[1], b[2], b[3]);
}
int main(void) {
    fd = open(DEV, O_RDWR);
    v17("start");
    ctl(0x10, 0x0800, 0x0800); v17("0x10 0x0800 0x0800 #1");
    ctl(0x10, 0x0800, 0x0800); v17("0x10 0x0800 0x0800 #2");
    ctl(0x10, 0x0800, 0x0800); v17("0x10 0x0800 0x0800 #3");
    ctl(0x10, 0x0000, 0x3000); v17("0x10 0x3000 #1");
    ctl(0x10, 0x0000, 0x3000); v17("0x10 0x3000 #2");
    ctl(0x10, 0x0000, 0x3000); v17("0x10 0x3000 #3");
    ctl(0x10, 0x0000, 0x3000); v17("0x10 0x3000 #4");
    close(fd); return 0;
}
