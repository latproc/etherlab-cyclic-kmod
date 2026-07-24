// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "cw_ethercat_uapi.h"

#define LINE_MAX_BYTES 1024U
#define TOKEN_MAX 10U

enum record_kind {
	RECORD_EMPTY,
	RECORD_SLAVE,
	RECORD_SYNC,
	RECORD_PDO,
	RECORD_ENTRY,
	RECORD_DC,
	RECORD_DC_POLICY,
};

struct record {
	enum record_kind kind;
	union {
		struct cw_ec_config_slave slave;
		struct cw_ec_config_sync sync;
		struct cw_ec_config_pdo pdo;
		struct cw_ec_config_entry entry;
		struct cw_ec_config_dc dc;
		struct cw_ec_config_dc_policy dc_policy;
	};
};

struct counts {
	uint32_t slaves;
	uint32_t syncs;
	uint32_t pdos;
	uint32_t entries;
	uint32_t dcs;
};

static void usage(const char *program)
{
	fprintf(stderr,
		"usage:\n"
		"  %s check CONFIG\n"
		"  %s prepare CONFIG [DEVICE]\n"
		"  %s cycle CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]\n"
		"  %s cycle-zero-arm CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]\n"
		"  %s cycle-zero-hold CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]\n",
		program, program, program, program, program);
}

static int parse_u64(const char *text, uint64_t maximum, uint64_t *value)
{
	char *end;
	uintmax_t parsed;

	if (!text[0] || text[0] == '-')
		return -1;
	errno = 0;
	parsed = strtoumax(text, &end, 0);
	if (errno || *end || parsed > maximum)
		return -1;
	*value = parsed;
	return 0;
}

static int parse_s32(const char *text, int32_t *value)
{
	char *end;
	intmax_t parsed;

	errno = 0;
	parsed = strtoimax(text, &end, 0);
	if (errno || !text[0] || *end || parsed < INT32_MIN ||
	    parsed > INT32_MAX)
		return -1;
	*value = parsed;
	return 0;
}

static int split_line(char *line, char *tokens[TOKEN_MAX])
{
	char *comment = strchr(line, '#');
	char *save = NULL;
	char *token;
	int count = 0;

	if (comment)
		*comment = '\0';
	for (token = strtok_r(line, " \t\r\n", &save); token;
	     token = strtok_r(NULL, " \t\r\n", &save)) {
		if (count == TOKEN_MAX)
			return -1;
		tokens[count++] = token;
	}
	return count;
}

static int parse_direction(const char *text, uint8_t *direction)
{
	if (!strcmp(text, "output"))
		*direction = CW_EC_DIR_OUTPUT;
	else if (!strcmp(text, "input"))
		*direction = CW_EC_DIR_INPUT;
	else
		return -1;
	return 0;
}

static int parse_watchdog(const char *text, uint8_t *watchdog)
{
	if (!strcmp(text, "default"))
		*watchdog = CW_EC_WD_DEFAULT;
	else if (!strcmp(text, "enable"))
		*watchdog = CW_EC_WD_ENABLE;
	else if (!strcmp(text, "disable"))
		*watchdog = CW_EC_WD_DISABLE;
	else
		return -1;
	return 0;
}

