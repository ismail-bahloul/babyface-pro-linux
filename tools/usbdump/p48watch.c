// Toggle phantom on AN1 via the kernel driver and watch the 0x17 readback
// at wIdx 0x0000 vs 0x003F, to pin the byte0 encoding (raw bits vs 0x0C base).
// Usage: sudo ./p48watch /dev/bus/usb/BBB/DDD
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

static int rd17(int fd, int idx, unsigned char *b)
{
    struct usbdevfs_ctrltransfer c;
    memset(&c, 0, sizeof(c));
    memset(b, 0, 4);
    c.bRequestType = 0xC0;
    c.bRequest = 0x17;
    c.wIndex = idx;
    c.wLength = 4;
    c.timeout = 1000;
    c.data = b;
    return ioctl(fd, USBDEVFS_CONTROL, &c) == 4;
}

int main(int argc, char **argv)
{
    int fd;
    unsigned char b0[4], b3f[4];

    if (argc < 2) { fprintf(stderr, "usage: %s /dev/bus/usb/BBB/DDD\n", argv[0]); return 1; }
    fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    /* 5 s of polling: the user toggles 48V AN1 via the GUI/TUI or amixer
     * while we watch both readback indices. */
    printf("watching 48V AN1 toggles for 5s... (toggle via: amixer -c 3 cset 'name=Phantom Power Mic 1' on/off)\n");
    for (int i = 0; i < 250; i++) {
        if (rd17(fd, 0x0000, b0) && rd17(fd, 0x003f, b3f))
            printf("idx0=%02X %02X %02X %02X   idx3F=%02X %02X %02X %02X\n",
                   b0[0], b0[1], b0[2], b0[3], b3f[0], b3f[1], b3f[2], b3f[3]);
        usleep(20000);
    }
    close(fd);
    return 0;
}
