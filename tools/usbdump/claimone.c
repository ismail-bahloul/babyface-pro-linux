// Claim each interface individually (release first) and read 0x17 after
// each — which claim shifts the byte2 bits?
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
static void claim(int i) {
    int r = ioctl(fd, USBDEVFS_CLAIMINTERFACE, &i);
    printf("  claim iface %d: %s\n", i, r == 0 ? "ok" : "fail");
}
static void release(int i) { ioctl(fd, USBDEVFS_RELEASEINTERFACE, &i); }
int main(void) {
    fd = open(DEV, O_RDWR);
    v17("start");
    claim(0); v17("after claim 0");
    claim(1); v17("after claim 1");
    claim(2); v17("after claim 2");
    claim(3); v17("after claim 3");
    claim(4); v17("after claim 4");
    claim(5); v17("after claim 5");
    close(fd);
    return 0;
}
