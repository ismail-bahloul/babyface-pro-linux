// usbread.c — bulk-IN reader for the Babyface Pro FS status endpoints.
// Claims an interface via usbfs (detaching the generic driver), then
// loops bulk reads on the given endpoint, discarding the data.
// Usage: usbread <bus/dev> <iface> <ep:hex> [seconds]
//   e.g. usbread 003/002 1 85 30     (read ep 0x85 for 30 s)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s bus/dev iface ep [seconds]\n", argv[0]);
		return 2;
	}
	char path[64];
	snprintf(path, sizeof(path), "/dev/bus/usb/%s", argv[1]);
	int fd = open(path, O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	int iface = atoi(argv[2]);
	unsigned ep = strtoul(argv[3], NULL, 16);
	int secs = argc > 4 ? atoi(argv[4]) : 0;
	unsigned char buf[16384];
	unsigned long total = 0, n = 0;

	/* Detach whatever driver owns the interface (generic, etc.). */
	struct usbdevfs_disconnect_claim dc = {
		.interface = iface,
		.flags = USBDEVFS_DISCONNECT_CLAIM_IF_DRIVER,
		.driver = "",
	};
	if (ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &dc) < 0) {
		perror("DISCONNECT_CLAIM");
		return 1;
	}
	printf("claimed iface %d, reading ep 0x%02x\n", iface, ep);

	long t0 = time(0);
	do {
		int r = ioctl(fd, USBDEVFS_BULK,
			      &(struct usbdevfs_bulktransfer){
				      .ep = (__u32)ep | 0x80,
				      .len = sizeof(buf),
				      .data = buf,
				      .timeout = 1000,
			      });
		if (r < 0) {
			perror("BULK");
			break;
		}
		total += r;
		n++;
	} while (secs <= 0 || time(0) - t0 < secs);
	printf("ep 0x%02x: %lu transfers, %lu bytes total\n", ep, n, total);
	ioctl(fd, USBDEVFS_RELEASEINTERFACE, iface);
	close(fd);
	return 0;
}
