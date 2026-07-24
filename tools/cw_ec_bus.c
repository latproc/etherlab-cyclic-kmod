// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "cw_ethercat_uapi.h"

static const char *al_state_name(unsigned int state)
{
	switch (state) {
	case 1:
		return "INIT";
	case 2:
		return "PREOP";
	case 4:
		return "SAFEOP";
	case 8:
		return "OP";
	default:
		return "UNKNOWN";
	}
}

int main(int argc, char **argv)
{
	const char *device = "/dev/cw_ethercat0";
	struct cw_ec_api_version version;
	struct cw_ec_master_info master;
	unsigned int position;
	int fd;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [device]\n", argv[0]);
		return 2;
	}
	if (argc == 2)
		device = argv[1];

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "cw_ec_bus: cannot open %s: %s\n",
			device, strerror(errno));
		if (errno == EBUSY)
			fprintf(stderr,
				"cw_ec_bus: EtherLab master 0 is already owned\n");
		return 1;
	}

	memset(&version, 0, sizeof(version));
	if (ioctl(fd, CW_EC_IOC_GET_API_VERSION, &version) < 0) {
		fprintf(stderr, "cw_ec_bus: GET_API_VERSION: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}
	if (version.struct_size != sizeof(version) ||
	    version.major != CW_EC_API_VERSION_MAJOR) {
		fprintf(stderr,
			"cw_ec_bus: incompatible API: kernel %u.%u, tool %u.%u\n",
			version.major, version.minor, CW_EC_API_VERSION_MAJOR,
			CW_EC_API_VERSION_MINOR);
		close(fd);
		return 1;
	}

	memset(&master, 0, sizeof(master));
	if (ioctl(fd, CW_EC_IOC_GET_MASTER_INFO, &master) < 0) {
		fprintf(stderr, "cw_ec_bus: GET_MASTER_INFO: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}

	printf("API: %u.%u\n", version.major, version.minor);
	printf("master: 0\n");
	printf("link: %s\n", master.link_up ? "up" : "down");
	printf("scan: %s\n", master.scan_busy ? "busy" : "complete");
	printf("slaves: %" PRIu32 "\n\n", master.slave_count);
	printf("pos  alias  vendor      product     revision    state   err  name\n");

	for (position = 0; position < master.slave_count; position++) {
		struct cw_ec_slave_info slave = {
			.struct_size = sizeof(slave),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.position = position,
		};

		if (ioctl(fd, CW_EC_IOC_GET_SLAVE_INFO, &slave) < 0) {
			fprintf(stderr,
				"cw_ec_bus: GET_SLAVE_INFO position %u: %s\n",
				position, strerror(errno));
			close(fd);
			return 1;
		}

		printf("%-4" PRIu16 " %-6" PRIu16 " 0x%08" PRIx32
		       "  0x%08" PRIx32 "  0x%08" PRIx32 "  %-7s %-4u %s\n",
		       slave.position, slave.alias, slave.vendor_id,
		       slave.product_code, slave.revision_number,
		       al_state_name(slave.al_state), slave.error_flag,
		       slave.name);
	}

	if (close(fd) < 0) {
		fprintf(stderr, "cw_ec_bus: close: %s\n", strerror(errno));
		return 1;
	}

	return 0;
}
