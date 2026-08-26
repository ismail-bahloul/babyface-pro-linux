// Set alt-settings on interfaces 0, 2, 5 (like Windows would) and watch
// the byte2 bit pattern: 0x47 (bits 1,2 set) -> ... -> 0x41?
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#define DEV "/dev/bus/usb/003/004"

static int fd;
static void v17(const char *s) {
    unsigned char b[4]={0}; struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0xC0; c.bRequest=0x17; c.wLength=4; c.timeout=1000; c.data=b;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)
        printf("  %-40s 0x17=%02X %02X %02X %02X\n", s, b[0], b[1], b[2], b[3]);
}
static void claim(int i) { ioctl(fd, USBDEVFS_CLAIMINTERFACE, &i); }
static void setif(int i, int a) {
    struct usbdevfs_setinterface si = {i, a};
    int r = ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    printf("  SET_INTERFACE(%d,%d) -> %s\n", i, a, r == 0 ? "ok" : "fail");
}
int main(void) {
    fd = open(DEV, O_RDWR);
    for (int i = 0; i <= 5; i++) claim(i);
    v17("all claimed");
    setif(0, 0); v17("iface0 alt0");
    setif(2, 0); v17("iface2 alt0");
    setif(5, 1); v17("iface5 alt1");
    setif(0, 1); v17("iface0 alt1");
    setif(2, 1); v17("iface2 alt1 (if exists)");
    setif(5, 1); v17("iface5 alt1 again");
    setif(1, 0); setif(3, 0); setif(4, 0);
    v17("ifaces 1,3,4 alt0");
    close(fd);
    return 0;
}
