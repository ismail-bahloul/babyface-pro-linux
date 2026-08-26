#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#define DEV "/dev/bus/usb/003/002"
static int fd;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0x40; c.bRequest=req; c.wValue=val; c.wIndex=idx; c.timeout=1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl errno=%d\n", errno);
}
static void v17(const char *s) {
    unsigned char b[4]={0}; struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0xC0; c.bRequest=0x17; c.wLength=4; c.timeout=1000; c.data=b;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)
        printf("  %-36s 0x17=%02X %02X %02X %02X\n", s, b[0], b[1], b[2], b[3]);
}
int main(void) {
    fd = open(DEV, O_RDWR);
    v17("baseline (0x0001 keepalive)");
    ctl(0x10, 0x0004, 0x05CF);   /* clock source = Optical */
    v17("after 0x10 0x0004 0x05CF (expect byte2=0x80)");
    sleep(2);
    v17("after 2s");
    ctl(0x10, 0x0001, 0x05CF);   /* back to Internal */
    v17("after 0x10 0x0001 0x05CF (expect byte2=0x40)");
    close(fd);
    return 0;
}
