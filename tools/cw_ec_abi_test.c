// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "cw_ethercat_uapi.h"

static int expect_errno(const char *name, int result, int expected)
{
	if (result == -1 && errno == expected) {
		printf("PASS: %s returned %s\n", name, strerror(expected));
		return 0;
	}

	fprintf(stderr, "FAIL: %s: expected %s, got %s\n", name,
		strerror(expected), result == -1 ? strerror(errno) : "success");
	return 1;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/cw_ethercat0";
	struct cw_ec_slave_info slave;
	unsigned long unknown_ioctl = _IO(CW_EC_IOC_MAGIC, 0x7f);
	int failures = 0;
	int second_fd;
	int fd;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [device]\n", argv[0]);
		return 2;
	}
	if (argc == 2)
		device = argv[1];

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "cw_ec_abi_test: cannot open %s: %s\n",
			device, strerror(errno));
		return 1;
	}

	errno = 0;
	second_fd = open(device, O_RDWR | O_CLOEXEC);
	failures += expect_errno("second control open", second_fd, EBUSY);
	if (second_fd >= 0)
		close(second_fd);

	errno = 0;
	failures += expect_errno("unknown ioctl",
				 ioctl(fd, unknown_ioctl, NULL), ENOTTY);

	memset(&slave, 0, sizeof(slave));
	slave.struct_size = sizeof(slave) - 1;
	slave.api_major = CW_EC_API_VERSION_MAJOR;
	errno = 0;
	failures += expect_errno("short slave structure",
				 ioctl(fd, CW_EC_IOC_GET_SLAVE_INFO, &slave),
				 EINVAL);

	memset(&slave, 0, sizeof(slave));
	slave.struct_size = sizeof(slave);
	slave.api_major = CW_EC_API_VERSION_MAJOR + 1;
	errno = 0;
	failures += expect_errno("wrong API major",
				 ioctl(fd, CW_EC_IOC_GET_SLAVE_INFO, &slave),
				 EPROTONOSUPPORT);

	memset(&slave, 0, sizeof(slave));
	slave.struct_size = sizeof(slave);
	slave.api_major = CW_EC_API_VERSION_MAJOR;
	slave.position = UINT16_MAX;
	errno = 0;
	failures += expect_errno("invalid slave position",
				 ioctl(fd, CW_EC_IOC_GET_SLAVE_INFO, &slave),
				 ENOENT);

	if (close(fd) < 0) {
		fprintf(stderr, "FAIL: close: %s\n", strerror(errno));
		failures++;
	}

	if (failures) {
		fprintf(stderr, "cw_ec_abi_test: %d failure(s)\n", failures);
		return 1;
	}

	return 0;
}
