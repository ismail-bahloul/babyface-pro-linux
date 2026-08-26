// Single-value 48V probe: write ONE wVal to 0x17 0x003F + 0x21, wait
// `sec` seconds, then restore AN1 48V ON (0x0D). Run one at a time.
#include <stdio.h>
#include <stdlib.h>
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
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl req=%02x errno=%d\n", req, errno);
}
static void v17(const char *s) {
    unsigned char b[4]={0}; struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0xC0; c.bRequest=0x17; c.wLength=4; c.timeout=1000; c.data=b;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)
        printf("  %-24s 0x17=%02X %02X %02X %02X\n", s, b[0], b[1], b[2], b[3]);
}
int main(int argc, char **argv) {
    int val = argc > 1 ? (int)strtol(argv[1], NULL, 16) : 0x0D;
    int sec = argc > 2 ? atoi(argv[2]) : 8;
    fd = open(DEV, O_RDWR);
    printf(">>> 48V 0x%02X — WATCH THE P48 LEDs (%ds) <<<\n", val, sec);
    ctl(0x17, val, 0x003F);
    ctl(0x21, 0x0000, 0x0000);
    v17("readback");
    sleep(sec);
    ctl(0x17, 0x000D, 0x003F);   /* restore AN1 48V ON */
    ctl(0x21, 0x0000, 0x0000);
    v17("restored AN1 ON");
    close(fd);
    return 0;
}
