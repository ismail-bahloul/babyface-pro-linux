// Fader-anchor calibration test: host tone at -18 dBFS (0x100000) on
// PB1 -> AN1/2, master fixed at 0x2000 (0 dB, master curve). The
// crosspoint is stepped through calibrated positions; read the
// physical OUTPUT VU meter (dB) at each.
//   If "0 dB" (0x16A0) = unity:      VU shows -18 at 0x16A0, -12 at 0x2D41.
//   If "+6 dB" (0x2D41) = unity:     VU shows -24 at 0x16A0, -18 at 0x2D41.
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
#define FRAME 56
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
static void pump(int ms) {
    long long end = us() + ms * 1000;
    while (us() < end) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) ioctl(fd, USBDEVFS_SUBMITURB, done);
    }
}
static void fill_tone(unsigned char *buf, int amp) {
    double phase = 0.0;
    const double step = 2.0 * M_PI * 440.0 / 48000.0;
    for (int f = 0; f < LEN / FRAME; f++) {
        int s = (int)(sin(phase) * amp);
        for (int ch = 0; ch < 2; ch++) {
            int base = f * FRAME + ch * 4;
            buf[base] = 0x00;
            buf[base + 1] = s & 0xFF;
            buf[base + 2] = (s >> 8) & 0xFF;
            buf[base + 3] = (s >> 16) & 0xFF;
        }
        phase += step;
        if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
    }
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
    unsigned char *tone = calloc(1, LEN);
    fill_tone(tone, 0x100000);   /* -18 dBFS */
    struct usbdevfs_urb *outs[NQ], *ins[NQ];
    for (int i = 0; i < NQ; i++) {
        outs[i] = calloc(1, sizeof(*outs[i]));
        outs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        outs[i]->endpoint = 0x01;
        outs[i]->buffer = tone;
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
    /* master fixed at 0 dB (master curve 0x2000) */
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, 0x2000, 0x03E0 | 0xC000);
    ctl(0x12, 0x2000, 0x03E1 | 0xC000);

    static const struct { int raw; const char *label; } steps[] = {
        { 0x0B51, "-6 dB  (0x0B51)  -> expect VU -24 if anchor OK, -18 if +6=unity" },
        { 0x16A0, " 0 dB  (0x16A0)  -> expect VU -18 if anchor OK, -24 if +6=unity" },
        { 0x1C7C, "+2 dB  (0x1C7C)  -> expect VU -16" },
        { 0x2D41, "+6 dB  (0x2D41)  -> expect VU -12 if anchor OK, -18 if +6=unity" },
    };
    printf(">>> READ THE PHYSICAL OUTPUT VU METER (dB) AT EACH STEP, 4 s each <<<\n");
    t0 = us();
    for (size_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
        ctl(0x12, steps[i].raw, 0x0040 | 0xC000);   /* PB1 L -> AN1/2 */
        ctl(0x12, steps[i].raw, 0x005B | 0xC000);   /* PB1 R -> AN1/2 */
        printf("  [%5.1fs] crosspoint %s\n", (us() - t0) / 1e6, steps[i].label);
        pump(4000);
    }
    printf(">>> DONE <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
