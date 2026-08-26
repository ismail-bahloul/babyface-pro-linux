// Bisect the init sequence: read 0x17 after EVERY write, so we can see
// exactly which step moves the device state (0x41 -> 0x43 -> ...).
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
    struct usbdevfs_ctrltransfer c;
    memset(&c, 0, sizeof(c));
    c.bRequestType = 0x40;
    c.bRequest = req;
    c.wValue = val;
    c.wIndex = idx;
    c.timeout = 1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("    ctl req=%02x errno=%d\n", req, errno);
}

static void v17(const char *step) {
    unsigned char buf[4] = {0};
    struct usbdevfs_ctrltransfer c;
    memset(&c, 0, sizeof(c));
    c.bRequestType = 0xC0;
    c.bRequest = 0x17;
    c.wLength = 4;
    c.timeout = 1000;
    c.data = buf;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)
        printf("  %-42s 0x17=%02X %02X %02X %02X\n", step, buf[0], buf[1], buf[2], buf[3]);
}

int main(void) {
    fd = open(DEV, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct usbdevfs_setinterface si = {5, 1};
    ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    v17("after SET_INTERFACE(5,1)");
    for (int i = 0; i <= 0x3D; i++) {
        if (i == 0x1E || i == 0x1F) continue;
        ctl(0x16, 0x0000, i);
    }
    v17("after 0x16 clears");
    ctl(0x1B, 0xC350, 0x0000);
    ctl(0x1B, 0x8DB8, 0xD201);
    ctl(0x1B, 0x8234, 0xD302);
    ctl(0x1B, 0x7CFF, 0xF803);
    v17("after 0x1B rate codes");
    ctl(0x10, 0x0021, 0x05FF);
    v17("after 0x10 0x0021 0x05FF");
    ctl(0x17, 0x000C, 0x0000);
    v17("after 0x17 0x000C 0x0000");
    ctl(0x21, 0x0000, 0x0000);
    v17("after 0x21");
    ctl(0x10, 0x0000, 0x3000);
    ctl(0x10, 0x0000, 0x3000);
    v17("after 0x10 0x3000 x2");
    ctl(0x10, 0x0800, 0x0800);
    ctl(0x10, 0x0800, 0x0800);
    ctl(0x10, 0x0800, 0x0800);
    v17("after 0x10 0x0800 0x0800 x3");
    ctl(0x10, 0x0000, 0x8000);
    ctl(0x1D, 0x0000, 0x0000);
    v17("after trigger 0x10 0x8000 + 0x1D");
    ctl(0x14, 0x0000, 0xC000);
    v17("after 0x14 0xC000 arm");
    close(fd);
    return 0;
}
