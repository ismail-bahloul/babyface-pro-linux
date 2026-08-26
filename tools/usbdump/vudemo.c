// Input VU-meter demo: stream + 48V AN1 + routing, compute the
// per-channel level from the IN stream (ch0=AN1, ch1=AN2, ch2=AN3,
// ch3=AN4) and draw terminal bars with peak hold + decay.
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
#define NQ 9
#define NCH 4          /* AN1-4 */
#define REF 8388608.0  /* 2^23 full scale */
#define SEG 50         /* bar segments (0 dB .. -50 dB) */
#define HOLD_S 2.0     /* peak-hold decay time */

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
static void draw(const char *tag, double db, double dbhold) {
    /* bar spans 0 dB .. -50 dB; clamp */
    double d = db > 0 ? 0 : (db < -50 ? -50 : db);
    double dh = dbhold > 0 ? 0 : (dbhold < -50 ? -50 : dbhold);
    int n = (int)((d + 50) * SEG / 50.0);
    int h = (int)((dh + 50) * SEG / 50.0);
    printf("%-4s |", tag);
    for (int i = 0; i < SEG; i++) {
        if (i == h) printf("#");
        else if (i < n) printf("=");
        else printf(" ");
    }
    printf("| %6.1f dB", db);
    if (dbhold > -60) printf("  (peak %6.1f)", dbhold);
    printf("\n");
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
    ctl(0x17, 0x000D, 0x003F); ctl(0x21, 0x0000, 0x0000);   /* 48V AN1 */
    ctl(0x1A, (17 & 0x1F) | 0x20, 0x0000);                   /* gain AN1 */
    ctl(0x12, 0x4000, 0x0034 | 0xC000);
    ctl(0x12, 0x4000, 0x004E | 0xC000);
    ctl(0x1A, 0x00F3, 0x0004); ctl(0x1A, 0x00F3, 0x0005);
    ctl(0x12, 0x4000, 0x03E0 | 0xC000);
    ctl(0x12, 0x4000, 0x03E1 | 0xC000);

    const char *names[NCH] = {"AN1", "AN2", "AN3", "AN4"};
    double peak[NCH] = {0}, hold[NCH] = {0};
    long long last_hold = us();
    long long t0 = us();
    long long last_draw = 0;
    long long last_peak = 0;
    printf(">>> INPUT VU METERS — SPEAK INTO THE MIC (AN1), 12s <<<\n");
    while (us() - t0 < 12000000) {
        struct usbdevfs_urb *done = NULL;
        errno = 0;
        if (ioctl(fd, USBDEVFS_REAPURBNDELAY, &done) == 0) {
            if (done->endpoint == 0x82 && done->actual_length >= 56) {
                unsigned int *fr = (unsigned int *)done->buffer;
                for (int f = 0; f < done->actual_length / 56; f++) {
                    for (int c = 0; c < NCH; c++) {
                        int v = ((int)fr[f * 14 + c]) >> 8;
                        double a = v < 0 ? -(double)v : (double)v;
                        if (a > peak[c]) peak[c] = a;
                    }
                }
            }
            ioctl(fd, USBDEVFS_SUBMITURB, done);
        }
        long long now = us();
        if (now - last_hold > 200000) {
            for (int c = 0; c < NCH; c++) {
                if (peak[c] > hold[c]) hold[c] = peak[c];
                /* decay the hold */
                hold[c] *= 0.86;
                peak[c] = 0;
            }
            last_hold = now;
        }
        if (now - last_draw > 500000) {
            last_draw = now;
            printf("\n--- t=%.1fs ---\n", (now - t0) / 1e6);
            for (int c = 0; c < NCH; c++) {
                double db = peak[c] > 0 ? 20 * log10(peak[c] / REF) : -120;
                double dbh = hold[c] > 0 ? 20 * log10(hold[c] / REF) : -120;
                draw(names[c], db > -60 ? db : -60, dbh > -60 ? dbh : -60);
            }
            fflush(stdout);
        }
    }
    ctl(0x13, 0x0000, 0xC000);
    close(fd);
    return 0;
}
