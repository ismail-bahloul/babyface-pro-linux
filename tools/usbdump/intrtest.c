// Test the derived 44.1kHz rate code (0xB372) in the 0x1B setup, with
// the full stream + ARM. Watch byte2. Also test 0x1B written AFTER the
// stream starts.
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
#define FRAME_US 5333

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

static void burst_rest(int fd) {
    for (int i = 0; i <= 0x3D; i++) {
        if (i == 0x1E || i == 0x1F) continue;
        ctl(fd, 0x16, 0x0000, i);
    }
    ctl(fd, 0x1B, 0xC350, 0x0000);        /* rate codes right after the 0x16 clears,
                                             matching cap_coldplug order */
    ctl(fd, 0x1B, 0x8DB8, 0xD201);
    ctl(fd, 0x1B, 0x8234, 0xD302);
    ctl(fd, 0x1B, 0x7CFF, 0xF803);
    unsigned char v[4];
    readreg(fd, 0x1C, v);                 /* cap_coldplug #173: a 0x1C READ here */
    ctl(fd, 0x10, 0x0021, 0x05FF);
    ctl(fd, 0x17, 0x000C, 0x0000);        /* 0x000C at init (capture), not 0x000D */
    ctl(fd, 0x21, 0x0000, 0x0000);
    ctl(fd, 0x10, 0x0000, 0x3000);
    ctl(fd, 0x10, 0x0000, 0x3000);
    ctl(fd, 0x10, 0x0800, 0x0800);
    ctl(fd, 0x10, 0x0800, 0x0800);
    ctl(fd, 0x10, 0x0800, 0x0800);
}

static int run_session(int fd, int rateval, const char *tag) {
    struct usbdevfs_setinterface si = {5, 1};
    ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    burst_rest(fd);
    ctl(fd, 0x1B, rateval, 0x0000);        /* rate code slot 1 override */
    ctl(fd, 0x10, 0x0000, 0x8000);
    ctl(fd, 0x1D, 0x0000, 0x0000);

    struct usbdevfs_urb *outs[NQ], *ins[NQ], *do_[NQ], *di[NQ];
    for (int i = 0; i < NQ; i++) {
        outs[i] = calloc(1, sizeof(*outs[i]));
        outs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        outs[i]->endpoint = 0x01;
        outs[i]->buffer = calloc(1, LEN);
        outs[i]->buffer_length = LEN;
        /* Fill with valid 14-ch frames: marker ch4/ch5 = 0x20000000
           (matches the IN stream), rest zero. */
        for (int f = 0; f < LEN / 56; f++) {
            unsigned int *fr = (unsigned int *)((char *)outs[i]->buffer + f * 56);
            fr[4] = 0x20000000;
            fr[5] = 0x20000000;
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
    printf("--- %s (rate 0x%04X): stream + arm + 48V ---\n", tag, rateval);

    long long t0 = us();
    int last_print = 0;
    while (us() - t0 < 6000000) {
        long long now = us();
        for (int i = 0; i < NQ; i++) {
            errno = 0;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, di[i]) == 0) ioctl(fd, USBDEVFS_SUBMITURB, ins[i]);
            errno = 0;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, do_[i]) == 0) ioctl(fd, USBDEVFS_SUBMITURB, outs[i]);
        }
        if (now - t0 >= 2000000LL * (last_print + 1)) {
            last_print++;
            unsigned char v17[4];
            readreg(fd, 0x17, v17);
            printf("  t=%ds 0x17=%02X %02X %02X %02X  (byte2 bit7=%d)\n",
                   last_print * 2, v17[0], v17[1], v17[2], v17[3], (v17[2] >> 7) & 1);
        }
    }
    return 0;
}

int main(void) {
    int fd = open(DEV, O_RDWR);
    int iface = 5;
    ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    run_session(fd, 0xC350, "48k code");
    run_session(fd, 0xB372, "44.1k-derived");
    run_session(fd, 0xC350, "48k again");
    close(fd);
    return 0;
}
