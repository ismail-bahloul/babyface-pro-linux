// Capture the device's 9-byte status packets on ep 0x82 (the always-on
// status stream Windows keeps running) — idle, then during a full
// session. Dump content + correlate with the 0x17 readback.
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
#define NQ 4

static void ctl(int fd, int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c;
    memset(&c, 0, sizeof(c));
    c.bRequestType = 0x40;
    c.bRequest = req;
    c.wValue = val;
    c.wIndex = idx;
    c.timeout = 1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("    ctl req=%02x errno=%d\n", req, errno);
}

static void readreg(int fd, int req, unsigned char *out) {
    unsigned char buf[4] = {0};
    struct usbdevfs_ctrltransfer c;
    memset(&c, 0, sizeof(c));
    c.bRequestType = 0xC0;
    c.bRequest = req;
    c.wLength = 4;
    c.timeout = 1000;
    c.data = buf;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4 && out) memcpy(out, buf, 4);
}

static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

/* Submit n 9-byte IN URBs on 0x82; reap in a loop for `ms`, printing each
   completion's content and the 0x17 readback every 500ms. */
static void poll9(int fd, struct usbdevfs_urb *urbs[16], struct usbdevfs_urb *done[16],
                  int n, long ms, const char *tag) {
    for (int i = 0; i < n && i < 16; i++) {
        urbs[i] = calloc(1, sizeof(*urbs[i]));
        urbs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        urbs[i]->endpoint = 0x82;
        urbs[i]->buffer = calloc(1, 9);
        urbs[i]->buffer_length = 9;
        done[i] = calloc(1, sizeof(*done[i]) + 9);
        ioctl(fd, USBDEVFS_SUBMITURB, urbs[i]);
    }
    printf("--- %s: %d x 9B IN URBs ---\n", tag, n);
    long long t0 = us();
    long long next_v17 = t0;
    int printed = 0;
    while (us() - t0 < ms * 1000) {
        for (int i = 0; i < n && i < 16; i++) {
            errno = 0;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, done[i]) == 0) {
                unsigned char *b = (unsigned char *)done[i]->buffer;
                int al = done[i]->actual_length;
                if (printed++ < 8 || al != 9)
                    printf("  9B actual=%d data=%02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                           al, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8]);
                ioctl(fd, USBDEVFS_SUBMITURB, urbs[i]);
            }
        }
        if (us() >= next_v17) {
            unsigned char v[4];
            readreg(fd, 0x17, v);
            printf("  0x17=%02X %02X %02X %02X\n", v[0], v[1], v[2], v[3]);
            next_v17 += 500000;
        }
    }
}

int main(void) {
    int fd = open(DEV, O_RDWR);
    int iface = 5;
    ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    struct usbdevfs_setinterface si = {5, 1};
    ioctl(fd, USBDEVFS_SETINTERFACE, &si);

    /* Phase 1: idle — 9B status only, no audio URBs, no trigger. */
    struct usbdevfs_urb *urbs9[16], *done9[16];
    poll9(fd, urbs9, done9, 3, 2000, "IDLE");

    /* Phase 2: full session — trigger + 48V arm + audio URBs; keep 9B polls. */
    ctl(fd, 0x10, 0x0000, 0x8000);
    ctl(fd, 0x1D, 0x0000, 0x0000);
    struct usbdevfs_urb *outs[NQ], *ins[NQ], *do_[NQ], *di[NQ];
    for (int i = 0; i < NQ; i++) {
        outs[i] = calloc(1, sizeof(*outs[i]));
        outs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        outs[i]->endpoint = 0x01;
        outs[i]->buffer = calloc(1, LEN);
        outs[i]->buffer_length = LEN;
        for (int f = 0; f < LEN / 56; f++) {
            unsigned int *fr = (unsigned int *)((char *)outs[i]->buffer + f * 56);
            fr[4] = 0x20000000; fr[5] = 0x20000000;
        }
        ins[i] = calloc(1, sizeof(*ins[i]));
        ins[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        ins[i]->endpoint = 0x82;
        ins[i]->buffer = calloc(1, LEN);
        ins[i]->buffer_length = LEN;
        do_[i] = calloc(1, sizeof(*do_[i]) + LEN);
        di[i] = calloc(1, sizeof(*di[i]) + LEN);
        ioctl(fd, USBDEVFS_SUBMITURB, outs[i]);
        ioctl(fd, USBDEVFS_SUBMITURB, ins[i]);
    }
    ctl(fd, 0x14, 0x0000, 0xC000);
    ctl(fd, 0x17, 0x000D, 0x003F);   /* 48V ON */
    ctl(fd, 0x21, 0x0000, 0x0000);
    printf("--- STREAMING (audio URBs + 9B polls) ---\n");
    long long t0 = us();
    long long next_v17 = t0;
    int printed = 0;
    while (us() - t0 < 4000000) {
        for (int i = 0; i < NQ; i++) {
            errno = 0;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, di[i]) == 0) ioctl(fd, USBDEVFS_SUBMITURB, ins[i]);
            errno = 0;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, do_[i]) == 0) ioctl(fd, USBDEVFS_SUBMITURB, outs[i]);
        }
        for (int i = 0; i < 3; i++) {
            errno = 0;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, done9[i]) == 0) {
                unsigned char *b = (unsigned char *)done9[i]->buffer;
                if (printed++ < 10)
                    printf("  9B actual=%d data=%02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                           done9[i]->actual_length, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8]);
                ioctl(fd, USBDEVFS_SUBMITURB, urbs9[i]);
            }
        }
        if (us() >= next_v17) {
            unsigned char v[4];
            readreg(fd, 0x17, v);
            printf("  0x17=%02X %02X %02X %02X\n", v[0], v[1], v[2], v[3]);
            next_v17 += 500000;
        }
    }
    close(fd);
    return 0;
}
