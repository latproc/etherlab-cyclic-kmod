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
		"  %s cycle CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]\n",
		program, program, program);
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
		 unsigned int duration_seconds, const char *device)
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
	struct cw_ec_cycle_deactivate deactivate = {
		.struct_size = sizeof(deactivate),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	int fd = open(device, O_RDWR | O_CLOEXEC);
	bool active = false;
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
	if (!strcmp(argv[1], "cycle") && (argc == 5 || argc == 6)) {
		if (parse_u64(argv[3], CW_EC_CYCLE_PERIOD_MAX_NS, &period) ||
		    period < CW_EC_CYCLE_PERIOD_MIN_NS ||
		    parse_u64(argv[4], 3600, &duration) || !duration) {
			fprintf(stderr, "cw_ec_config: invalid period or duration\n");
			return 2;
		}
		if (argc == 6)
			device = argv[5];
		return cycle(argv[2], period, duration, device);
	}
	usage(argv[0]);
	return 2;
}
