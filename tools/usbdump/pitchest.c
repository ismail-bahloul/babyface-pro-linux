// Pitch/varispeed test v2: replay the captured 0x1B DDS quads mid-session
// (LINUX-VALIDATION.md §5) + the clock keepalive (cap_fus2.pcap) and
// MEASURE the IN-stream byte rate per phase. If the pitch applies, the
// device clock drifts and the rate shifts by the same % (48k @56B/frame
// = 2.688 MB/s baseline; +4.16% -> ~2.80 MB/s, -3.85% -> ~2.58 MB/s).
// No listening needed — the numbers are the verdict.
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
static long long t0;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0x40; c.bRequest=req; c.wValue=val; c.wIndex=idx; c.timeout=1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl req=%02x errno=%d\n", req, errno);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
/* replay a captured pitch quad verbatim (bank 3 terminator is constant),
   then the clock-source keepalive that always follows in the capture */
static void pitch(int pct100) {          /* pct100 = pitch in 1/100 % */
    if (pct100 == 0) {
        ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);
        ctl(0x1B, 0x8234, 0xD302); ctl(0x1B, 0x7CFF, 0xF803);
    } else if (pct100 == 416) {
        ctl(0x1B, 0xCB72, 0xD300); ctl(0x1B, 0x93A0, 0x3401);
        ctl(0x1B, 0x87A2, 0x2302); ctl(0x1B, 0x7CFF, 0xF803);
    } else if (pct100 == -385) {
        ctl(0x1B, 0xBBCC, 0xA100); ctl(0x1B, 0x8845, 0x6801);
        ctl(0x1B, 0x7D33, 0x1602); ctl(0x1B, 0x7CFF, 0xF803);
    }
    ctl(0x10, 0x0001, 0x05CF);
    printf("  [%5.1fs] pitch quad -> %+d.%02d%%\n", (us() - t0) / 1e6, pct100 / 100, abs(pct100) % 100);
}
/* pump `ms`, counting IN bytes, and report the rate */
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
    double dt = (us() - t0) / 1e6;
    double bps = bytes / ((end - (end - ms * 1000)) / 1e6); /* ms window */
    printf("  [%5.1fs] %-14s %8.0f B/s  (%lld bytes in %d ms)\n", dt, label, bps, bytes, ms);
}
int main(int argc, char **argv) {
    int src = argc > 1 ? atoi(argv[1]) : 1;     /* input channel 1 or 2 */
    int gval = argc > 2 ? atoi(argv[2]) : 10;   /* raw gain value */
    int vol = argc > 3 ? (int)strtol(argv[3], NULL, 0) : 0x2000; /* crosspoint+master level */
    int p48val = src == 1 ? 0x0D : 0x0E;        /* bit0 = AN1, bit1 = AN2 */
    fd = open(DEV, O_RDWR);
    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    struct usbdevfs_setinterface si = {5, 1}; ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    /* init burst */
    for (int i = 0; i <= 0x3D; i++) { if (i == 0x1E || i == 0x1F) continue; ctl(0x16, 0x0000, i); }
    ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);   /* 0% quad */
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
    /* routing src -> AN1/2 at `vol`: L = 0x34 + src-1, R = 0x4E + src-1 */
    ctl(0x12, vol, (0x0034 + src - 1) | 0xC000);
    ctl(0x12, vol, (0x004E + src - 1) | 0xC000);
    /* master AN1/2: unmute 8-bit + 16-bit `vol` */
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, vol, 0x03E0 | 0xC000);
    ctl(0x12, vol, 0x03E1 | 0xC000);

    printf(">>> PITCH RATE TEST — 48V AN%d, gain raw %d, level 0x%04X. Measuring IN rate per phase <<<\n", src, gval, vol);
    printf(">>> baseline 48k @56B/frame = 2.688 MB/s. Pitch shifts it by the same %% <<<\n");
    t0 = us();
    measure(3000, "0% baseline");
    pitch(416);   /* +4.16% */
    measure(4000, "+4.16%");
    pitch(-385);  /* -3.85% */
    measure(4000, "-3.85%");
    pitch(0);     /* back to 0% */
    measure(3000, "0% back");
    printf(">>> DONE <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