static int parse_record(char *line, struct record *record)
{
	char *tokens[TOKEN_MAX];
	uint64_t value;
	int count;

	memset(record, 0, sizeof(*record));
	count = split_line(line, tokens);
	if (count <= 0)
		return count;

	if (!strcmp(tokens[0], "slave") && count == 7) {
		record->kind = RECORD_SLAVE;
		record->slave.struct_size = sizeof(record->slave);
		record->slave.api_major = CW_EC_API_VERSION_MAJOR;
		if (parse_u64(tokens[1], UINT32_MAX, &value))
			return -1;
		record->slave.config_id = value;
		if (parse_u64(tokens[2], UINT16_MAX, &value))
			return -1;
		record->slave.alias = value;
		if (parse_u64(tokens[3], UINT16_MAX, &value))
			return -1;
		record->slave.position = value;
		if (parse_u64(tokens[4], UINT32_MAX, &value))
			return -1;
		record->slave.vendor_id = value;
		if (parse_u64(tokens[5], UINT32_MAX, &value))
			return -1;
		record->slave.product_code = value;
		if (parse_u64(tokens[6], UINT32_MAX, &value))
			return -1;
		record->slave.revision_number = value;
		return 1;
	}

	if (!strcmp(tokens[0], "sync") && count == 6) {
		record->kind = RECORD_SYNC;
		record->sync.struct_size = sizeof(record->sync);
		record->sync.api_major = CW_EC_API_VERSION_MAJOR;
		if (parse_u64(tokens[1], UINT32_MAX, &value))
			return -1;
		record->sync.config_id = value;
		if (parse_u64(tokens[2], UINT32_MAX, &value))
			return -1;
		record->sync.slave_config_id = value;
		if (parse_u64(tokens[3], UINT8_MAX, &value))
			return -1;
		record->sync.sync_index = value;
		if (parse_direction(tokens[4], &record->sync.direction) ||
		    parse_watchdog(tokens[5], &record->sync.watchdog_mode))
			return -1;
		return 1;
	}

	if (!strcmp(tokens[0], "pdo") && count == 4) {
		record->kind = RECORD_PDO;
		record->pdo.struct_size = sizeof(record->pdo);
		record->pdo.api_major = CW_EC_API_VERSION_MAJOR;
		if (parse_u64(tokens[1], UINT32_MAX, &value))
			return -1;
		record->pdo.config_id = value;
		if (parse_u64(tokens[2], UINT32_MAX, &value))
			return -1;
		record->pdo.sync_config_id = value;
		if (parse_u64(tokens[3], UINT16_MAX, &value))
			return -1;
		record->pdo.pdo_index = value;
		return 1;
	}

	if (!strcmp(tokens[0], "entry") && count == 7) {
		record->kind = RECORD_ENTRY;
		record->entry.struct_size = sizeof(record->entry);
		record->entry.api_major = CW_EC_API_VERSION_MAJOR;
		if (parse_u64(tokens[1], UINT32_MAX, &value))
			return -1;
		record->entry.config_id = value;
		if (parse_u64(tokens[2], UINT32_MAX, &value))
			return -1;
		record->entry.pdo_config_id = value;
		if (parse_u64(tokens[3], UINT32_MAX, &value))
			return -1;
		record->entry.entry_id = value;
		if (parse_u64(tokens[4], UINT16_MAX, &value))
			return -1;
		record->entry.index = value;
		if (parse_u64(tokens[5], UINT8_MAX, &value))
			return -1;
		record->entry.subindex = value;
		if (parse_u64(tokens[6], UINT8_MAX, &value))
			return -1;
		record->entry.bit_length = value;
		return 1;
	}

	if (!strcmp(tokens[0], "dc") && count == 8) {
		record->kind = RECORD_DC;
		record->dc.struct_size = sizeof(record->dc);
		record->dc.api_major = CW_EC_API_VERSION_MAJOR;
		if (parse_u64(tokens[1], UINT32_MAX, &value))
			return -1;
		record->dc.config_id = value;
		if (parse_u64(tokens[2], UINT32_MAX, &value))
			return -1;
		record->dc.slave_config_id = value;
		if (parse_u64(tokens[3], UINT16_MAX, &value))
			return -1;
		record->dc.assign_activate = value;
		if (parse_u64(tokens[4], UINT32_MAX, &value))
			return -1;
		record->dc.sync0_cycle_ns = value;
		if (parse_s32(tokens[5], &record->dc.sync0_shift_ns))
			return -1;
		if (parse_u64(tokens[6], UINT32_MAX, &value))
			return -1;
		record->dc.sync1_cycle_ns = value;
		if (parse_s32(tokens[7], &record->dc.sync1_shift_ns))
			return -1;
		return 1;
	}

	if (!strcmp(tokens[0], "dc_policy") && count == 3) {
		record->kind = RECORD_DC_POLICY;
		record->dc_policy.struct_size = sizeof(record->dc_policy);
		record->dc_policy.api_major = CW_EC_API_VERSION_MAJOR;
		if (!strcmp(tokens[1], "disabled"))
			record->dc_policy.reference_mode =
				CW_EC_DC_REFERENCE_DISABLED;
		else if (!strcmp(tokens[1], "auto"))
			record->dc_policy.reference_mode =
				CW_EC_DC_REFERENCE_AUTO;
		else if (!strcmp(tokens[1], "explicit"))
			record->dc_policy.reference_mode =
				CW_EC_DC_REFERENCE_EXPLICIT;
		else
			return -1;
		if (parse_u64(tokens[2], UINT32_MAX, &value))
			return -1;
		record->dc_policy.reference_slave_config_id = value;
		return 1;
	}

	return -1;
}

