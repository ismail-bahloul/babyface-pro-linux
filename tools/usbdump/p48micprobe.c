// Per-mic 48V probe: cycle candidate wVal values on 0x17 0x003F and let
// the user watch which P48 LEDs light (under the INPUT VU meters).
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#define DEV "/dev/bus/usb/003/002"
static int fd;
static void ctl(int req, int val, int idx) {
    struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0x40; c.bRequest=req; c.wValue=val; c.wIndex=idx; c.timeout=1000;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) < 0) printf("  ctl req=%02x errno=%d\n", req, errno);
}
static void v17(const char *s) {
    unsigned char b[4]={0}; struct usbdevfs_ctrltransfer c; memset(&c,0,sizeof(c));
    c.bRequestType=0xC0; c.bRequest=0x17; c.wLength=4; c.timeout=1000; c.data=b;
    if (ioctl(fd, USBDEVFS_CONTROL, &c) == 4)
        printf("  %-34s 0x17=%02X %02X %02X %02X\n", s, b[0], b[1], b[2], b[3]);
}
int main(void) {
    fd = open(DEV, O_RDWR);
    struct { int val; const char *desc; } tests[] = {
        {0x0D, "0x0D (mic1?  bit0)"},
        {0x0E, "0x0E (bit1)"},
        {0x0F, "0x0F (bits0+1)"},
        {0x09, "0x09 (bit0+bit3)"},
        {0x0B, "0x0B (bits0+1+3)"},
        {0x07, "0x07 (bits0+1+2)"},
        {0x1D, "0x1D (0x0D+PAD)"},
        {0x0C, "0x0C (all off)"},
    };
    for (int i = 0; i < (int)(sizeof(tests)/sizeof(tests[0])); i++) {
        ctl(0x17, tests[i].val, 0x003F);
        ctl(0x21, 0x0000, 0x0000);
        printf(">>> 48V %-22s — WATCH THE P48 LEDs (2.5s) <<<\n", tests[i].desc);
        v17("readback");
        sleep(2.5);
    }
    ctl(0x17, 0x000D, 0x003F); ctl(0x21, 0x0000, 0x0000);  /* restore mic1 on */
    close(fd);
    return 0;
}
