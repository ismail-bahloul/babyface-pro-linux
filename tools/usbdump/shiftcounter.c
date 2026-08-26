// Test the byte2 shift-counter theory: repeatedly SET_INTERFACE(5,1)
// (with releases) to drive 0x4F -> 0x47 -> 0x43 -> 0x41, reading 0x17
// after each step.
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
        printf("  %-44s 0x17=%02X %02X %02X %02X\n", s, b[0], b[1], b[2], b[3]);
}
static void setif_alt1(void) {
    struct usbdevfs_setinterface si = {5, 1};
    ioctl(fd, USBDEVFS_SETINTERFACE, &si);
}
int main(void) {
    fd = open(DEV, O_RDWR);
    int iface = 5;
    ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    v17("start");
    setif_alt1(); v17("SET_INTERFACE(5,1) #1");
    setif_alt1(); v17("SET_INTERFACE(5,1) #2");
    setif_alt1(); v17("SET_INTERFACE(5,1) #3");
    setif_alt1(); v17("SET_INTERFACE(5,1) #4");
    setif_alt1(); v17("SET_INTERFACE(5,1) #5");
    setif_alt1(); v17("SET_INTERFACE(5,1) #6");
    setif_alt1(); v17("SET_INTERFACE(5,1) #7");
    setif_alt1(); v17("SET_INTERFACE(5,1) #8");
    /* also try alt 0 -> alt 1 transitions */
    struct usbdevfs_setinterface s0 = {5, 0};
    ioctl(fd, USBDEVFS_SETINTERFACE, &s0); v17("alt 0");
    setif_alt1(); v17("back to alt 1");
    close(fd);
    return 0;
}