static int scan_config(const char *path, struct counts *counts)
{
	char line[LINE_MAX_BYTES];
	unsigned int line_number = 0;
	struct record record;
	FILE *file;

	memset(counts, 0, sizeof(*counts));
	file = fopen(path, "r");
	if (!file) {
		fprintf(stderr, "cw_ec_config: cannot open %s: %s\n",
			path, strerror(errno));
		return -1;
	}
	while (fgets(line, sizeof(line), file)) {
		int parsed;

		line_number++;
		if (!strchr(line, '\n') && !feof(file)) {
			fprintf(stderr, "%s:%u: line exceeds %u bytes\n",
				path, line_number, LINE_MAX_BYTES - 1);
			fclose(file);
			return -1;
		}
		parsed = parse_record(line, &record);
		if (parsed < 0) {
			fprintf(stderr, "%s:%u: invalid configuration record\n",
				path, line_number);
			fclose(file);
			return -1;
		}
		if (!parsed)
			continue;
		switch (record.kind) {
		case RECORD_SLAVE:
			counts->slaves++;
			break;
		case RECORD_SYNC:
			counts->syncs++;
			break;
		case RECORD_PDO:
			counts->pdos++;
			break;
		case RECORD_ENTRY:
			counts->entries++;
			break;
		case RECORD_DC:
			counts->dcs++;
			break;
		case RECORD_DC_POLICY:
			break;
		default:
			break;
		}
	}
	if (ferror(file)) {
		fprintf(stderr, "cw_ec_config: read %s: %s\n",
			path, strerror(errno));
		fclose(file);
		return -1;
	}
	fclose(file);
	if (!counts->slaves || !counts->entries ||
	    counts->slaves > CW_EC_CONFIG_SLAVE_MAX ||
	    counts->syncs > CW_EC_CONFIG_SYNC_MAX ||
	    counts->pdos > CW_EC_CONFIG_PDO_MAX ||
	    counts->entries > CW_EC_CONFIG_ENTRY_MAX ||
	    counts->dcs > CW_EC_CONFIG_DC_MAX) {
		fprintf(stderr, "cw_ec_config: invalid or excessive object counts\n");
		return -1;
	}
	return 0;
}

static int submit_record(int fd, const struct record *record)
{
	switch (record->kind) {
	case RECORD_SLAVE:
		return ioctl(fd, CW_EC_IOC_CONFIG_ADD_SLAVE, &record->slave);
	case RECORD_SYNC:
		return ioctl(fd, CW_EC_IOC_CONFIG_ADD_SYNC, &record->sync);
	case RECORD_PDO:
		return ioctl(fd, CW_EC_IOC_CONFIG_ADD_PDO, &record->pdo);
	case RECORD_ENTRY:
		return ioctl(fd, CW_EC_IOC_CONFIG_ADD_ENTRY, &record->entry);
	case RECORD_DC:
		return ioctl(fd, CW_EC_IOC_CONFIG_ADD_DC, &record->dc);
	case RECORD_DC_POLICY:
		return ioctl(fd, CW_EC_IOC_CONFIG_SET_DC_POLICY,
			     &record->dc_policy);
	default:
		return 0;
	}
}

static int submit_config(int fd, const char *path)
{
	char line[LINE_MAX_BYTES];
	unsigned int line_number = 0;
	struct record record;
	FILE *file = fopen(path, "r");

	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		int parsed;

		line_number++;
		parsed = parse_record(line, &record);
		if (parsed > 0 && submit_record(fd, &record) < 0) {
			fprintf(stderr, "%s:%u: kernel rejected record: %s\n",
				path, line_number, strerror(errno));
			fclose(file);
			return -1;
		}
	}
	fclose(file);
	return 0;
}

