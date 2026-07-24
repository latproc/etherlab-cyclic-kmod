// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "cw_ethercat_uapi.h"

static int require_ioctl(int fd, unsigned long request, void *argument,
			 const char *operation)
{
	if (ioctl(fd, request, argument) == 0)
		return 0;
	fprintf(stderr, "cw_ec_config_stress: %s: %s\n",
		operation, strerror(errno));
	return -1;
}

static int require_limit(int fd, unsigned long request, void *argument,
			 const char *operation)
{
	errno = 0;
	if (ioctl(fd, request, argument) < 0 && errno == E2BIG)
		return 0;
	fprintf(stderr,
		"cw_ec_config_stress: %s did not return E2BIG: %s\n",
		operation, errno ? strerror(errno) : "unexpected success");
	return -1;
}

static int stress_setup(int fd)
{
	struct cw_ec_setup_begin begin = {
		.struct_size = sizeof(begin),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_setup_sdo sdo = {
		.struct_size = sizeof(sdo),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.position = UINT16_MAX,
		.index = 0x6060,
		.type = CW_EC_SDO_U8,
		.data_len = 1,
	};
	uint32_t i;

	if (require_ioctl(fd, CW_EC_IOC_SETUP_BEGIN, &begin, "setup begin"))
		return -1;
	for (i = 0; i < CW_EC_SETUP_SDO_MAX; i++) {
		sdo.sequence = i + 1;
		sdo.subindex = i & UINT8_MAX;
		sdo.data[0] = i;
		if (require_ioctl(fd, CW_EC_IOC_SETUP_ADD_SDO, &sdo,
				  "setup add"))
			return -1;
	}
	sdo.sequence++;
	if (require_limit(fd, CW_EC_IOC_SETUP_ADD_SDO, &sdo, "setup limit"))
		return -1;
	return require_ioctl(fd, CW_EC_IOC_SETUP_RESET, &begin, "setup reset");
}

static int stress_config(int fd)
{
	struct cw_ec_config_begin begin = {
		.struct_size = sizeof(begin),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_config_slave slave = {
		.struct_size = sizeof(slave),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.vendor_id = 1,
		.product_code = 1,
	};
	struct cw_ec_config_sync sync = {
		.struct_size = sizeof(sync),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.slave_config_id = 1,
		.direction = CW_EC_DIR_OUTPUT,
		.watchdog_mode = CW_EC_WD_DEFAULT,
	};
	struct cw_ec_config_pdo pdo = {
		.struct_size = sizeof(pdo),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.sync_config_id = 1,
	};
	struct cw_ec_config_entry entry = {
		.struct_size = sizeof(entry),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.pdo_config_id = 1,
		.subindex = 0,
		.bit_length = 1,
	};
	struct cw_ec_config_dc dc = {
		.struct_size = sizeof(dc),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.slave_config_id = 1,
		.assign_activate = 0x0300,
		.sync0_cycle_ns = 1000000,
	};
	struct cw_ec_config_domain domain = {
		.struct_size = sizeof(domain),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_config_domain_assignment assignment = {
		.struct_size = sizeof(assignment),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.slave_config_id = 1,
		.domain_config_id = 1,
	};
	uint32_t i;

	if (require_ioctl(fd, CW_EC_IOC_CONFIG_BEGIN, &begin, "config begin"))
		return -1;
	for (i = 0; i < CW_EC_CONFIG_SLAVE_MAX; i++) {
		slave.config_id = i + 1;
		slave.position = i;
		if (require_ioctl(fd, CW_EC_IOC_CONFIG_ADD_SLAVE, &slave,
				  "slave add"))
			return -1;
	}
	slave.config_id++;
	slave.position++;
	if (require_limit(fd, CW_EC_IOC_CONFIG_ADD_SLAVE, &slave,
			  "slave limit"))
		return -1;

	for (i = 0; i < CW_EC_CONFIG_SYNC_MAX; i++) {
		sync.config_id = i + 1;
		sync.sync_index = i % 8;
		sync.direction = i & 1 ? CW_EC_DIR_INPUT : CW_EC_DIR_OUTPUT;
		if (require_ioctl(fd, CW_EC_IOC_CONFIG_ADD_SYNC, &sync,
				  "sync add"))
			return -1;
	}
	sync.config_id++;
	if (require_limit(fd, CW_EC_IOC_CONFIG_ADD_SYNC, &sync, "sync limit"))
		return -1;

	for (i = 0; i < CW_EC_CONFIG_PDO_MAX; i++) {
		pdo.config_id = i + 1;
		pdo.pdo_index = (i % UINT16_MAX) + 1;
		if (require_ioctl(fd, CW_EC_IOC_CONFIG_ADD_PDO, &pdo,
				  "PDO add"))
			return -1;
	}
	pdo.config_id++;
	if (require_limit(fd, CW_EC_IOC_CONFIG_ADD_PDO, &pdo, "PDO limit"))
		return -1;

	for (i = 0; i < CW_EC_CONFIG_ENTRY_MAX; i++) {
		entry.config_id = i + 1;
		entry.entry_id = i + 1;
		entry.index = (i % UINT16_MAX) + 1;
		if (require_ioctl(fd, CW_EC_IOC_CONFIG_ADD_ENTRY, &entry,
				  "entry add"))
			return -1;
	}
	entry.config_id++;
	entry.entry_id++;
	if (require_limit(fd, CW_EC_IOC_CONFIG_ADD_ENTRY, &entry,
			  "entry limit"))
		return -1;

	for (i = 0; i < CW_EC_CONFIG_DC_MAX; i++) {
		dc.config_id = i + 1;
		if (require_ioctl(fd, CW_EC_IOC_CONFIG_ADD_DC, &dc, "DC add"))
			return -1;
	}
	dc.config_id++;
	if (require_limit(fd, CW_EC_IOC_CONFIG_ADD_DC, &dc, "DC limit"))
		return -1;

	for (i = 0; i < CW_EC_CONFIG_DOMAIN_MAX; i++) {
		domain.config_id = i + 1;
		if (require_ioctl(fd, CW_EC_IOC_CONFIG_ADD_DOMAIN, &domain,
				  "domain add"))
			return -1;
	}
	domain.config_id++;
	if (require_limit(fd, CW_EC_IOC_CONFIG_ADD_DOMAIN, &domain,
			  "domain limit"))
		return -1;

	for (i = 0; i < CW_EC_CONFIG_SLAVE_MAX; i++) {
		assignment.config_id = i + 1;
		if (require_ioctl(fd, CW_EC_IOC_CONFIG_ASSIGN_DOMAIN,
				  &assignment, "domain assignment add"))
			return -1;
	}
	assignment.config_id++;
	if (require_limit(fd, CW_EC_IOC_CONFIG_ASSIGN_DOMAIN, &assignment,
			  "domain assignment limit"))
		return -1;

	/* A new transaction must synchronously free the maximum pending set. */
	return require_ioctl(fd, CW_EC_IOC_CONFIG_BEGIN, &begin,
			     "maximum config reset");
}

int main(int argc, char **argv)
{
	const char *device = "/dev/cw_ethercat0";
	unsigned long iterations = 1;
	char *end;
	int fd;

	if (argc > 3) {
		fprintf(stderr, "usage: %s [ITERATIONS [DEVICE]]\n", argv[0]);
		return 2;
	}
	if (argc >= 2) {
		errno = 0;
		iterations = strtoul(argv[1], &end, 10);
		if (errno || *end || !iterations || iterations > 100) {
			fprintf(stderr, "invalid iteration count\n");
			return 2;
		}
	}
	if (argc == 3)
		device = argv[2];

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "cw_ec_config_stress: open %s: %s\n",
			device, strerror(errno));
		return 1;
	}
	for (unsigned long i = 0; i < iterations; i++) {
		if (stress_setup(fd) || stress_config(fd)) {
			close(fd);
			return 1;
		}
		printf("completed maximum pending create/reset iteration %lu/%lu\n",
		       i + 1, iterations);
	}
	if (close(fd) < 0) {
		fprintf(stderr, "cw_ec_config_stress: close: %s\n",
			strerror(errno));
		return 1;
	}
	printf("PASS: %lu maximum pending configuration iteration(s)\n",
	       iterations);
	return 0;
}
