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
	struct cw_ec_setup_begin begin = {
		.struct_size = sizeof(begin),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_setup_sdo setup_sdo = {
		.struct_size = sizeof(setup_sdo),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.sequence = 1,
		.position = 0,
		.index = 0x2000,
		.subindex = 0,
		.type = CW_EC_SDO_U8,
		.data_len = 1,
		.data = { 0 },
	};
	struct cw_ec_setup_apply apply = {
		.struct_size = sizeof(apply),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_sdo_upload upload = {
		.struct_size = sizeof(upload),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.position = 0,
		.index = 0x2000,
		.subindex = 0,
	};
	struct cw_ec_config_begin config_begin = {
		.struct_size = sizeof(config_begin),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_config_slave config_slave = {
		.struct_size = sizeof(config_slave),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.config_id = 1,
		.position = 123,
		.vendor_id = 1,
		.product_code = 1,
	};
	struct cw_ec_config_sync config_sync = {
		.struct_size = sizeof(config_sync),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.config_id = 2,
		.slave_config_id = 1,
		.sync_index = 2,
		.direction = CW_EC_DIR_OUTPUT,
		.watchdog_mode = CW_EC_WD_DEFAULT,
	};
	struct cw_ec_config_pdo config_pdo = {
		.struct_size = sizeof(config_pdo),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.config_id = 3,
		.sync_config_id = 2,
		.pdo_index = 0x1600,
	};
	struct cw_ec_config_entry config_entry = {
		.struct_size = sizeof(config_entry),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.config_id = 4,
		.pdo_config_id = 3,
		.entry_id = 1001,
		.index = 0x6040,
		.bit_length = 16,
	};
	struct cw_ec_config_validate config_validate = {
		.struct_size = sizeof(config_validate),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
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

	errno = 0;
	failures += expect_errno("setup add before begin",
				 ioctl(fd, CW_EC_IOC_SETUP_ADD_SDO,
				       &setup_sdo),
				 EINVAL);

	if (ioctl(fd, CW_EC_IOC_SETUP_BEGIN, &begin) < 0) {
		fprintf(stderr, "FAIL: setup begin: %s\n", strerror(errno));
		failures++;
	}

	setup_sdo.data_len = 2;
	errno = 0;
	failures += expect_errno("setup scalar length mismatch",
				 ioctl(fd, CW_EC_IOC_SETUP_ADD_SDO,
				       &setup_sdo),
				 EINVAL);
	setup_sdo.data_len = 1;

	if (ioctl(fd, CW_EC_IOC_SETUP_ADD_SDO, &setup_sdo) < 0) {
		fprintf(stderr, "FAIL: valid setup add: %s\n", strerror(errno));
		failures++;
	}

	errno = 0;
	failures += expect_errno("duplicate setup sequence",
				 ioctl(fd, CW_EC_IOC_SETUP_ADD_SDO,
				       &setup_sdo),
				 EEXIST);

	if (ioctl(fd, CW_EC_IOC_SETUP_RESET, &begin) < 0) {
		fprintf(stderr, "FAIL: setup reset: %s\n", strerror(errno));
		failures++;
	}

	errno = 0;
	failures += expect_errno("apply empty setup batch",
				 ioctl(fd, CW_EC_IOC_SETUP_APPLY, &apply),
				 EINVAL);

	errno = 0;
	failures += expect_errno("zero-length SDO upload",
				 ioctl(fd, CW_EC_IOC_SDO_UPLOAD, &upload),
				 EINVAL);

	upload.requested_len = 1;
	upload.index = 0;
	errno = 0;
	failures += expect_errno("zero-index SDO upload",
				 ioctl(fd, CW_EC_IOC_SDO_UPLOAD, &upload),
				 EINVAL);

	errno = 0;
	failures += expect_errno("config add before begin",
				 ioctl(fd, CW_EC_IOC_CONFIG_ADD_SLAVE,
				       &config_slave),
				 EINVAL);

	if (ioctl(fd, CW_EC_IOC_CONFIG_BEGIN, &config_begin) < 0) {
		fprintf(stderr, "FAIL: config begin: %s\n", strerror(errno));
		failures++;
	}
	if (ioctl(fd, CW_EC_IOC_CONFIG_ADD_SLAVE, &config_slave) < 0) {
		fprintf(stderr, "FAIL: valid config slave: %s\n",
			strerror(errno));
		failures++;
	}
	errno = 0;
	failures += expect_errno("duplicate config slave ID",
				 ioctl(fd, CW_EC_IOC_CONFIG_ADD_SLAVE,
				       &config_slave),
				 EEXIST);

	config_sync.slave_config_id = 99;
	if (ioctl(fd, CW_EC_IOC_CONFIG_ADD_SYNC, &config_sync) < 0) {
		fprintf(stderr, "FAIL: add orphan config sync: %s\n",
			strerror(errno));
		failures++;
	}
	errno = 0;
	failures += expect_errno("validate orphan config sync",
				 ioctl(fd, CW_EC_IOC_CONFIG_VALIDATE,
				       &config_validate),
				 ENOENT);

	if (ioctl(fd, CW_EC_IOC_CONFIG_BEGIN, &config_begin) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_SLAVE, &config_slave) < 0) {
		fprintf(stderr, "FAIL: restart config transaction: %s\n",
			strerror(errno));
		failures++;
	}
	config_sync.slave_config_id = config_slave.config_id;
	if (ioctl(fd, CW_EC_IOC_CONFIG_ADD_SYNC, &config_sync) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_PDO, &config_pdo) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_ENTRY, &config_entry) < 0) {
		fprintf(stderr, "FAIL: add valid config hierarchy: %s\n",
			strerror(errno));
		failures++;
	}
	memset(&config_validate, 0, sizeof(config_validate));
	config_validate.struct_size = sizeof(config_validate);
	config_validate.api_major = CW_EC_API_VERSION_MAJOR;
	if (ioctl(fd, CW_EC_IOC_CONFIG_VALIDATE, &config_validate) < 0) {
		fprintf(stderr, "FAIL: validate config hierarchy: %s\n",
			strerror(errno));
		failures++;
	} else if (config_validate.slave_count != 1 ||
		   config_validate.sync_count != 1 ||
		   config_validate.pdo_count != 1 ||
		   config_validate.entry_count != 1) {
		fprintf(stderr, "FAIL: validated config counts are incorrect\n");
		failures++;
	} else {
		printf("PASS: valid config hierarchy accepted\n");
	}
	errno = 0;
	failures += expect_errno("config mutation after validation",
				 ioctl(fd, CW_EC_IOC_CONFIG_ADD_ENTRY,
				       &config_entry),
				 EINVAL);

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
