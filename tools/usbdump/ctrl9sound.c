// ctrl9sound.c — AUDIBLE validation of the §9 controls: a steady 440 Hz
// host tone on PB1 -> AN1/2 plays throughout Part 1, so the phase /
// stereo-split / width / FX-send changes are HEARD on the headphones.
// Part 2 drops the tone and uses the AN2 mic (talk into it): MS proc
// cuts the mic, loopback feeds it back.
//
// Build/run: gcc -O2 -o /tmp/ctrl9sound tools/usbdump/ctrl9sound.c -lm
//            /tmp/ctrl9sound
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
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c, 0, sizeof(c));
    c.bRequestType = 0x40; c.bRequest = req; c.wValue = val; c.wIndex = idx; c.timeout = 1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl req=%02x errno=%d\n", req, errno);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
/* pump `ms` (keep the URBs moving) */
static void wait_ms(int ms) {
    long long end = us() + ms * 1000;
    while (us() < end) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) ioctl(fd, USBDEVFS_SUBMITURB, done);
    }
}
/* fill a 14336-B OUT URB with a 440 Hz sine on ch0+ch1 (24-bit LE,
   byte 0 = frame marker 0x00; amp 2^20 = -18 dBFS = comfortable) */
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
    /* PB1 (idx 12/13) -> AN1/2 + master at 0x16A0 (~0 dB fader) */
    ctl(0x12, 0x16A0, 0x0040 | 0xC000);
    ctl(0x12, 0x16A0, 0x005B | 0xC000);
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, 0x2000, 0x03E0 | 0xC000);
    ctl(0x12, 0x2000, 0x03E1 | 0xC000);

    printf(">>> ctrl9 SOUND test — steady 440 Hz on the headphones <<<\n");
    wait_ms(3000);
    printf(">>> [3s] baseline stereo tone (L+R in phase) <<<\n");

    /* 1. PHASE: invert the L crosspoint only (!0x16A0 = 0xE95F). */
    ctl(0x12, 0xE95F, 0x0040 | 0xC000);
    printf(">>> [5s] phase: L INVERTED (image goes hollow / outside the head) <<<\n");
    wait_ms(5000);
    ctl(0x12, 0x16A0, 0x0040 | 0xC000);
    printf(">>> [3s] phase normal <<<\n");
    wait_ms(3000);

    /* 2. STEREO SPLIT: L at 0 dB (0x2000), R muted. */
    ctl(0x12, 0x2000, 0x000C); ctl(0x12, 0x0000, 0x0027);
    ctl(0x12, 0x2000, 0x0040 | 0xC000); ctl(0x12, 0x0000, 0x005B | 0xC000);
    printf(">>> [5s] SPLIT: tone only in the L ear, ~6 dB louder <<<\n");
    wait_ms(5000);
    ctl(0x12, 0x1000, 0x000C); ctl(0x12, 0x1000, 0x0027);
    ctl(0x12, 0x1000, 0x0040 | 0xC000); ctl(0x12, 0x1000, 0x005B | 0xC000);
    printf(">>> [3s] split OFF: both ears at -6 dB <<<\n");
    wait_ms(3000);

    /* 3. WIDTH +0.75: 4 balance pairs -> 0x1C00/0x0400 (image shifts L). */
    ctl(0x12, 0x1C00, 0x00AE); ctl(0x12, 0x0400, 0x00AF);
    ctl(0x12, 0x1C00, 0x00C8); ctl(0x12, 0x0400, 0x00C9);
    ctl(0x12, 0x1C00, 0x0046); ctl(0x12, 0x0400, 0x0047);
    ctl(0x12, 0x1C00, 0x0060); ctl(0x12, 0x0400, 0x0061);
    printf(">>> [5s] width +0.75 (image tilts? if nothing, regs aren't PB1) <<<\n");
    wait_ms(5000);
    unsigned pairs[4][2] = {{0x00AE, 0x00AF}, {0x00C8, 0x00C9}, {0x0046, 0x0047}, {0x0060, 0x0061}};
    for (int i = 0; i < 4; i++) {
        ctl(0x12, 0x1000, pairs[i][0]); ctl(0x12, 0x1000, pairs[i][1]);
    }
    printf(">>> [3s] width neutral <<<\n");
    wait_ms(3000);

    /* 4. FX SEND to max (reverb is HOST-side, probably silent). */
    ctl(0x12, 0x1000, 0x0138); ctl(0x12, 0x1000, 0x0153);
    printf(">>> [4s] FX send max (reverb is host-side — likely no change) <<<\n");
    wait_ms(4000);
    ctl(0x12, 0x000C, 0x0138); ctl(0x12, 0x000C, 0x0153);
    printf(">>> [2s] FX send off <<<\n");
    wait_ms(2000);

    /* ── Part 2: mic. Drop the tone, route AN2 -> AN1/2. ── */
    memset(tone, 0, LEN); /* silence */
    ctl(0x12, 0x16A0, 0x0035 | 0xC000);   /* AN2 L -> AN1/2 L */
    ctl(0x12, 0x16A0, 0x004F | 0xC000);   /* AN2 R -> AN1/2 R */
    printf(">>> [4s] mic AN2 -> AN1/2 (talk into the mic) <<<\n");
    wait_ms(4000);

    /* 5. MS PROC ON: AN2 crosspoints -> 0x0000 (mic cuts). */
    ctl(0x12, 0x0000, 0x0001); ctl(0x12, 0x0000, 0x0035);
    printf(">>> [5s] MS proc ON: mic should CUT (talk -> nothing) <<<\n");
    wait_ms(5000);
    ctl(0x12, 0x16A0, 0x0001); ctl(0x12, 0x16A0, 0x0035);
    printf(">>> [3s] MS proc OFF: mic back <<<\n");
    wait_ms(3000);

    /* 6. LOOPBACK AN1/2 ON (ch 0/1) — you should hear the mic. */
    ctl(0x15, 0x0001, 0); ctl(0x15, 0x0001, 1);
    printf(">>> [5s] LOOPBACK ON: mic heard in the headphones? <<<\n");
    wait_ms(5000);
    ctl(0x15, 0x0000, 0); ctl(0x15, 0x0000, 1);
    printf(">>> [2s] loopback OFF <<<\n");
    wait_ms(2000);

    printf(">>> DONE — report what you heard per step <<<\n");
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
