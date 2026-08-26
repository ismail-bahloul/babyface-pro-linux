// ISO test on interface 0 (the TRUE isochronous pair per the descriptor:
// bmAttributes 0x01, alt 1 = 420/396 B) — does it complete?
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

#define DEV "/dev/bus/usb/003/002"

static int try_iso(int fd, int ep, int npkts, int pkt, int secs, const char *tag) {
    struct usbdevfs_urb *u = calloc(1, sizeof(*u) + npkts * sizeof(struct usbdevfs_iso_packet_desc));
    unsigned char *buf = calloc(1, npkts * pkt);
    u->type = USBDEVFS_URB_TYPE_ISO;
    u->endpoint = ep;
    u->flags = USBDEVFS_URB_ISO_ASAP;
    u->buffer = buf;
    u->buffer_length = npkts * pkt;
    u->number_of_packets = npkts;
    for (int i = 0; i < npkts; i++) u->iso_frame_desc[i].length = pkt;
    errno = 0;
    int r = ioctl(fd, USBDEVFS_SUBMITURB, u);
    if (r < 0) { printf("  [%s] submit FAILED errno=%d (%s)\n", tag, errno, strerror(errno)); free(u); free(buf); return -1; }
    fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
    struct timeval tv = {secs, 0};
    int s = select(fd + 1, &fds, NULL, NULL, &tv);
    if (s <= 0) { printf("  [%s] NO COMPLETION in %ds\n", tag, secs); free(u); free(buf); return -1; }
    r = ioctl(fd, USBDEVFS_REAPURB, u);
    printf("  [%s] COMPLETED status=%d actual=%d errcnt=%d\n", tag, u->status, u->actual_length, u->error_count);
    free(u); free(buf);
    return r;
}

int main(void) {
    int fd = open(DEV, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    int iface = 0;
    int r = ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface);
    printf("CLAIMINTERFACE(0): r=%d errno=%d\n", r, errno);
    struct usbdevfs_setinterface si = {0, 1};
    r = ioctl(fd, USBDEVFS_SETINTERFACE, &si);
    printf("SETINTERFACE(0,1): r=%d errno=%d\n", r, errno);

    try_iso(fd, 0x03, 8, 420, 3, "ISO OUT 8x420");
    try_iso(fd, 0x84, 8, 396, 3, "ISO IN 8x396");
    close(fd);
    return 0;
}
