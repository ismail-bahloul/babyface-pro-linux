// eqbulk.c — userspace bulk EQ upload test (usbfs).
// Claims interface 1, bulk OUT the two EQ blocks (L/R) for a bell
// +6 dB @ 440 Hz Q=0.7 on strip AN1 (ch 0/1), then releases.
// Usage: eqbulk <bus/dev> <on|off>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>
#include <stdint.h>

static void fill_block(uint8_t *b, int ch, const int32_t *slot1, int32_t c4)
{
    memset(b, 0, 64);
    b[0] = ch; b[1] = 0; b[2] = ch; b[3] = 0x80;
    for (int k = 0; k < 4; k++) {
        uint32_t v = (uint32_t)slot1[k];
        b[0x04 + 4 * k] = v & 0xff;
        b[0x04 + 4 * k + 1] = (v >> 8) & 0xff;
        b[0x04 + 4 * k + 2] = (v >> 16) & 0xff;
        b[0x04 + 4 * k + 3] = (v >> 24) & 0xff;
    }
    uint32_t v = (uint32_t)c4;
    b[0x34] = v & 0xff; b[0x35] = (v >> 8) & 0xff;
    b[0x36] = (v >> 16) & 0xff; b[0x37] = (v >> 24) & 0xff;
    b[0x38] = 0; b[0x39] = 0; b[0x3a] = 0; b[0x3b] = 0x04; /* low cut off */
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s bus/dev on|off\n", argv[0]);
        return 2;
    }
    char path[64];
    snprintf(path, sizeof(path), "/dev/bus/usb/%s", argv[1]);
    int fd = open(path, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    /* detach any kernel driver on interface 1, then claim it */
    unsigned int iface = 1;
    struct usbdevfs_getdriver gd = { .interface = 1 };
    if (ioctl(fd, USBDEVFS_GETDRIVER, &gd) == 0 && strcmp(gd.driver, "snd-usb-babyface-pro") == 0) {
        if (ioctl(fd, USBDEVFS_DISCONNECT, 1) < 0) perror("disconnect iface1");
    }
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
        perror("claim iface1");
        close(fd);
        return 1;
    }

    /* bell +6 dB @ 440 Hz, Q=0.7 (computed with the driver's RBJ words) */
    int32_t on_slot[4] = { -0xfd0a0c6, 0x7d407d6, -0xf857a78, 0x78543e8 };
    int32_t on_c4 = 0x0839a76c;
    int32_t off_slot[4] = { 0, 0, 0, 0 };
    int32_t off_c4 = 0x08000000;

    for (int ch = 0; ch < 2; ch++) {
        uint8_t blk[64];
        fill_block(blk, ch, (argv[2][0] == 'o') ? on_slot : off_slot,
                   (argv[2][0] == 'o') ? on_c4 : off_c4);
        struct usbdevfs_bulktransfer bt = {
            .ep = 0x0a, .len = 64, .data = blk, .timeout = 1000,
        };
        int n = ioctl(fd, USBDEVFS_BULK, &bt);
        printf("ch%d bulk -> %d\n", ch, n);
        usleep(50000);
    }

    ioctl(fd, USBDEVFS_RELEASEINTERFACE, &iface);
    close(fd);
    return 0;
}
