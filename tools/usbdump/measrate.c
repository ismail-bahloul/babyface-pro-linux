// Measure the device's ACTUAL stream rate: count 14336-B IN completions
// over 2s and dump a few frame bytes. rate = completions/s * 256 frames.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <linux/usbdevice_fs.h>
#define DEV "/dev/bus/usb/003/004"
#define LEN 14336
#define NQ 9

static int fd;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0x40; c.bRequest=req; c.wValue=val; c.wIndex=idx; c.timeout=1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl req=%02x errno=%d\n", req, errno);
}
static long long us(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}
int main(void) {
    fd = open(DEV, O_RDWR);
    struct usbdevfs_setinterface si = {5, 1};
    ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    for (int i = 0; i <= 0x3D; i++) { if (i == 0x1E || i == 0x1F) continue; ctl(0x16, 0x0000, i); }
    ctl(0x1B, 0xC350, 0x0000); ctl(0x1B, 0x8DB8, 0xD201);
    ctl(0x1B, 0x8234, 0xD302); ctl(0x1B, 0x7CFF, 0xF803);
    ctl(0x10, 0x0021, 0x05FF);
    ctl(0x17, 0x000C, 0x0000); ctl(0x21, 0x0000, 0x0000);
    ctl(0x10, 0x0000, 0x3000); ctl(0x10, 0x0000, 0x3000);
    ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800); ctl(0x10, 0x0800, 0x0800);
    ctl(0x10, 0x0000, 0x8000);
    ctl(0x1D, 0x0000, 0x0000);
    struct usbdevfs_urb *outs[NQ], *ins[NQ], *do_[NQ], *di[NQ];
    struct usbdevfs_urb *done = NULL;
    for (int i = 0; i < NQ; i++) {
        outs[i] = calloc(1, sizeof(*outs[i]));
        outs[i]->type = USBDEVFS_URB_TYPE_INTERRUPT;
        outs[i]->endpoint = 0x01;
        outs[i]->buffer = calloc(1, LEN);
        outs[i]->buffer_length = LEN;
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
    ctl(0x14, 0x0000, 0xC000);
    long long t0 = us();
    long long cnt = 0;
    int printed = 0;
    long long report = t0;
    while (us() - t0 < 4000000) {
        for (int i = 0; i < NQ; i++) {
            errno = 0;
            done = NULL;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
                if (done->endpoint == 0x82) {
                    cnt++;
                    if (printed < 10) {
                        unsigned char *b = (unsigned char *)done->buffer;
                        printf("  IN actual=%d  first9=%02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                               done->actual_length, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8]);
                        if (done->actual_length >= 224) {
                            unsigned int *fr = (unsigned int *)(b + 2 * 56);
                            printf("    frame2: ch0=%08X ch1=%08X ch4=%08X ch5=%08X\n", fr[0], fr[1], fr[4], fr[5]);
                        }
                        printed++;
                    }
                }
                ioctl(fd, USBDEVFS_SUBMITURB, done);
            }
        }
        if (us() >= report) {
            double dt = (us() - t0) / 1000000.0;
            printf("  t=%.1fs completions=%lld rate=%.1f/s (%.1f kHz)\n",
                   dt, cnt, cnt / dt, cnt / dt * 256 / 1000.0);
            report = us() + 1000000;
        }
    }
    close(fd);
    return 0;
}
