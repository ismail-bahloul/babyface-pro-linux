// 0x17 readback sweep over wIndex — find which indices return what.
// Usage: sudo ./v17sweep /dev/bus/usb/BBB/DDD
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

int main(int argc, char **argv)
{
    int fd;
    struct usbdevfs_ctrltransfer c;
    unsigned char b[4];
    int idx, n;

    if (argc < 2) { fprintf(stderr, "usage: %s /dev/bus/usb/BBB/DDD\n", argv[0]); return 1; }
    fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    for (idx = 0; idx <= 0x3f; idx += 1) {
        int good = 0;
        for (n = 0; n < 2; n++) {
            memset(&c, 0, sizeof(c));
            memset(b, 0, sizeof(b));
            c.bRequestType = 0xC0;
            c.bRequest = 0x17;
            c.wIndex = idx;
            c.wLength = 4;
            c.timeout = 1000;
            c.data = b;
            if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)
                good = 1;
        }
        if (good)
            printf("idx=0x%02X  0x17 = %02X %02X %02X %02X\n", idx, b[0], b[1], b[2], b[3]);
    }
    close(fd);
    return 0;
}
