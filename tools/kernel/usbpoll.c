// usbpoll.c — TotalMix-style status polling: the 5-read cycle
// (0x1c, 0x1e, 0x1f, 0x17, 0x11) at ~250 reads/s (50 cycles/s, 20 ms).
// Usage: usbpoll <bus/dev> <seconds>
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
	uint8_t reqs[5] = { 0x1c, 0x1e, 0x1f, 0x17, 0x11 };
	uint8_t buf[4];
	long t0 = time(0);
	unsigned long cycles = 0;
	while (time(0) - t0 < secs) {
		for (int i = 0; i < 5; i++) {
			struct usbdevfs_ctrltransfer ctrl = {
				.bRequestType = USB_DIR_IN | USB_TYPE_VENDOR |
						USB_RECIP_DEVICE,
				.bRequest = reqs[i],
				.wValue = 0,
				.wIndex = 0,
				.wLength = 4,
				.data = buf,
				.timeout = 50,
			};
			ioctl(fd, USBDEVFS_CONTROL, &ctrl);
		}
		cycles++;
	}
	printf("done: %lu cycles in %d s (%.0f/s, %.0f reads/s)\n",
	       cycles, secs, cycles / (double)secs, cycles * 5.0 / secs);
	close(fd);
	return 0;
}
