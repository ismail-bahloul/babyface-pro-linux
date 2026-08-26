// Quick 0x17 readback probe — same parameters as the kernel worker:
// bmRequestType 0xC0, bRequest 0x17, wIndex 0x0000, 4 bytes.
// Usage: sudo ./v17probe /dev/bus/usb/BBB/DDD
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
    unsigned char b[4] = {0};
    int i;

    if (argc < 2) { fprintf(stderr, "usage: %s /dev/bus/usb/BBB/DDD\n", argv[0]); return 1; }
    fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    for (i = 0; i < 20; i++) {
        memset(&c, 0, sizeof(c));
        memset(b, 0, sizeof(b));
        c.bRequestType = 0xC0;
        c.bRequest = 0x17;
        c.wLength = 4;
        c.timeout = 1000;
        c.data = b;
        if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)
            printf("0x17 = %02X %02X %02X %02X\n", b[0], b[1], b[2], b[3]);
        else
            printf("read failed errno=%d\n", errno);
        usleep(20000);
    }
    close(fd);
    return 0;
}
