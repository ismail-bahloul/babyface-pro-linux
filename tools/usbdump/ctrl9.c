// ctrl9.c — hardware validation of the §9 controls (implemented in
// tuxmix-usb/src/protocol.rs, decoded on Windows 2026-08-23). Each step
// sends the EXACT requests the Rust functions produce, with a pause so
// you can observe the card (VU meters / LEDs / audio).
//
// Steps: 1 loopback 2 AN 1>2 3 MS proc 4 phase 5 FX send 6 stereo split
//        7 width 8 ref level. Every state is restored at the end.
//
// Build/run: gcc -O2 -o /tmp/ctrl9 tools/usbdump/ctrl9.c && /tmp/ctrl9
#define _GNU_SOURCE
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
    struct usbdevfs_ctrltransfer c; memset(&c, 0, sizeof(c));
    c.bRequestType = 0x40; c.bRequest = req; c.wValue = val; c.wIndex = idx; c.timeout = 1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) {
        fprintf(stderr, "ctl(%02x,%04x,%04x) failed: %s\n", req, val, idx, strerror(errno));
        exit(1);
    }
}
static void step(const char *label) {
    printf(">>> %s\n", label);
    fflush(stdout);
    usleep(2500000); /* 2.5 s to observe the card */
}
static void restore_stream(void) {
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
}
int main(void) {
    fd = open(DEV, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    struct usbdevfs_setinterface si = {5, 1}; ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    /* identical init to pitchsweep.c (validated) */
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
    printf(">>> ctrl9 — §9 controls validation (observe the card) <<<\n");

    /* 1. Loopback ON then OFF on the AN1/2 output (ch 0/1). */
    ctl(0x15, 0x0001, 0); ctl(0x15, 0x0001, 1);
    step("1/8 loopback AN1/2 ON  (mic -> loopback path? check VU/routing)");
    ctl(0x15, 0x0000, 0); ctl(0x15, 0x0000, 1);
    step("1/8 loopback OFF");

    /* 2. AN 1>2 toggle. */
    ctl(0x17, 0x1400, 0x1000);
    step("2/8 AN 1>2 ON   (AN1 routed to AN2? check VU)");
    ctl(0x17, 0x0400, 0x1000);
    step("2/8 AN 1>2 OFF");

    /* 3. MS proc: AN2 crosspoints muted, then restored (0x068E). */
    ctl(0x12, 0x0000, 0x0001); ctl(0x12, 0x0000, 0x0035);
    step("3/8 MS proc ON   (AN2 VU should drop to ~0)");
    ctl(0x12, 0x068E, 0x0001); ctl(0x12, 0x068E, 0x0035);
    step("3/8 MS proc OFF  (AN2 VU restored)");

    /* 4. Phase: set AN1->out0 to 0 dB (0x16A0), negate it (!0x16A0 =
     * 0xE95F), restore. Low map + standard map, like the capture. */
    ctl(0x12, 0x16A0, 0x0000); ctl(0x12, 0x16A0, 0x0034);
    step("4/8 AN1 -> out0 at 0 dB");
    ctl(0x12, 0xE95F, 0x0000); ctl(0x12, 0xE95F, 0x0034);
    step("4/8 phase INVERT (polarity flip — audibly hollow/thin in stereo)");
    ctl(0x12, 0x16A0, 0x0000); ctl(0x12, 0x16A0, 0x0034);
    step("4/8 phase normal");

    /* 5. FX send to max, then back to the observed ramp start. */
    ctl(0x12, 0x1000, 0x0138); ctl(0x12, 0x1000, 0x0153);
    step("5/8 FX send 0 dB (max observed — reverb is HOST-side, may be silent)");
    ctl(0x12, 0x000C, 0x0138); ctl(0x12, 0x000C, 0x0153);
    step("5/8 FX send off (0x000C)");

    /* 6. Stereo split PB1 into AN1/2 (0x2000/0x0000), then back to the
     * -6 dB stereo pair. */
    ctl(0x12, 0x2000, 0x000C); ctl(0x12, 0x0000, 0x0027);
    ctl(0x12, 0x2000, 0x0040); ctl(0x12, 0x0000, 0x005B);
    step("6/8 stereo split PB1 (L at 0 dB, R muted — check playback VU)");
    ctl(0x12, 0x1000, 0x000C); ctl(0x12, 0x1000, 0x0027);
    ctl(0x12, 0x1000, 0x0040); ctl(0x12, 0x1000, 0x005B);
    step("6/8 stereo split OFF (both -6 dB)");

    /* 7. Width +0.75 (0x1C00/0x0400 on the 4 pairs), then neutral. */
    ctl(0x12, 0x1C00, 0x00AE); ctl(0x12, 0x0400, 0x00AF);
    ctl(0x12, 0x1C00, 0x00C8); ctl(0x12, 0x0400, 0x00C9);
    ctl(0x12, 0x1C00, 0x0046); ctl(0x12, 0x0400, 0x0047);
    ctl(0x12, 0x1C00, 0x0060); ctl(0x12, 0x0400, 0x0061);
    step("7/8 width +0.75 (which strip changes? TBD mapping)");
    unsigned pairs[4][2] = {{0x00AE, 0x00AF}, {0x00C8, 0x00C9}, {0x0046, 0x0047}, {0x0060, 0x0061}};
    for (int i = 0; i < 4; i++) {
        ctl(0x12, 0x1000, pairs[i][0]); ctl(0x12, 0x1000, pairs[i][1]);
    }
    step("7/8 width neutral");

    /* 8. Ref level cycle (Instr 3/4): 0x000C <-> 0x0000 + commit. */
    ctl(0x17, 0x000C, 0x003F); ctl(0x21, 0x0000, 0x0000);
    step("8/8 ref level state 0x000C (default)");
    ctl(0x17, 0x0000, 0x003F); ctl(0x21, 0x0000, 0x0000);
    step("8/8 ref level state 0x0000");
    ctl(0x17, 0x000C, 0x003F); ctl(0x21, 0x0000, 0x0000);
    step("8/8 ref level back to 0x000C");

    printf(">>> DONE — everything restored <<<\n");
    restore_stream();
    return 0;
}
