// Validate the rate-change decode: mid-session SET_INTERFACE(5, alt)
// ONLY, with per-alt URB sizes (frame-size * 256: alt1 56B->14336,
// alt2 40B->10240, alt3 32B->8192). Measure the IN frame rate.
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
#define NQ 9

static int fd;
static struct usbdevfs_urb *outs[NQ], *ins[NQ];
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0x40; c.bRequest=req; c.wValue=val; c.wIndex=idx; c.timeout=1000;
    ioctl(fd, USBDEVFS_CONTROL, &c);
}
static void rd11(const char *s) {
    unsigned char b[4]={0}; struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0xC0; c.bRequest=0x11; c.wLength=4; c.timeout=1000; c.data=b;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)
        printf("  %-28s 0x11=%02X %02X %02X %02X (byte3&0x0F=0x%02X)\n", s, b[0], b[1], b[2], b[3], b[3] & 0x0F);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
static void setif(int alt) {
    struct usbdevfs_setinterface si = {5, alt};
    int r = ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    printf("  SET_INTERFACE(5,%d) -> %s\n", alt, r == 0 ? "ok" : "fail");
}
/* pump `ms`, resizing every reaped IN URB to `sz` before resubmit */
static double measure(int ms, int sz) {
    long long t0 = us();
    long long bytes = 0, nurb = 0;
    while (us() - t0 < ms * 1000) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
            if (done->endpoint == 0x82) {
                bytes += done->actual_length;
                nurb++;
            }
            done->buffer_length = sz;   /* resize for the alt */
            ioctl(fd, USBDEVFS_SUBMITURB, done);
        }
    }
    double dt = (us() - t0) / 1e6;
    return bytes / dt;
}
int main(void) {
    fd = open(DEV, O_RDWR);
    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    setif(1);
    for (int i = 0; i <= 0x3D; i++) { if (i == 0x1E || i == 0x1F) continue; ctl(0x16, 0x0000, i); }
    ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);
    ctl(0x1B, 0x8234, 0xD302); ctl(0x1B, 0x7CFF, 0xF803);
    ctl(0x10, 0x0021, 0x05FF);
    ctl(0x17, 0x000C, 0x0000); ctl(0x21, 0x0000, 0x0000);
    ctl(0x10, 0x0000, 0x3000); ctl(0x10, 0x0000, 0x3000);
    ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800);
    ctl(0x10, 0x0000, 0x8000);
    ctl(0x1D, 0x0000, 0x0000);
    for (int i = 0; i < NQ; i++) {
        outs[i] = calloc(1, sizeof(*outs[i]));
        outs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        outs[i]->endpoint = 0x01;
        outs[i]->buffer = calloc(1, 20480);
        outs[i]->buffer_length = 14336;
        ioctl(fd, USBDEVFS_SUBMITURB, outs[i]);
    }
    for (int i = 0; i < NQ; i++) {
        ins[i] = calloc(1, sizeof(*ins[i]));
        ins[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        ins[i]->endpoint = 0x82;
        ins[i]->buffer = calloc(1, 20480);
        ins[i]->buffer_length = 14336;
        ioctl(fd, USBDEVFS_SUBMITURB, ins[i]);
    }
    ctl(0x14, 0x0000, 0xC000);
    rd11("alt1 readback");
    printf("  alt1: %.0f B/s (expect 48k = 2.688 MB/s)\n", measure(3000, 14336));
    setif(2);
    rd11("after alt2 readback");
    printf("  alt2: %.0f B/s (expect 96k = 3.84 MB/s @40B, or 5.38 @56B)\n", measure(3000, 10240));
    setif(3);
    rd11("after alt3 readback");
    printf("  alt3: %.0f B/s (expect 192k = 6.14 MB/s @32B)\n", measure(3000, 8192));
    setif(1);
    rd11("back to alt1 readback");
    printf("  alt1: %.0f B/s\n", measure(3000, 14336));
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
