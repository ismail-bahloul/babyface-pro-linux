// DEFINITIVE audio-pitch test: host generates a steady 440 Hz tone on
// OUT ch0/1 (PB1), routed PB1 -> AN1/2 (0x0040/0x005B) + master AN1/2.
// Mid-session pitch quads shift the device clock; if the audio clock
// follows, the tone detunes by the same % (440 Hz -> ~457 Hz at +4%).
// A steady tone makes even a 1% detune obvious. The IN rate is also
// measured per phase as an objective cross-check.
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
#define LEN 14336          /* 256 frames x 56 B */
#define FRAME 56
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
/* replay a captured pitch quad verbatim + the clock keepalive that
   follows it in cap_fus2.pcap (rate x DDS = const: 0xBBCC = +4%,
   0xCB72 = -4% — measured 2026-08-23, labels in old docs were flipped) */
static void pitch(int which) {
    if (which == 0) {          /* 0% */
        ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);
        ctl(0x1B, 0x8234, 0xD302); ctl(0x1B, 0x7CFF, 0xF803);
    } else if (which == 1) {   /* measured +4.00% (tone UP) */
        ctl(0x1B, 0xBBCC, 0xA100); ctl(0x1B, 0x8845, 0x6801);
        ctl(0x1B, 0x7D33, 0x1602); ctl(0x1B, 0x7CFF, 0xF803);
    } else {                   /* measured -4.06% (tone DOWN) */
        ctl(0x1B, 0xCB72, 0xD300); ctl(0x1B, 0x93A0, 0x3401);
        ctl(0x1B, 0x87A2, 0x2302); ctl(0x1B, 0x7CFF, 0xF803);
    }
    ctl(0x10, 0x0001, 0x05CF);
    printf("  [%5.1fs] pitch quad -> %s\n", (us() - t0) / 1e6,
           which == 0 ? "0%" : (which == 1 ? "+4% (tone UP)" : "-4% (tone DOWN)"));
}
/* pump `ms`, counting IN bytes for the rate cross-check */
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
    printf("  [%5.1fs] %-16s %8.0f B/s\n", (us() - t0) / 1e6, label, bps);
}
/* fill a 14336-B OUT URB with a 440 Hz sine on ch0+ch1 (bytes 1-3 LE,
   24-bit; byte 0 = frame marker 0x00) */
static void fill_tone(unsigned char *buf) {
    double phase = 0.0;
    const double step = 2.0 * M_PI * 440.0 / 48000.0;
    const int amp = 0x100000;   /* 2^20 = -18 dBFS: the old 2^23 was 0 dBFS = red meters */
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
int main(int argc, char **argv) {
    int vol = argc > 1 ? (int)strtol(argv[1], NULL, 0) : 0x2000; /* master+crosspoint level */
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
    /* route PB1 (idx 12/13) -> AN1/2 + master AN1/2 at `vol` */
    ctl(0x12, vol, 0x0040 | 0xC000);   /* PB1 L  -> AN1/2 L  */
    ctl(0x12, vol, 0x005B | 0xC000);   /* PB1 R  -> AN1/2 R  */
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, vol, 0x03E0 | 0xC000);
    ctl(0x12, vol, 0x03E1 | 0xC000);

    printf(">>> TONE PITCH TEST — steady 440 Hz on the headphones. Listen for the DETUNE <<<\n");
    printf(">>> expect: 440 Hz -> UP ~457 Hz -> 440 -> DOWN ~422 Hz -> 440 <<<\n");
    t0 = us();
    measure(3000, "0% (440 Hz)");
    pitch(1);                       /* +4%: tone UP */
    measure(4000, "+4%");
    pitch(0);
    measure(3000, "0% back");
    pitch(2);                       /* -4%: tone DOWN */
    measure(4000, "-4%");
    pitch(0);
    measure(3000, "0% back");
    printf(">>> DONE — did the tone detune UP then DOWN? <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
