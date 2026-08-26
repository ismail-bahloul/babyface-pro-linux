// CUE test: host 440 Hz tone on PB1 -> AN1/2 (0x0040/0x005B), then replay
// the CUE batches from cap_cue.pcap (0x12 writes, NO 0xC000 flag):
//   CUE ON  = 0x0000 to all PB pairs except the output's source PB
//   CUE OFF = 0x2000 (restore)
// Expect: tone keeps playing when CUE on AN1/2 (source PB1 excluded),
// tone STOPS when CUE on PH3/4 (PB1 muted), tone returns on restore.
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
/* PB1-6 L/R crosspoints into AN1/2 (the CUE map) */
static const int pbl[6] = {0x0040, 0x0042, 0x0044, 0x0046, 0x0048, 0x004A};
static const int pbr[6] = {0x005B, 0x005D, 0x005F, 0x0061, 0x0063, 0x0065};
/* CUE ON an output strip: mute all PB pairs except its source (out_idx+1) */
static void cue(int out_idx, int on) {
    for (int p = 0; p < 6; p++) {
        if (p == out_idx) continue;              /* source PB kept */
        ctl(0x12, on ? 0x0000 : 0x2000, pbl[p]); /* bare wIdx, no 0xC000 */
        ctl(0x12, on ? 0x0000 : 0x2000, pbr[p]);
    }
    printf("  [%5.1fs] CUE %s on output %d (source PB%d kept)\n",
           (us() - t0) / 1e6, on ? "ON " : "OFF", out_idx + 1, out_idx + 1);
}
static void restore_all(void) {
    for (int p = 0; p < 6; p++) {
        ctl(0x12, 0x2000, pbl[p]);
        ctl(0x12, 0x2000, pbr[p]);
    }
    printf("  [%5.1fs] CUE restore all (0x2000)\n", (us() - t0) / 1e6);
}
static void pump(int ms) {
    long long end = us() + ms * 1000;
    while (us() < end) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) ioctl(fd, USBDEVFS_SUBMITURB, done);
    }
}
static void fill_tone(unsigned char *buf) {
    double phase = 0.0;
    const double step = 2.0 * M_PI * 440.0 / 48000.0;
    const int amp = 0x100000;   /* 2^20 = -18 dBFS */
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
    int vol = argc > 1 ? (int)strtol(argv[1], NULL, 0) : 0x2000;
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
    /* route PB1 -> AN1/2 + master at `vol` (0xC000 transaction flag) */
    ctl(0x12, vol, 0x0040 | 0xC000);
    ctl(0x12, vol, 0x005B | 0xC000);
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, vol, 0x03E0 | 0xC000);
    ctl(0x12, vol, 0x03E1 | 0xC000);

    printf(">>> CUE TEST — steady 440 Hz. Listen: keeps -> STOPS -> back <<<\n");
    printf(">>> A: tone  B: CUE AN1/2 (source PB1 kept -> tone continues)\n");
    printf(">>> C: CUE PH3/4 (PB1 muted -> tone STOPS)  D: restore (tone back) <<<\n");
    t0 = us();
    pump(3000);                       /* A: baseline tone */
    cue(0, 1);                        /* B: CUE AN1/2, source PB1 excluded */
    pump(4000);
    cue(1, 1);                        /* C: CUE PH3/4, PB1 muted -> tone stops */
    pump(4000);
    restore_all();                    /* D */
    pump(3000);
    printf(">>> DONE <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
