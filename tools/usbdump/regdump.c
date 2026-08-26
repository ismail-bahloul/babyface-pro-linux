// Register dump: read 0x00-0x3F (4-byte vendor reads) — look for clock
// / sample-rate state (e.g. 0x1B-related values).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#define DEV "/dev/bus/usb/003/002"

int main(void) {
    int fd = open(DEV, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    for (int r = 0; r <= 0x3F; r++) {
        unsigned char buf[4] = {0};
        struct usbdevfs_ctrltransfer c;
        memset(&c, 0, sizeof(c));
        c.bRequestType = 0xC0;
        c.bRequest = r;
        c.wLength = 4;
        c.timeout = 1000;
        c.data = buf;
        errno = 0;
        int n = ioctl(fd, USBDEVFS_CONTROL, &c);
        if (n == 4)
            printf("reg %02X = %02X %02X %02X %02X\n", r, buf[0], buf[1], buf[2], buf[3]);
        else if (n > 0)
            printf("reg %02X = %d bytes: %02X %02X %02X %02X\n", r, n, buf[0], buf[1], buf[2], buf[3]);
        else if (errno == 32)
            printf("reg %02X = EPIPE (stall)\n", r);
        else if (errno != 0 && errno != 22)
            printf("reg %02X = errno %d\n", r, errno);
    }
    close(fd);
    return 0;
}
