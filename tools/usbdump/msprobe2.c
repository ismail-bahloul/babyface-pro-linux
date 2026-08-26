// msprobe2.c — decisive MS-proc test with a STEADY tone (no hand-rubbing).
// Chain: PB1 tone -> AN1/2 out (0x0B57, -6 dB) -> loopback -> AN1/2 IN
// (steady, measurable) -> AN 1>2 -> AN2 -> AN2->out crosspoints (0x1000,
// -6 dB) -> echo back into the output. MS proc mutes the AN2 crosspoints;
// a working mute removes the echo and the loopback level DROPS. Compares
// the BARE write (as the capture shows) against the FLAGGED write (0xC000
// transaction bits, like every fader write).
#define _GNU_SOURCE
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
#define FULL 8388608.0

static int fd;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c, 0, sizeof(c));
    c.bRequestType = 0x40; c.bRequest = req; c.wValue = val; c.wIndex = idx; c.timeout = 1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl(%02x) errno=%d\n", req, errno);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
static void measure(int ms, const char *label) {
    long long end = us() + ms * 1000;
    double peak = 0.0;
    while (us() < end) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
            if (done->endpoint == 0x82) {
                unsigned char *b = done->buffer;
                int len = done->actual_length;
                for (int f = 0; f < len / FRAME; f++) {
                    unsigned word = (unsigned)b[f * FRAME + 1]
                                  | ((unsigned)b[f * FRAME + 2] << 8)
                                  | ((unsigned)b[f * FRAME + 3] << 16);
                    double mag = fabs((int)(word << 8) >> 8) / FULL;
                    if (mag > peak) peak = mag;
                }
            }
            ioctl(fd, USBDEVFS_SUBMITURB, done);
        }
    }
    printf("  %-36s IN ch0: %6.1f dBFS\n", label, 20 * log10(peak + 1e-9));
}
static void fill_tone(unsigned char *buf) {
    double phase = 0.0;
    const double step = 2.0 * M_PI * 440.0 / 48000.0;
    const int amp = 0x100000;
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
/* write the AN2->out crosspoints (low + standard, L+R) */
static void an2_cross(int val, int flag) {
    ctl(0x12, val, 0x0001 | flag);   /* low L */
    ctl(0x12, val, 0x001B | flag);   /* low R */
    ctl(0x12, val, 0x0035 | flag);   /* std L (AN2 -> out0) */
    ctl(0x12, val, 0x004F | flag);   /* std R */
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
    fill_tone(tone);
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
    /* PB1 -> out at -6 dB + master; loopback ON; AN 1>2 ON (commit) */
    ctl(0x12, 0x0B57, 0x0040 | 0xC000);
    ctl(0x12, 0x0B57, 0x005B | 0xC000);
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, 0x2000, 0x03E0 | 0xC000);
    ctl(0x12, 0x2000, 0x03E1 | 0xC000);
    ctl(0x15, 0x0001, 0); ctl(0x15, 0x0001, 1);        /* loopback ON */
    ctl(0x17, 0x1400, 0x1000); ctl(0x21, 0x0000, 0x0000); /* AN 1>2 ON */

    printf(">>> MS proc decisive test — steady tone via loopback <<<\n");
    printf("  baseline (PB1 only, AN2 not routed)      "); measure(2500, "baseline (PB1 only)");
    an2_cross(0x1000, 0xC000);   /* route AN2 -> out (flagged, like TotalMix) */
    printf("  AN2 routed at -6 dB (echo expected)       "); measure(2500, "AN2 routed -6 dB");
    an2_cross(0x0000, 0);        /* MS proc ON, BARE (as captured) */
    printf("  MS ON bare (no flag)                      "); measure(2500, "MS ON bare");
    an2_cross(0x0000, 0xC000);   /* MS proc ON, FLAGGED */
    printf("  MS ON flagged (0xC000)                    "); measure(2500, "MS ON flagged");
    an2_cross(0x1000, 0xC000);   /* restore (flagged) */
    printf("  MS OFF restored                          "); measure(2500, "MS OFF restored");
    printf(">>> DONE — the drop reveals which write actually mutes <<<\n");
    ctl(0x15, 0x0000, 0); ctl(0x15, 0x0000, 1); ctl(0x15, 0x0000, 2); ctl(0x15, 0x0000, 3);
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
