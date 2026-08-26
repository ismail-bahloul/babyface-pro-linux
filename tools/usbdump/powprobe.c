// Minimal read-only state probe — run IMMEDIATELY after a power cycle,
// before anything writes to the device. Prints 0x17 + the poll-cycle
// registers the Windows driver reads, and the audio-relevant ones.
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
    int regs[] = {0x10, 0x11, 0x17, 0x19, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
    for (int i = 0; i < (int)(sizeof(regs) / sizeof(regs[0])); i++) {
        unsigned char buf[4] = {0};
        struct usbdevfs_ctrltransfer c;
        memset(&c, 0, sizeof(c));
        c.bRequestType = 0xC0;
        c.bRequest = regs[i];
        c.wLength = 4;
        c.timeout = 1000;
        c.data = buf;
        errno = 0;
        if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)
            printf("reg %02X = %02X %02X %02X %02X\n", regs[i], buf[0], buf[1], buf[2], buf[3]);
        else
            printf("reg %02X = errno %d\n", regs[i], errno);
    }
    close(fd);
    return 0;
}
