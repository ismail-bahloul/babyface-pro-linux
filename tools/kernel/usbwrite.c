// usbwrite.c — raw vendor requests to the Babyface Pro FS (ep0, device level).
// Usage: usbwrite <bus/dev> w <req:hex> <value:hex> <index:hex>
//        usbwrite <bus/dev> r <req:hex> <index:hex>     (reads 4 bytes)
//   e.g. usbwrite 003/002 w 1a f3 0006   (Phones L 8-bit master = 0 dB)
//        usbwrite 003/002 r 11 0000      (status read)
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>

int main(int argc, char **argv)
{
	if (argc != 5 && argc != 6) {
		fprintf(stderr, "usage: %s bus/dev w req value idx | %s bus/dev r req idx\n",
			argv[0], argv[0]);
		return 2;
	}
	int is_read = argv[2][0] == 'r';
	char path[64];
	snprintf(path, sizeof(path), "/dev/bus/usb/%s", argv[1]);
	int fd = open(path, O_RDWR);
	if (fd < 0) { perror("open"); return 1; }
	uint8_t req = strtoul(argv[3], NULL, 16);
	uint16_t val = is_read ? 0 : strtoul(argv[4], NULL, 16);
	uint16_t idx = strtoul(argv[is_read ? 4 : 5], NULL, 16);
	uint8_t buf[4] = {0};

	struct usbdevfs_ctrltransfer ctrl = {
		.bRequestType = (is_read ? USB_DIR_IN : USB_DIR_OUT) |
				USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		.bRequest = req,
		.wValue = val,
		.wIndex = idx,
		.wLength = is_read ? 4 : 0,
		.data = buf,
		.timeout = 1000,
	};
	int ret = ioctl(fd, USBDEVFS_CONTROL, &ctrl);
	if (ret < 0)
		perror("USBDEVFS_CONTROL");
	else if (is_read)
		printf("read req=0x%02x idx=0x%04x -> %02x %02x %02x %02x\n",
		       req, idx, buf[0], buf[1], buf[2], buf[3]);
	else
		printf("write req=0x%02x val=0x%04x idx=0x%04x -> %d\n", req, val, idx, ret);
	close(fd);
	return ret < 0 ? 1 : 0;
}
