// wdpat.c — replicate the exact Windows keepalive pattern:
//  - the 5-register control read cycle (0x1c,0x1e,0x1f,0x17,0x11) at
//    50 cycles/s (20 ms period, like the Windows driver)
//  - bulk IN reads on ep 0x85 at 480 B, ~45/s (like the Windows driver)
// Usage: wdpat <bus/dev> <seconds>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>

static int ctrl_read(int fd, uint8_t req, uint8_t *buf)
{
	struct usbdevfs_ctrltransfer ctrl = {
		.bRequestType = USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		.bRequest = req,
		.wValue = 0,
		.wIndex = 0,
		.wLength = 4,
		.data = buf,
		.timeout = 50,
	};
	return ioctl(fd, USBDEVFS_CONTROL, &ctrl);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s bus/dev seconds\n", argv[0]);
		return 2;
	}
	char path[64];
	snprintf(path, sizeof(path), "/dev/bus/usb/%s", argv[1]);
	int fd = open(path, O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	int secs = atoi(argv[2]);

	/* Claim iface 1 for the bulk reads (detach the generic driver). */
	struct usbdevfs_disconnect_claim dc = {
		.interface = 1,
		.flags = USBDEVFS_DISCONNECT_CLAIM_IF_DRIVER,
		.driver = "",
	};
	if (ioctl(fd, USBDEVFS_DISCONNECT_CLAIM, &dc) < 0)
		perror("DISCONNECT_CLAIM iface 1");

	uint8_t reqs[5] = { 0x1c, 0x1e, 0x1f, 0x17, 0x11 };
	uint8_t buf[512];
	long t0 = time(0);
	unsigned long cycles = 0, bulk_n = 0;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long long next_cycle_ms = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
	long long next_bulk_ms = next_cycle_ms;
	while (time(0) - t0 < secs) {
		clock_gettime(CLOCK_MONOTONIC, &ts);
		long long now = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
		if (now >= next_cycle_ms) {
			for (int i = 0; i < 5; i++)
				ctrl_read(fd, reqs[i], buf);
			cycles++;
			next_cycle_ms += 20;	/* 50 cycles/s */
			if (next_cycle_ms < now)
				next_cycle_ms = now;
		}
		if (now >= next_bulk_ms) {
			struct usbdevfs_bulktransfer bt = {
				.ep = 0x85,
				.len = 480,
				.data = buf,
				.timeout = 50,
			};
			if (ioctl(fd, USBDEVFS_BULK, &bt) >= 0)
				bulk_n++;
			next_bulk_ms += 22;	/* ~45/s */
			if (next_bulk_ms < now)
				next_bulk_ms = now;
		}
		usleep(100);
	}
	printf("done: %lu cycles (%lu/s), %lu bulk reads (%lu/s)\n",
	       cycles, cycles / (unsigned long)secs,
	       bulk_n, bulk_n / (unsigned long)secs);
	close(fd);
	return 0;
}
