#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/hdreg.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main()
{
	int fd = open("/dev/ex_blk", O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	unsigned long sectors;
	if (ioctl(fd, BLKGETSIZE, &sectors) == 0) {
		printf("Device size: %lu sectors (%lu MB)\n", sectors,
		       sectors * 512 / 1024 / 1024);
	}

	struct hd_geometry geo;
	if (ioctl(fd, HDIO_GETGEO, &geo) == 0) {
		printf("Geometry: heads=%d, sectors=%d, cylinders=%d\n",
		       geo.heads, geo.sectors, geo.cylinders);
	}

	unsigned long long size64;
	if (ioctl(fd, BLKGETSIZE64, &size64) == 0) {
		printf("Device size: %llu bytes (%llu MB)\n", size64,
		       size64 / 1024 / 1024);
	}

	close(fd);
	return 0;
}
