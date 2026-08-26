// set_pitch() test: compare the DERIVED quad (bank1/2 formulas, frac=0)
// against the CAPTURED verbatim quad — if the IN rate shifts identically,
// the formulas work and the fraction bytes don't matter.
// +4% : DDS_24 = round(12800000/1.04) = 0xBBCCAC
//       bank1 = round(48076*0.72562) = 0x8845, bank2 = round(48076*2/3) = 0x7D33
//       captured verbatim (cap_fus2): 0xBBCC/0xA1 0x8845/0x68 0x7D33/0x16 0x7CFF/0xF8
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/usbdevice_fs.h>
#define DEV "/dev/bus/usb/003/002"
#define LEN 14336
#define NQ 9

static int fd;
static long long t0;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0x40; c.bRequest=req; c.wValue=val; c.wIndex=idx; c.timeout=1000;
    ioctl(fd, USBDEVFS_CONTROL, &c);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
/* pitch quad: bank0 (val,frac), bank1, bank2, bank3 */
static void quad(int v0, int f0, int v1, int f1, int v2, int f2, int v3, int f3) {
    ctl(0x1B, v0, (f0 << 8) | 0);
    ctl(0x1B, v1, (f1 << 8) | 1);
    ctl(0x1B, v2, (f2 << 8) | 2);
    ctl(0x1B, v3, (f3 << 8) | 3);
    ctl(0x10, 0x0001, 0x05CF);   /* clock keepalive (required) */
}
/* pump `ms`, measure the IN byte rate */
static void measure(int ms, const char *label) {
    long long end = us() + ms * 1000;
    long long bytes = 0;
    while (us() < end) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
            if (done->endpoint == 0x82) bytes += done->actual_length;
            ioctl(fd, USBDEVFS_SUBMITURB, done);
        }
    }
    double bps = bytes / (ms / 1000.0);
    printf("  [%5.1fs] %-30s %8.0f B/s  (%lld B in %d ms)\n", (us() - t0) / 1e6, label, bps, bytes, ms);
}
int main(void) {
    fd = open(DEV, O_RDWR);
    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    struct usbdevfs_setinterface si = {5, 1}; ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    for (int i = 0; i <= 0x3D; i++) { if (i == 0x1E || i == 0x1F) continue; ctl(0x16, 0x0000, i); }
    ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);
    ctl(0x1B, 0x8234, 0xD302); ctl(0x1B, 0x7CFF, 0xF803);
    ctl(0x10, 0x0021, 0x05FF);
    ctl(0x17, 0x000C, 0x0000); ctl(0x21, 0x0000, 0x0000);
    ctl(0x10, 0x0000, 0x3000); ctl(0x10, 0x0000, 0x3000);
    ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800);
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
    printf(">>> set_pitch test — derived vs captured quad, measure the IN rate <<<\n");
    t0 = us();
    measure(3000, "0% (baseline)");
    /* derived +4%: frac=0 everywhere, bank3 = 0% value */
    quad(0xBBCC, 0xAC, 0x8845, 0x00, 0x7D33, 0x00, 0x7CFF, 0x00);
    printf("  [%5.1fs] derived +4%% quad sent (frac=0)\n", (us() - t0) / 1e6);
    measure(4000, "derived +4%");
    /* captured +4% verbatim */
    quad(0xBBCC, 0xA1, 0x8845, 0x68, 0x7D33, 0x16, 0x7CFF, 0xF8);
    printf("  [%5.1fs] captured +4%% quad sent (verbatim)\n", (us() - t0) / 1e6);
    measure(4000, "captured +4%");
    /* back to 0% */
    quad(0xC350, 0x00, 0x8DB8, 0xD2, 0x8234, 0xD3, 0x7CFF, 0xF8);
    measure(3000, "0% back");
    printf(">>> DONE — same rate = formulas work, fracs don't matter <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
