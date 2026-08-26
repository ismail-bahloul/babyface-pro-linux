// v17watch.c — watch the 0x17 readback (wIdx 0) continuously, printing
// only CHANGES, at ~50 Hz.  Usage: sudo ./v17watch /dev/bus/usb/BBB/DDD [hz]
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
    int fd, n = 0;
    struct usbdevfs_ctrltransfer c;
    unsigned char b[4], last[4] = {0xff, 0xff, 0xff, 0xff};
    int hz = argc > 2 ? atoi(argv[2]) : 50;

    if (argc < 2) { fprintf(stderr, "usage: %s /dev/bus/usb/BBB/DDD [hz]\n", argv[0]); return 1; }
    fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    while (1) {
        memset(&c, 0, sizeof(c));
        memset(b, 0, sizeof(b));
        c.bRequestType = 0xC0;
        c.bRequest = 0x17;
        c.wLength = 4;
        c.timeout = 1000;
        c.data = b;
        if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4) {
            if (memcmp(b, last, 4)) {
                printf("%8.3f 0x17 = %02X %02X %02X %02X%s%s%s\n",
                       0.0 + n / (double)hz, b[0], b[1], b[2], b[3],
                       b[3] != 0x40 ? "  <-- byte3 flash" : "",
                       (b[1] & 0x20) ? "  DIM" : "",
                       (b[0] & 0x80) ? "  MIX" : "");
                fflush(stdout);
                memcpy(last, b, 4);
            }
        }
        usleep(1000000 / hz);
        n++;
    }
    close(fd);
    return 0;
}
