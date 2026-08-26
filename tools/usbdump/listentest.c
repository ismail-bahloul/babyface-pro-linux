// Full listen test: init + stream + 48V ON + gain raw 5 (~35 dB) +
// routing AN1 -> AN1/2 at 0x4000 + master unmuted at 0x4000.
// Speak into the mic, listen to the headphones.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/usbdevice_fs.h>
#define DEV "/dev/bus/usb/003/002"
#define LEN 14336
#define NQ 9

static int fd;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0x40; c.bRequest=req; c.wValue=val; c.wIndex=idx; c.timeout=1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl req=%02x errno=%d\n", req, errno);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
int main(int argc, char **argv) {
    int src = argc > 1 ? atoi(argv[1]) : 1;     /* input channel 1 or 2 */
    int gval = argc > 2 ? atoi(argv[2]) : 17;   /* raw gain value */
    int gsec = argc > 3 ? atoi(argv[3]) : 15;   /* seconds */
    int p48val = src == 1 ? 0x0D : 0x0E;        /* bit0 = AN1, bit1 = AN2 */
    fd = open(DEV, O_RDWR);
    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    struct usbdevfs_setinterface si = {5, 1}; ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    /* init burst */
    for (int i = 0; i <= 0x3D; i++) { if (i == 0x1E || i == 0x1F) continue; ctl(0x16, 0x0000, i); }
    ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);
    ctl(0x1B, 0x8234, 0xD302); ctl(0x1B, 0x7CFF, 0xF803);
    ctl(0x10, 0x0021, 0x05FF);
    ctl(0x17, 0x000C, 0x0000); ctl(0x21, 0x0000, 0x0000);
    ctl(0x10, 0x0000, 0x3000); ctl(0x10, 0x0000, 0x3000);
    ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800);
    /* session */
    ctl(0x10, 0x0000, 0x8000);
    ctl(0x1D, 0x0000, 0x0000);
    struct usbdevfs_urb *outs[NQ], *ins[NQ];
    for (int i = 0; i < NQ; i++) {
        outs[i] = calloc(1, sizeof(*outs[i]));
        outs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        outs[i]->endpoint = 0x01;
        outs[i]->buffer = calloc(1, LEN);
        outs[i]->buffer_length = LEN;
        ioctl(fd, USBDEVFS_SUBMITURB, outs[i]);
    }
    for (int i = 0; i < NQ; i++) {
        ins[i] = calloc(1, sizeof(*ins[i]));
        ins[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        ins[i]->endpoint = 0x82;
        ins[i]->buffer = calloc(1, LEN);
        ins[i]->buffer_length = LEN;
        ioctl(fd, USBDEVFS_SUBMITURB, ins[i]);
    }
    ctl(0x14, 0x0000, 0xC000);
    /* 48V ON for the selected input */
    ctl(0x17, p48val, 0x003F); ctl(0x21, 0x0000, 0x0000);
    /* gain raw value, gain register = mic index (0x0000 for AN1, 0x0001 for AN2) */
    ctl(0x1A, (gval & 0x1F) | 0x20, src - 1);
    /* routing src -> AN1/2 at 0x4000: L = 0x34 + src-1, R = 0x4E + src-1 */
    ctl(0x12, 0x4000, (0x0034 + src - 1) | 0xC000);
    ctl(0x12, 0x4000, (0x004E + src - 1) | 0xC000);
    /* master AN1/2: unmute 8-bit + 16-bit 0x4000 */
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, 0x4000, 0x03E0 | 0xC000);
    ctl(0x12, 0x4000, 0x03E1 | 0xC000);
    printf(">>> STREAMING %ds — 48V %s, gain raw %d, AN%d -> AN1/2. SPEAK + LISTEN <<<\n",
           gsec, src == 1 ? "AN1" : "AN2", gval, src);

    long long t0 = us();
    while (us() - t0 < gsec * 1000000LL) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) ioctl(fd, USBDEVFS_SUBMITURB, done);
    }
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
