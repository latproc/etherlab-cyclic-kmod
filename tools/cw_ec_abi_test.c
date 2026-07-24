// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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
	struct cw_ec_config_entry config_padding = {
		.struct_size = sizeof(config_padding),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.config_id = 6,
		.pdo_config_id = 3,
		.entry_id = 0,
		.index = 0,
		.subindex = 0,
		.bit_length = 8,
	};
	struct cw_ec_config_dc config_dc = {
		.struct_size = sizeof(config_dc),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.config_id = 5,
		.slave_config_id = 1,
		.assign_activate = 0x0300,
		.sync0_cycle_ns = 1000000,
	};
	struct cw_ec_config_dc_policy dc_policy = {
		.struct_size = sizeof(dc_policy),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.reference_mode = CW_EC_DC_REFERENCE_EXPLICIT,
		.reference_slave_config_id = 1,
	};
	struct cw_ec_config_domain config_domain = {
		.struct_size = sizeof(config_domain),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.config_id = 10,
	};
	struct cw_ec_config_domain_assignment domain_assignment = {
		.struct_size = sizeof(domain_assignment),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.config_id = 11,
		.slave_config_id = 1,
		.domain_config_id = 10,
	};
	struct cw_ec_config_validate config_validate = {
		.struct_size = sizeof(config_validate),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_config_apply config_apply = {
		.struct_size = sizeof(config_apply),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_domain_create domain_create = {
		.struct_size = sizeof(domain_create),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_entry_offset entry_offset = {
		.struct_size = sizeof(entry_offset),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.entry_id = 1001,
	};
	struct cw_ec_cycle_activate cycle_activate = {
		.struct_size = sizeof(cycle_activate),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_cycle_status cycle_status = {
		.struct_size = sizeof(cycle_status),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_cycle_deactivate cycle_deactivate = {
		.struct_size = sizeof(cycle_deactivate),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_dc_status dc_status = {
		.struct_size = sizeof(dc_status),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_io_status io_status = {
		.struct_size = sizeof(io_status),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_config_slave_status slave_status = {
		.struct_size = sizeof(slave_status),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.config_id = 1,
	};
	uint8_t snapshot_byte;
	struct cw_ec_input_snapshot snapshot = {
		.struct_size = sizeof(snapshot),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.data_ptr = (uintptr_t)&snapshot_byte,
		.data_capacity = sizeof(snapshot_byte),
	};
	struct cw_ec_output_publish output = {
		.struct_size = sizeof(output),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.data_ptr = (uintptr_t)&snapshot_byte,
		.mask_ptr = (uintptr_t)&snapshot_byte,
		.data_size = sizeof(snapshot_byte),
		.config_generation = 1,
	};
	struct cw_ec_output_arm arm = {
		.struct_size = sizeof(arm),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.config_generation = 1,
		.output_sequence = 1,
	};
	struct cw_ec_output_disarm disarm = {
		.struct_size = sizeof(disarm),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.config_generation = 1,
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
	errno = 0;
	failures += expect_errno("DC add before begin",
				 ioctl(fd, CW_EC_IOC_CONFIG_ADD_DC,
				       &config_dc),
				 EINVAL);

	if (ioctl(fd, CW_EC_IOC_CONFIG_BEGIN, &config_begin) < 0) {
		fprintf(stderr, "FAIL: config begin: %s\n", strerror(errno));
		failures++;
	}
	config_padding.index = 1;
	errno = 0;
	failures += expect_errno("padding with nonzero object index",
				 ioctl(fd, CW_EC_IOC_CONFIG_ADD_ENTRY,
				       &config_padding),
				 EINVAL);
	config_padding.index = 0;
	config_padding.entry_id = 1;
	errno = 0;
	failures += expect_errno("zero object with registered entry ID",
				 ioctl(fd, CW_EC_IOC_CONFIG_ADD_ENTRY,
				       &config_padding),
				 EINVAL);
	config_padding.entry_id = 0;
	config_slave.revision_number = 1;
	errno = 0;
	failures += expect_errno("unsupported revision constraint",
				 ioctl(fd, CW_EC_IOC_CONFIG_ADD_SLAVE,
				       &config_slave),
				 EINVAL);
	config_slave.revision_number = 0;
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
	config_dc.assign_activate = 0;
	errno = 0;
	failures += expect_errno("zero DC AssignActivate",
				 ioctl(fd, CW_EC_IOC_CONFIG_ADD_DC,
				       &config_dc),
				 EINVAL);
	config_dc.assign_activate = 0x0300;
	config_dc.sync0_cycle_ns = 0;
	errno = 0;
	failures += expect_errno("zero DC SYNC0 cycle",
				 ioctl(fd, CW_EC_IOC_CONFIG_ADD_DC,
				       &config_dc),
				 EINVAL);
	config_dc.sync0_cycle_ns = 1000000;
	dc_policy.reference_slave_config_id = 0;
	errno = 0;
	failures += expect_errno("explicit DC reference without slave",
				 ioctl(fd, CW_EC_IOC_CONFIG_SET_DC_POLICY,
				       &dc_policy),
				 EINVAL);
	dc_policy.reference_slave_config_id = 1;

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
		fprintf(stderr, "FAIL: restart DC config transaction: %s\n",
			strerror(errno));
		failures++;
	}
	config_dc.slave_config_id = 99;
	if (ioctl(fd, CW_EC_IOC_CONFIG_ADD_DC, &config_dc) < 0) {
		fprintf(stderr, "FAIL: add orphan DC record: %s\n",
			strerror(errno));
		failures++;
	}
	errno = 0;
	failures += expect_errno("validate orphan DC record",
				 ioctl(fd, CW_EC_IOC_CONFIG_VALIDATE,
				       &config_validate),
				 ENOENT);
	config_dc.slave_config_id = config_slave.config_id;

	if (ioctl(fd, CW_EC_IOC_CONFIG_BEGIN, &config_begin) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_SLAVE, &config_slave) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_DOMAIN, &config_domain) < 0) {
		fprintf(stderr, "FAIL: start unassigned domain config: %s\n",
			strerror(errno));
		failures++;
	}
	errno = 0;
	failures += expect_errno("explicit domain without assignment",
				 ioctl(fd, CW_EC_IOC_CONFIG_VALIDATE,
				       &config_validate),
				 EINVAL);

	if (ioctl(fd, CW_EC_IOC_CONFIG_BEGIN, &config_begin) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_SLAVE, &config_slave) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_DOMAIN, &config_domain) < 0) {
		fprintf(stderr, "FAIL: start unknown domain assignment: %s\n",
			strerror(errno));
		failures++;
	}
	domain_assignment.domain_config_id = 99;
	if (ioctl(fd, CW_EC_IOC_CONFIG_ASSIGN_DOMAIN,
		  &domain_assignment) < 0) {
		fprintf(stderr, "FAIL: add unknown domain assignment: %s\n",
			strerror(errno));
		failures++;
	}
	errno = 0;
	failures += expect_errno("assignment to unknown domain",
				 ioctl(fd, CW_EC_IOC_CONFIG_VALIDATE,
				       &config_validate),
				 ENOENT);
	domain_assignment.domain_config_id = config_domain.config_id;

	if (ioctl(fd, CW_EC_IOC_CONFIG_BEGIN, &config_begin) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_SLAVE, &config_slave) < 0) {
		fprintf(stderr, "FAIL: restart config transaction: %s\n",
			strerror(errno));
		failures++;
	}
	config_sync.slave_config_id = config_slave.config_id;
	if (ioctl(fd, CW_EC_IOC_CONFIG_ADD_SYNC, &config_sync) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_PDO, &config_pdo) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_ENTRY, &config_padding) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_ENTRY, &config_entry) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_ADD_DC, &config_dc) < 0 ||
	    ioctl(fd, CW_EC_IOC_CONFIG_SET_DC_POLICY, &dc_policy) < 0) {
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
		   config_validate.entry_count != 2) {
		fprintf(stderr, "FAIL: validated config counts are incorrect\n");
		failures++;
	} else {
		printf("PASS: valid config hierarchy with padding accepted\n");
	}
	errno = 0;
	failures += expect_errno("config mutation after validation",
				 ioctl(fd, CW_EC_IOC_CONFIG_ADD_ENTRY,
				       &config_entry),
				 EINVAL);

	if (ioctl(fd, CW_EC_IOC_CONFIG_APPLY, &config_apply) < 0) {
		fprintf(stderr,
			"FAIL: apply config hierarchy: %s; kind=%u id=%" PRIu32
			"\n",
			strerror(errno), config_apply.failed_object_kind,
			config_apply.failed_config_id);
		failures++;
	} else {
		printf("PASS: validated config hierarchy applied to EtherLab\n");
	}
	errno = 0;
	failures += expect_errno("duplicate config apply",
				 ioctl(fd, CW_EC_IOC_CONFIG_APPLY,
				       &config_apply),
				 EINVAL);
	errno = 0;
	failures += expect_errno("config begin after apply",
				 ioctl(fd, CW_EC_IOC_CONFIG_BEGIN,
				       &config_begin),
				 EBUSY);

	if (ioctl(fd, CW_EC_IOC_DOMAIN_CREATE, &domain_create) < 0) {
		fprintf(stderr,
			"FAIL: create domain: %s; config id=%" PRIu32 "\n",
			strerror(errno), domain_create.failed_config_id);
		failures++;
	} else if (domain_create.entry_count != 1) {
		fprintf(stderr, "FAIL: domain registered incorrect entry count\n");
		failures++;
	} else {
		printf("PASS: domain created with one registered entry\n");
	}
	if (ioctl(fd, CW_EC_IOC_GET_ENTRY_OFFSET, &entry_offset) < 0) {
		fprintf(stderr, "FAIL: get entry offset: %s\n", strerror(errno));
		failures++;
	} else if (entry_offset.domain_offset != 1 ||
		   entry_offset.bit_position != 0 ||
		   entry_offset.bit_length != 16) {
		fprintf(stderr,
			"FAIL: unexpected entry mapping offset=%" PRIu32
			" bit=%u length=%u\n",
			entry_offset.domain_offset, entry_offset.bit_position,
			entry_offset.bit_length);
		failures++;
	} else {
		printf("PASS: padding skipped and stable entry ID resolved\n");
	}
	entry_offset.entry_id = 9999;
	errno = 0;
	failures += expect_errno("unknown entry ID",
				 ioctl(fd, CW_EC_IOC_GET_ENTRY_OFFSET,
				       &entry_offset),
				 ENOENT);
	errno = 0;
	failures += expect_errno("duplicate domain create",
				 ioctl(fd, CW_EC_IOC_DOMAIN_CREATE,
				       &domain_create),
				 EINVAL);

	if (ioctl(fd, CW_EC_IOC_CYCLE_GET_STATUS, &cycle_status) < 0) {
		fprintf(stderr, "FAIL: get inactive cycle status: %s\n",
			strerror(errno));
		failures++;
	} else if (cycle_status.active || cycle_status.cycle_count) {
		fprintf(stderr, "FAIL: initial cycle status is not inactive\n");
		failures++;
	} else {
		printf("PASS: initial cycle status is inactive\n");
	}
	errno = 0;
	failures += expect_errno("deactivate inactive cycle",
				 ioctl(fd, CW_EC_IOC_CYCLE_DEACTIVATE,
				       &cycle_deactivate),
				 EINVAL);
	if (ioctl(fd, CW_EC_IOC_CYCLE_GET_DC_STATUS, &dc_status) < 0) {
		fprintf(stderr, "FAIL: get inactive DC status: %s\n",
			strerror(errno));
		failures++;
	} else if (!dc_status.enabled || dc_status.reference_valid ||
		   dc_status.monitor_pending ||
		   dc_status.reference_read_error_count ||
		   dc_status.monitor_success_count) {
		fprintf(stderr, "FAIL: initial DC status is inconsistent\n");
		failures++;
	} else {
		printf("PASS: configured inactive DC status is clean\n");
	}
	if (ioctl(fd, CW_EC_IOC_GET_IO_STATUS, &io_status) < 0) {
		fprintf(stderr, "FAIL: get inactive IO status: %s\n",
			strerror(errno));
		failures++;
	} else if (!io_status.config_generation ||
		   io_status.bus_healthy || io_status.outputs_armed ||
		   io_status.rearm_required || io_status.fault_count ||
		   io_status.configured_slave_count != 1) {
		fprintf(stderr, "FAIL: initial IO status is inconsistent\n");
		failures++;
	} else {
		printf("PASS: generation-bound inactive IO status is clean\n");
	}
	slave_status.config_generation = io_status.config_generation + 1;
	errno = 0;
	failures += expect_errno("stale configured-slave status generation",
				 ioctl(fd,
				       CW_EC_IOC_GET_CONFIG_SLAVE_STATUS,
				       &slave_status),
				 ESTALE);
	slave_status.config_generation = io_status.config_generation;
	slave_status.config_id = 99;
	errno = 0;
	failures += expect_errno("unknown configured-slave status ID",
				 ioctl(fd,
				       CW_EC_IOC_GET_CONFIG_SLAVE_STATUS,
				       &slave_status),
				 ENOENT);
	slave_status.config_id = 1;
	if (ioctl(fd, CW_EC_IOC_GET_CONFIG_SLAVE_STATUS,
		  &slave_status) < 0) {
		fprintf(stderr,
			"FAIL: configured-slave inactive status failed: %s\n",
			strerror(errno));
		failures++;
	} else if (slave_status.active || slave_status.online ||
		   slave_status.operational || slave_status.data_valid ||
		   slave_status.state_result != -ENODATA) {
		fprintf(stderr,
			"FAIL: configured-slave inactive status is not clean\n");
		failures++;
	} else {
		printf("PASS: configured-slave inactive status is invalid\n");
	}
	snapshot.flags = 1;
	errno = 0;
	failures += expect_errno("unsupported input snapshot flags",
				 ioctl(fd, CW_EC_IOC_GET_INPUT_SNAPSHOT,
				       &snapshot),
				 EINVAL);
	snapshot.flags = 0;
	errno = 0;
	failures += expect_errno("input snapshot while inactive",
				 ioctl(fd, CW_EC_IOC_GET_INPUT_SNAPSHOT,
				       &snapshot),
				 EINVAL);
	output.flags = 1;
	errno = 0;
	failures += expect_errno("unsupported output publish flags",
				 ioctl(fd, CW_EC_IOC_PUBLISH_OUTPUT, &output),
				 EINVAL);
	output.flags = 0;
	errno = 0;
	failures += expect_errno("output publish while inactive",
				 ioctl(fd, CW_EC_IOC_PUBLISH_OUTPUT, &output),
				 EINVAL);
	arm.flags = 1;
	errno = 0;
	failures += expect_errno("unsupported output arm flags",
				 ioctl(fd, CW_EC_IOC_ARM_OUTPUTS, &arm),
				 EINVAL);
	arm.flags = 0;
	errno = 0;
	failures += expect_errno("output arm while inactive",
				 ioctl(fd, CW_EC_IOC_ARM_OUTPUTS, &arm),
				 EINVAL);
	disarm.flags = 1;
	errno = 0;
	failures += expect_errno("unsupported output disarm flags",
				 ioctl(fd, CW_EC_IOC_DISARM_OUTPUTS, &disarm),
				 EINVAL);
	disarm.flags = 0;
	errno = 0;
	failures += expect_errno("output disarm while inactive",
				 ioctl(fd, CW_EC_IOC_DISARM_OUTPUTS, &disarm),
				 EINVAL);
	cycle_activate.cycle_period_ns = CW_EC_CYCLE_PERIOD_MIN_NS - 1;
	errno = 0;
	failures += expect_errno("cycle period below minimum",
				 ioctl(fd, CW_EC_IOC_CYCLE_ACTIVATE,
				       &cycle_activate),
				 EINVAL);
	cycle_activate.cycle_period_ns = CW_EC_CYCLE_PERIOD_MAX_NS + 1U;
	errno = 0;
	failures += expect_errno("cycle period above maximum",
				 ioctl(fd, CW_EC_IOC_CYCLE_ACTIVATE,
				       &cycle_activate),
				 EINVAL);
	cycle_activate.cycle_period_ns = 1000000;
	cycle_activate.flags = 1;
	errno = 0;
	failures += expect_errno("unsupported cycle flags",
				 ioctl(fd, CW_EC_IOC_CYCLE_ACTIVATE,
				       &cycle_activate),
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
