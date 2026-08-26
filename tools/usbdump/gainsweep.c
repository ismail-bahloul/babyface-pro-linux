// Gain sweep with live level measurement: set raw gain 0,2,4,6,8,10,
// measure the ch0/ch1 peak from the IN stream for 1s each. The user
// speaks/taps the mic. Calibrates the raw->dB scale.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <math.h>
#include <linux/usbdevice_fs.h>
#define DEV "/dev/bus/usb/003/002"
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
    int iface = 5; ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    struct usbdevfs_setinterface si = {5, 1}; ioctl(fd, USBDEVFS_SETINTERFACE, &si);
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
    ctl(0x17, 0x000D, 0x003F);   /* 48V ON */
    ctl(0x21, 0x0000, 0x0000);
    printf("stream + 48V ON. Speak/tap the mic during the sweep.\n");

    unsigned char cycle = 0;
    int gains[] = {0, 2, 4, 6, 8, 10, 6, 2, 0};
    for (int g = 0; g < (int)(sizeof(gains)/sizeof(gains[0])); g++) {
        int val = gains[g];
        int counter = (g % 3 == 0) ? 0x20 : (g % 3 == 1 ? 0x00 : 0x40);
        ctl(0x1A, val | counter, 0x0000);   /* gain mic 1 */
        long long t0 = us();
        long long sum0 = 0, sum1 = 0, n = 0;
        while (us() - t0 < 1200000) {
            struct usbdevfs_urb *done = NULL;
            errno = 0;
            if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
                if (done->endpoint == 0x82 && done->actual_length >= 56) {
                    unsigned int *fr = (unsigned int *)done->buffer;
                    for (int f = 0; f < done->actual_length / 56; f++) {
                        int v0 = ((int)fr[f * 14 + 0]) >> 8;  /* 24-bit in bytes 1-3, sign-ext */
                        int v1 = ((int)fr[f * 14 + 1]) >> 8;
                        sum0 += (long long)v0 * v0;
                        sum1 += (long long)v1 * v1;
                        n++;
                    }
                }
                ioctl(fd, USBDEVFS_SUBMITURB, done);
            }
        }
        double rms0 = n ? sqrt((double)sum0 / n) : 0;
        double rms1 = n ? sqrt((double)sum1 / n) : 0;
        double db0 = rms0 > 0 ? 20 * log10(rms0 / 32768.0) : -120;
        printf("  gain raw 0x%02X (val %2d): ch0 RMS=%9.1f (%6.1f dBFS)  ch1 RMS=%9.1f (%6.1f dBFS)\n",
               val | counter, val, rms0, db0, rms1, 20 * log10(rms1 / 32768.0));
    }
    ctl(0x1A, 0x00 | 0x20, 0x0000);   /* gain back to 0 */
    close(fd);
    return 0;
}
