// Claim ALL interfaces (0-5) + set alt-settings like the Windows driver
// would, then run the init burst and read 0x17 — does byte2 go 0x41?
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
        printf("  %-40s 0x17=%02X %02X %02X %02X\n", s, b[0], b[1], b[2], b[3]);
}
int main(void) {
    fd = open(DEV, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    for (int i = 0; i <= 5; i++) {
        int r = ioctl(fd, USBDEVFS_CLAIMINTERFACE, &i);
        printf("claim iface %d: %s\n", i, r == 0 ? "ok" : "failed");
    }
    struct usbdevfs_setinterface si = {5, 1};
    ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    printf("set iface 5 alt 1\n");
    v17("after claim + setif");
    for (int i = 0; i <= 0x3D; i++) { if (i == 0x1E || i == 0x1F) continue; ctl(0x16, 0x0000, i); }
    ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);
    ctl(0x1B, 0x8234, 0xD302); ctl(0x1B, 0x7CFF, 0xF803);
    ctl(0x10, 0x0021, 0x05FF);
    ctl(0x17, 0x000C, 0x0000); ctl(0x21, 0x0000, 0x0000);
    ctl(0x10, 0x0000, 0x3000); ctl(0x10, 0x0000, 0x3000);
    ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800);
    v17("after full init burst");
    /* 0x15 clears + 0x17 0x8080 (coldplug state-restore start) */
    for (int i = 0; i <= 0x1D; i++) ctl(0x15, 0x0000, i);
    ctl(0x17, 0x0000, 0x8080);
    v17("after 0x15 clears + 0x17 0x8080");
    /* the tail writes of coldplug's restore */
    ctl(0x17, 0x000C, 0x003F); ctl(0x21, 0x0000, 0x0000);
    ctl(0x1A, 0x0000, 0x0000); ctl(0x1A, 0x0000, 0x0001);
    ctl(0x1A, 0x0000, 0x0002); ctl(0x1A, 0x0000, 0x0003);
    ctl(0x17, 0x0000, 0xF040);
    v17("after restore tail (0x1A + 0x17 0xF040)");
    close(fd);
    return 0;
}