static int print_offsets(int fd, const char *path)
{
	char line[LINE_MAX_BYTES];
	struct record record;
	FILE *file = fopen(path, "r");

	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		struct cw_ec_entry_offset offset;

		if (parse_record(line, &record) <= 0 ||
		    record.kind != RECORD_ENTRY)
			continue;
		memset(&offset, 0, sizeof(offset));
		offset.struct_size = sizeof(offset);
		offset.api_major = CW_EC_API_VERSION_MAJOR;
		offset.entry_id = record.entry.entry_id;
		if (ioctl(fd, CW_EC_IOC_GET_ENTRY_OFFSET, &offset) < 0) {
			fprintf(stderr, "entry %" PRIu32 ": offset lookup: %s\n",
				offset.entry_id, strerror(errno));
			fclose(file);
			return -1;
		}
		printf("entry %" PRIu32 " object 0x%04" PRIx16 ":%02" PRIx8
		       " offset=%" PRIu32 " bit=%" PRIu8 " length=%" PRIu8
		       "\n",
		       record.entry.entry_id, record.entry.index,
		       record.entry.subindex, offset.domain_offset,
		       offset.bit_position, offset.bit_length);
	}
	fclose(file);
	return 0;
}

static int configure_fd(int fd, const char *path,
			struct cw_ec_config_validate *validated)
{
	struct cw_ec_config_begin begin = {
		.struct_size = sizeof(begin),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_config_validate validate = {
		.struct_size = sizeof(validate),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_config_apply apply = {
		.struct_size = sizeof(apply),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_domain_create domain = {
		.struct_size = sizeof(domain),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	if (ioctl(fd, CW_EC_IOC_CONFIG_BEGIN, &begin) < 0 ||
	    submit_config(fd, path) < 0)
		return -1;
	if (ioctl(fd, CW_EC_IOC_CONFIG_VALIDATE, &validate) < 0) {
		fprintf(stderr, "cw_ec_config: validation failed: %s\n",
			strerror(errno));
		return -1;
	}
	if (ioctl(fd, CW_EC_IOC_CONFIG_APPLY, &apply) < 0) {
		fprintf(stderr,
			"cw_ec_config: apply failed: %s; kind=%u id=%" PRIu32
			"\n",
			strerror(errno), apply.failed_object_kind,
			apply.failed_config_id);
		return -1;
	}
	if (ioctl(fd, CW_EC_IOC_DOMAIN_CREATE, &domain) < 0) {
		fprintf(stderr,
			"cw_ec_config: domain registration failed: %s; id=%"
			PRIu32 "\n",
			strerror(errno), domain.failed_config_id);
		return -1;
	}
	if (print_offsets(fd, path))
		return -1;
	*validated = validate;
	return 0;
}

static int print_slave_statuses(int fd, const char *path,
				uint64_t generation)
{
	FILE *stream;
	char *line = NULL;
	size_t capacity = 0;
	unsigned int line_number = 0;
	int ret = -1;

	stream = fopen(path, "r");
	if (!stream) {
		fprintf(stderr, "cw_ec_config: cannot open %s: %s\n",
			path, strerror(errno));
		return -1;
	}
	while (getline(&line, &capacity, stream) >= 0) {
		struct cw_ec_config_slave_status status = {
			.struct_size = sizeof(status),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.config_generation = generation,
		};
		struct record record;

		line_number++;
		if (parse_record(line, &record) < 0) {
			fprintf(stderr, "%s:%u: invalid record\n", path,
				line_number);
			goto out;
		}
		if (record.kind != RECORD_SLAVE)
			continue;
		status.config_id = record.slave.config_id;
		if (ioctl(fd, CW_EC_IOC_GET_CONFIG_SLAVE_STATUS,
			  &status) < 0) {
			fprintf(stderr,
				"cw_ec_config: slave status %" PRIu32
				" failed: %s\n",
				status.config_id, strerror(errno));
			goto out;
		}
		printf("slave status: id=%" PRIu32 " active=%u online=%u"
		       " operational=%u valid=%u al=0x%02x result=%" PRId32
		       " cycle=%" PRIu64 " input_sequence=%" PRIu64 "\n",
		       status.config_id, status.active, status.online,
		       status.operational, status.data_valid, status.al_state,
		       status.state_result, (uint64_t)status.cycle_count,
		       (uint64_t)status.input_sequence);
	}
	if (ferror(stream)) {
		fprintf(stderr, "cw_ec_config: read %s: %s\n",
			path, strerror(errno));
		goto out;
	}
	ret = 0;
out:
	free(line);
	fclose(stream);
	return ret;
}

static int prepare(const char *path, const char *device)
{
	struct cw_ec_config_validate validate;
	int fd = open(device, O_RDWR | O_CLOEXEC);
	int ret = 1;

	if (fd < 0) {
		fprintf(stderr, "cw_ec_config: cannot open %s: %s\n",
			device, strerror(errno));
		return 1;
	}
	if (configure_fd(fd, path, &validate))
		goto out;
	printf("prepared %u slave(s), %u sync manager(s), %u PDO(s), "
	       "%u entry/entries; master was not activated\n",
	       validate.slave_count, validate.sync_count, validate.pdo_count,
	       validate.entry_count);
	ret = 0;
out:
	if (close(fd) < 0) {
		fprintf(stderr, "cw_ec_config: close: %s\n", strerror(errno));
		ret = 1;
	}
	return ret;
}

static int cycle(const char *path, uint32_t period_ns,
		 unsigned int duration_seconds, bool arm_zero,
		 bool hold_zero,
		 const char *device)
{
	struct cw_ec_config_validate validate;
	struct cw_ec_cycle_activate activate = {
		.struct_size = sizeof(activate),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.cycle_period_ns = period_ns,
	};
	struct cw_ec_cycle_status status = {
		.struct_size = sizeof(status),
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
	struct cw_ec_input_snapshot snapshot = {
		.struct_size = sizeof(snapshot),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_output_publish output = {
		.struct_size = sizeof(output),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_output_arm arm = {
		.struct_size = sizeof(arm),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_output_disarm disarm = {
		.struct_size = sizeof(disarm),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	uint8_t *snapshot_data = NULL;
	uint8_t *output_data = NULL;
	uint8_t *output_mask = NULL;
	struct cw_ec_cycle_deactivate deactivate = {
		.struct_size = sizeof(deactivate),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	int fd = open(device, O_RDWR | O_CLOEXEC);
	bool active = false;
	bool held_armed = false;
	int ret = 1;

	if (fd < 0) {
		fprintf(stderr, "cw_ec_config: cannot open %s: %s\n",
			device, strerror(errno));
		return 1;
	}
	if (configure_fd(fd, path, &validate))
		goto out;
	if (ioctl(fd, CW_EC_IOC_CYCLE_ACTIVATE, &activate) < 0) {
		fprintf(stderr, "cw_ec_config: activation failed: %s\n",
			strerror(errno));
		goto out;
	}
	active = true;
	printf("activated zero-output domain: size=%" PRIu32
	       " period=%" PRIu32 " ns for %u second(s)\n",
	       activate.domain_size, period_ns, duration_seconds);
	if (hold_zero || arm_zero) {
		unsigned int attempts;

		for (attempts = 0; attempts < 100; attempts++) {
			io_status.struct_size = sizeof(io_status);
			io_status.api_major = CW_EC_API_VERSION_MAJOR;
			if (ioctl(fd, CW_EC_IOC_GET_IO_STATUS,
				  &io_status) < 0) {
				fprintf(stderr,
					"cw_ec_config: IO status failed: %s\n",
					strerror(errno));
				goto out;
			}
			if (io_status.bus_healthy)
				break;
			usleep(50000);
		}
		if (!io_status.bus_healthy) {
			fprintf(stderr,
				"cw_ec_config: bus did not become healthy for zero-output operation\n");
			goto out;
		}
	}
	if (hold_zero) {
		output_data = calloc(activate.domain_size, 1);
		output_mask = malloc(activate.domain_size);
		if (!output_data || !output_mask) {
			fprintf(stderr,
				"cw_ec_config: allocate zero hold image: %s\n",
				strerror(errno));
			goto out;
		}
		memset(output_mask, 0xff, activate.domain_size);
		output.data_ptr = (uintptr_t)output_data;
		output.mask_ptr = (uintptr_t)output_mask;
		output.data_size = activate.domain_size;
		output.config_generation = io_status.config_generation;
		if (ioctl(fd, CW_EC_IOC_PUBLISH_OUTPUT, &output) < 0) {
			fprintf(stderr,
				"cw_ec_config: zero hold publication failed: %s\n",
				strerror(errno));
			goto out;
		}
		arm.config_generation = output.config_generation;
		arm.output_sequence = output.output_sequence;
		if (ioctl(fd, CW_EC_IOC_ARM_OUTPUTS, &arm) < 0) {
			fprintf(stderr,
				"cw_ec_config: zero hold arm failed: %s\n",
				strerror(errno));
			goto out;
		}
		held_armed = true;
		printf("READY: zero-output shadow armed for controller hold"
		       " generation=%" PRIu64 " sequence=%" PRIu64 "\n",
		       (uint64_t)output.config_generation,
		       (uint64_t)output.output_sequence);
		fflush(stdout);
	}
	while (duration_seconds)
		duration_seconds = sleep(duration_seconds);
	if (ioctl(fd, CW_EC_IOC_CYCLE_GET_STATUS, &status) < 0) {
		fprintf(stderr, "cw_ec_config: status failed: %s\n",
			strerror(errno));
		goto out;
	}
	printf("cycle status: active=%u cycles=%" PRIu64
	       " errors=%" PRIu64 " overruns=%" PRIu64
	       " maximum_lateness=%" PRIu64 " ns wc=%" PRIu32
	       " wc_state=%u last_result=%" PRId32 "\n",
	       status.active, (uint64_t)status.cycle_count,
	       (uint64_t)status.cycle_error_count,
	       (uint64_t)status.cycle_overrun_count,
	       (uint64_t)status.maximum_lateness_ns,
	       status.working_counter, status.working_counter_state,
	       status.last_cycle_result);
	if (ioctl(fd, CW_EC_IOC_CYCLE_GET_DC_STATUS, &dc_status) < 0) {
		fprintf(stderr, "cw_ec_config: DC status failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (dc_status.enabled) {
		printf("DC status: reference_valid=%u reference_result=%" PRId32
		       " difference=%" PRId32 " ns adjustment=%" PRId32
		       " ns monitor_pending=%u deviation=%" PRIu32
		       " ns maximum_deviation=%" PRIu32
		       " ns read_errors=%" PRIu64 " resumptions=%" PRIu64
		       " monitor_results=%" PRIu64 " monitor_timeouts=%" PRIu64
		       "\n",
		       dc_status.reference_valid,
		       dc_status.last_reference_result,
		       dc_status.last_difference_ns,
		       dc_status.cycle_adjustment_ns,
		       dc_status.monitor_pending,
		       dc_status.last_maximum_deviation_ns,
		       dc_status.maximum_deviation_ns,
		       (uint64_t)dc_status.reference_read_error_count,
		       (uint64_t)dc_status.reference_resume_count,
		       (uint64_t)dc_status.monitor_success_count,
		       (uint64_t)dc_status.monitor_timeout_count);
	}
	if (ioctl(fd, CW_EC_IOC_GET_IO_STATUS, &io_status) < 0) {
		fprintf(stderr, "cw_ec_config: IO status failed: %s\n",
			strerror(errno));
		goto out;
	}
	printf("IO status: generation=%" PRIu64 " healthy=%u armed=%u"
	       " rearm_required=%u faults=0x%08" PRIx32
	       " latched=0x%08" PRIx32 " fault_count=%" PRIu64
	       " link=%u responding=%" PRIu32
	       " configured=%" PRIu32 " online=%" PRIu32
	       " operational=%" PRIu32 "\n",
	       (uint64_t)io_status.config_generation,
	       io_status.bus_healthy, io_status.outputs_armed,
	       io_status.rearm_required, io_status.current_faults,
	       io_status.last_latched_faults,
	       (uint64_t)io_status.fault_count, io_status.link_up,
	       io_status.slaves_responding,
	       io_status.configured_slave_count,
	       io_status.configured_slaves_online,
	       io_status.configured_slaves_operational);
	if (print_slave_statuses(fd, path, io_status.config_generation))
		goto out;
	if (held_armed) {
		disarm.config_generation = output.config_generation;
		if (ioctl(fd, CW_EC_IOC_DISARM_OUTPUTS, &disarm) < 0) {
			fprintf(stderr,
				"cw_ec_config: held-output disarm failed: %s\n",
				strerror(errno));
			goto out;
		}
		held_armed = false;
		printf("held zero-output shadow synchronously disarmed\n");
	} else {
		output_data = malloc(activate.domain_size);
		output_mask = malloc(activate.domain_size);
		if (!output_data || !output_mask) {
			fprintf(stderr, "cw_ec_config: allocate output image: %s\n",
				strerror(errno));
			goto out;
		}
		memset(output_data, arm_zero ? 0x00 : 0xff,
		       activate.domain_size);
		memset(output_mask, 0xff, activate.domain_size);
		output.data_ptr = (uintptr_t)output_data;
		output.mask_ptr = (uintptr_t)output_mask;
		output.data_size = activate.domain_size;
		output.config_generation = io_status.config_generation;
		if (ioctl(fd, CW_EC_IOC_PUBLISH_OUTPUT, &output) < 0) {
			fprintf(stderr, "cw_ec_config: output publish failed: %s\n",
				strerror(errno));
			goto out;
		}
		printf("published masked output shadow: generation=%" PRIu64
		       " sequence=%" PRIu64 "%s\n",
		       (uint64_t)output.config_generation,
		       (uint64_t)output.output_sequence,
		       arm_zero ? " (zero-arm test pending)" :
				  " (outputs remain disarmed)");
		if (arm_zero) {
			arm.config_generation = output.config_generation;
			arm.output_sequence = output.output_sequence;
			if (ioctl(fd, CW_EC_IOC_ARM_OUTPUTS, &arm) < 0) {
				fprintf(stderr,
					"cw_ec_config: zero-output arm failed: %s\n",
					strerror(errno));
				goto out;
			}
			usleep((useconds_t)(period_ns / 1000U) * 10U);
			io_status.struct_size = sizeof(io_status);
			io_status.api_major = CW_EC_API_VERSION_MAJOR;
			if (ioctl(fd, CW_EC_IOC_GET_IO_STATUS, &io_status) < 0 ||
			    !io_status.outputs_armed) {
				fprintf(stderr,
					"cw_ec_config: zero-output arm was not reported active\n");
				goto out;
			}
			printf("zero-output shadow armed at sequence=%" PRIu64 "\n",
			       (uint64_t)output.output_sequence);
			disarm.config_generation = output.config_generation;
			if (ioctl(fd, CW_EC_IOC_DISARM_OUTPUTS, &disarm) < 0) {
				fprintf(stderr,
					"cw_ec_config: synchronous disarm failed: %s\n",
					strerror(errno));
				goto out;
			}
			io_status.struct_size = sizeof(io_status);
			io_status.api_major = CW_EC_API_VERSION_MAJOR;
			if (ioctl(fd, CW_EC_IOC_GET_IO_STATUS, &io_status) < 0 ||
			    io_status.outputs_armed ||
			    !io_status.rearm_required) {
				fprintf(stderr,
					"cw_ec_config: disarm state was not latched\n");
				goto out;
			}
			printf("zero-output shadow synchronously disarmed; fresh publication required\n");
			errno = 0;
			if (ioctl(fd, CW_EC_IOC_ARM_OUTPUTS, &arm) == 0 ||
			    errno != EAGAIN) {
				fprintf(stderr,
					"cw_ec_config: stale arm was not rejected with EAGAIN\n");
				goto out;
			}
			printf("stale output sequence correctly rejected after disarm\n");
			if (ioctl(fd, CW_EC_IOC_PUBLISH_OUTPUT, &output) < 0) {
				fprintf(stderr,
					"cw_ec_config: fresh zero publication failed: %s\n",
					strerror(errno));
				goto out;
			}
			arm.output_sequence = output.output_sequence;
			if (ioctl(fd, CW_EC_IOC_ARM_OUTPUTS, &arm) < 0) {
				fprintf(stderr,
					"cw_ec_config: fresh zero arm failed: %s\n",
					strerror(errno));
				goto out;
			}
			printf("fresh zero-output sequence=%" PRIu64
			       " accepted after disarm\n",
			       (uint64_t)output.output_sequence);
			if (ioctl(fd, CW_EC_IOC_DISARM_OUTPUTS, &disarm) < 0) {
				fprintf(stderr,
					"cw_ec_config: final synchronous disarm failed: %s\n",
					strerror(errno));
				goto out;
			}
		}
	}
	usleep((useconds_t)(period_ns / 1000U) * 10U);
	snapshot_data = calloc(activate.domain_size, 1);
	if (!snapshot_data) {
		fprintf(stderr, "cw_ec_config: allocate input snapshot: %s\n",
			strerror(errno));
		goto out;
	}
	snapshot.data_ptr = (uintptr_t)snapshot_data;
	snapshot.data_capacity = activate.domain_size;
	if (ioctl(fd, CW_EC_IOC_GET_INPUT_SNAPSHOT, &snapshot) < 0) {
		fprintf(stderr, "cw_ec_config: input snapshot failed: %s\n",
			strerror(errno));
		goto out;
	}
	printf("input snapshot: generation=%" PRIu64
	       " sequence=%" PRIu64 " cycle=%" PRIu64 " size=%" PRIu32
	       " data=",
	       (uint64_t)snapshot.config_generation,
	       (uint64_t)snapshot.input_sequence,
	       (uint64_t)snapshot.cycle_count, snapshot.data_size);
	for (uint32_t i = 0; i < snapshot.data_size && i < 64; i++)
		printf("%02x", snapshot_data[i]);
	if (snapshot.data_size > 64)
		printf("...");
	printf("\n");
	if (ioctl(fd, CW_EC_IOC_CYCLE_DEACTIVATE, &deactivate) < 0) {
		fprintf(stderr, "cw_ec_config: deactivation failed: %s\n",
			strerror(errno));
		goto out;
	}
	active = false;
	ret = 0;
out:
	/*
	 * Close is the kernel-enforced final unwind. It synchronously stops an
	 * active cycle even if a status/deactivation ioctl failed.
	 */
	if (active)
		fprintf(stderr, "cw_ec_config: closing active session for cleanup\n");
	free(output_data);
	free(output_mask);
	free(snapshot_data);
	if (close(fd) < 0) {
		fprintf(stderr, "cw_ec_config: close: %s\n", strerror(errno));
		ret = 1;
	}
	return ret;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/cw_ethercat0";
	struct counts counts;
	uint64_t duration;
	uint64_t period;

	if (argc < 3 || argc > 6) {
		usage(argv[0]);
		return 2;
	}
	if (scan_config(argv[2], &counts))
		return 1;
	if (!strcmp(argv[1], "check") && argc == 3) {
		printf("valid syntax: %u slave(s), %u sync manager(s), %u PDO(s), "
		       "%u entry/entries, %u DC record(s)\n",
		       counts.slaves, counts.syncs, counts.pdos, counts.entries,
		       counts.dcs);
		return 0;
	}
	if (!strcmp(argv[1], "prepare")) {
		if (argc > 4) {
			usage(argv[0]);
			return 2;
		}
		if (argc == 4)
			device = argv[3];
		return prepare(argv[2], device);
	}
	if ((!strcmp(argv[1], "cycle") ||
	     !strcmp(argv[1], "cycle-zero-arm") ||
	     !strcmp(argv[1], "cycle-zero-hold")) &&
	    (argc == 5 || argc == 6)) {
		if (parse_u64(argv[3], CW_EC_CYCLE_PERIOD_MAX_NS, &period) ||
		    period < CW_EC_CYCLE_PERIOD_MIN_NS ||
		    parse_u64(argv[4], 3600, &duration) || !duration) {
			fprintf(stderr, "cw_ec_config: invalid period or duration\n");
			return 2;
		}
		if (argc == 6)
			device = argv[5];
		return cycle(argv[2], period, duration,
			     !strcmp(argv[1], "cycle-zero-arm"),
			     !strcmp(argv[1], "cycle-zero-hold"), device);
	}
	usage(argv[0]);
	return 2;
}
