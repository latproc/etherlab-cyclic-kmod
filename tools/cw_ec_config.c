// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "cw_ethercat.h"

/* Map libcwethercat -errno returns to ioctl-style (-1 + errno). */
static int lib_ret(int ret)
{
	if (ret == 0)
		return 0;
	errno = -ret;
	return -1;
}

static int open_handle(const char *device, cw_ec_handle **out)
{
	int ret = cw_ec_open(device, out);

	if (ret) {
		errno = -ret;
		return -1;
	}
	return 0;
}


#define LINE_MAX_BYTES 1024U
#define TOKEN_MAX 10U
#define LATENCY_BUCKET_WIDTH_NS 1000LL
#define LATENCY_BUCKET_LIMIT_NS 1000000LL
#define LATENCY_BUCKET_COUNT 2003U

struct latency_stats {
	uint64_t count;
	int64_t total_ns;
	int64_t minimum_ns;
	int64_t maximum_ns;
	uint64_t buckets[LATENCY_BUCKET_COUNT];
};

static void latency_add(struct latency_stats *stats, int64_t value_ns)
{
	unsigned int bucket;

	if (!stats->count) {
		stats->minimum_ns = value_ns;
		stats->maximum_ns = value_ns;
	} else {
		if (value_ns < stats->minimum_ns)
			stats->minimum_ns = value_ns;
		if (value_ns > stats->maximum_ns)
			stats->maximum_ns = value_ns;
	}
	stats->count++;
	stats->total_ns += value_ns;
	if (value_ns < -LATENCY_BUCKET_LIMIT_NS)
		bucket = 0;
	else if (value_ns >= LATENCY_BUCKET_LIMIT_NS)
		bucket = LATENCY_BUCKET_COUNT - 1U;
	else
		bucket = (unsigned int)
			((value_ns + LATENCY_BUCKET_LIMIT_NS) /
			 LATENCY_BUCKET_WIDTH_NS) +
			 1U;
	stats->buckets[bucket]++;
}

static int64_t latency_percentile(const struct latency_stats *stats,
				  uint64_t numerator,
				  uint64_t denominator)
{
	uint64_t target;
	uint64_t seen = 0;
	unsigned int bucket;

	if (!stats->count)
		return 0;
	target = (stats->count * numerator + denominator - 1U) /
		 denominator;
	for (bucket = 0; bucket < LATENCY_BUCKET_COUNT; bucket++) {
		seen += stats->buckets[bucket];
		if (seen < target)
			continue;
		if (!bucket)
			return -LATENCY_BUCKET_LIMIT_NS;
		if (bucket == LATENCY_BUCKET_COUNT - 1U)
			return LATENCY_BUCKET_LIMIT_NS;
		return -LATENCY_BUCKET_LIMIT_NS +
		       (int64_t)(bucket - 1U) * LATENCY_BUCKET_WIDTH_NS +
		       LATENCY_BUCKET_WIDTH_NS / 2;
	}
	return stats->maximum_ns;
}

static void print_latency_stats(const char *name,
				const struct latency_stats *stats)
{
	printf("%s latency: samples=%" PRIu64
	       " mean=%.1f ns median=%" PRId64
	       " ns p99=%" PRId64 " ns p99.9=%" PRId64
	       " ns min=%" PRId64 " ns max=%" PRId64 " ns\n",
	       name, stats->count,
	       stats->count ?
		       (double)stats->total_ns / (double)stats->count :
		       0.0,
	       latency_percentile(stats, 500, 1000),
	       latency_percentile(stats, 990, 1000),
	       latency_percentile(stats, 999, 1000),
	       stats->minimum_ns, stats->maximum_ns);
}

enum record_kind {
	RECORD_EMPTY,
	RECORD_SLAVE,
	RECORD_SYNC,
	RECORD_PDO,
	RECORD_ENTRY,
	RECORD_DC,
	RECORD_DC_POLICY,
	RECORD_DOMAIN,
	RECORD_DOMAIN_ASSIGNMENT,
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
		struct cw_ec_config_domain domain;
		struct cw_ec_config_domain_assignment domain_assignment;
	};
};

struct counts {
	uint32_t slaves;
	uint32_t syncs;
	uint32_t pdos;
	uint32_t entries;
	uint32_t dcs;
	uint32_t domains;
	uint32_t domain_assignments;
};

struct io_entry {
	struct cw_ec_config_entry cfg;
	struct cw_ec_entry_offset offset;
	uint8_t direction;
};

struct io_metadata {
	struct cw_ec_config_sync *syncs;
	struct cw_ec_config_pdo *pdos;
	struct io_entry *entries;
	uint32_t sync_count;
	uint32_t pdo_count;
	uint32_t entry_count;
};

static bool suppress_offset_output;

static void usage(const char *program)
{
	fprintf(stderr,
		"usage:\n"
		"  %s check CONFIG\n"
		"  %s prepare CONFIG [DEVICE]\n"
		"  %s io CONFIG PERIOD_NS [DEVICE]\n"
		"  %s cycle CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]\n"
		"  %s cycle-strict CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]\n"
		"  %s cycle-rate CONFIG START_PERIOD_NS TARGET_PERIOD_NS DURATION_SECONDS [DEVICE]\n"
		"  %s cycle-exchange-rate CONFIG START_PERIOD_NS TARGET_PERIOD_NS DURATION_SECONDS [DEVICE]\n"
		"  %s cycle-history CONFIG PERIOD_NS DEPTH DURATION_SECONDS [DEVICE]\n"
		"  %s cycle-zero-arm CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]\n"
		"  %s cycle-zero-lease CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]\n"
		"  %s cycle-zero-hold CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]\n"
		"  %s cycle-monitor CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]\n"
		"  %s cycle-abi CONFIG PERIOD_NS DURATION_SECONDS [DEVICE]\n"
		"  %s pulse-entry CONFIG PERIOD_NS ENTRY_ID PULSE_MS [DEVICE]\n",
		program, program, program, program, program, program, program,
		program, program, program, program, program, program, program);
}

static int expect_ioctl_errno(cw_ec_handle *h, unsigned long request,
			      void *argument, int expected, const char *name)
{
	int fd = cw_ec_fd(h);

	if (fd < 0) {
		fprintf(stderr, "cw_ec_config: active %s: bad handle\n", name);
		return 1;
	}
	errno = 0;
	if (ioctl(fd, request, argument) < 0 && errno == expected) {
		printf("PASS: active %s returned %s\n", name,
		       strerror(expected));
		return 0;
	}
	fprintf(stderr, "cw_ec_config: active %s expected %s, got %s\n",
		name, strerror(expected), errno ? strerror(errno) : "success");
	return 1;
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

	if (!strcmp(tokens[0], "domain") && count == 2) {
		record->kind = RECORD_DOMAIN;
		record->domain.struct_size = sizeof(record->domain);
		record->domain.api_major = CW_EC_API_VERSION_MAJOR;
		if (parse_u64(tokens[1], UINT32_MAX, &value))
			return -1;
		record->domain.config_id = value;
		return 1;
	}

	if (!strcmp(tokens[0], "domain_slave") && count == 4) {
		record->kind = RECORD_DOMAIN_ASSIGNMENT;
		record->domain_assignment.struct_size =
			sizeof(record->domain_assignment);
		record->domain_assignment.api_major =
			CW_EC_API_VERSION_MAJOR;
		if (parse_u64(tokens[1], UINT32_MAX, &value))
			return -1;
		record->domain_assignment.config_id = value;
		if (parse_u64(tokens[2], UINT32_MAX, &value))
			return -1;
		record->domain_assignment.slave_config_id = value;
		if (parse_u64(tokens[3], UINT32_MAX, &value))
			return -1;
		record->domain_assignment.domain_config_id = value;
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
		case RECORD_DOMAIN:
			counts->domains++;
			break;
		case RECORD_DOMAIN_ASSIGNMENT:
			counts->domain_assignments++;
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
	    counts->dcs > CW_EC_CONFIG_DC_MAX ||
	    counts->domains > CW_EC_CONFIG_DOMAIN_MAX ||
	    counts->domain_assignments > CW_EC_CONFIG_SLAVE_MAX) {
		fprintf(stderr, "cw_ec_config: invalid or excessive object counts\n");
		return -1;
	}
	return 0;
}

static void free_io_metadata(struct io_metadata *metadata)
{
	free(metadata->syncs);
	free(metadata->pdos);
	free(metadata->entries);
	memset(metadata, 0, sizeof(*metadata));
}

static int load_io_metadata(const char *path, const struct counts *counts,
			    struct io_metadata *metadata)
{
	char line[LINE_MAX_BYTES];
	struct record record;
	FILE *file;
	uint32_t i;

	memset(metadata, 0, sizeof(*metadata));
	metadata->syncs = calloc(counts->syncs, sizeof(*metadata->syncs));
	metadata->pdos = calloc(counts->pdos, sizeof(*metadata->pdos));
	metadata->entries = calloc(counts->entries,
				   sizeof(*metadata->entries));
	if (!metadata->syncs || !metadata->pdos || !metadata->entries)
		goto fail;
	file = fopen(path, "r");
	if (!file)
		goto fail;
	while (fgets(line, sizeof(line), file)) {
		if (parse_record(line, &record) <= 0)
			continue;
		switch (record.kind) {
		case RECORD_SYNC:
			metadata->syncs[metadata->sync_count++] =
				record.sync;
			break;
		case RECORD_PDO:
			metadata->pdos[metadata->pdo_count++] = record.pdo;
			break;
		case RECORD_ENTRY:
			if (!record.entry.entry_id)
				break;
			metadata->entries[metadata->entry_count++].cfg =
				record.entry;
			break;
		default:
			break;
		}
	}
	if (ferror(file)) {
		fclose(file);
		goto fail;
	}
	fclose(file);
	for (i = 0; i < metadata->entry_count; i++) {
		struct cw_ec_config_pdo *pdo = NULL;
		struct cw_ec_config_sync *sync = NULL;
		uint32_t j;

		for (j = 0; j < metadata->pdo_count; j++) {
			if (metadata->pdos[j].config_id ==
			    metadata->entries[i].cfg.pdo_config_id) {
				pdo = &metadata->pdos[j];
				break;
			}
		}
		if (!pdo)
			goto fail;
		for (j = 0; j < metadata->sync_count; j++) {
			if (metadata->syncs[j].config_id ==
			    pdo->sync_config_id) {
				sync = &metadata->syncs[j];
				break;
			}
		}
		if (!sync)
			goto fail;
		metadata->entries[i].direction = sync->direction;
	}
	return 0;

fail:
	free_io_metadata(metadata);
	return -1;
}

static struct io_entry *find_io_entry(struct io_metadata *metadata,
				      uint32_t entry_id)
{
	uint32_t i;

	for (i = 0; i < metadata->entry_count; i++)
		if (metadata->entries[i].cfg.entry_id == entry_id)
			return &metadata->entries[i];
	return NULL;
}

static uint64_t image_get_value(const uint8_t *image,
				const struct cw_ec_entry_offset *offset)
{
	uint64_t value = 0;
	uint32_t first_bit = offset->global_offset * 8U +
			     offset->bit_position;
	uint32_t bit;

	for (bit = 0; bit < offset->bit_length; bit++)
		if (image[(first_bit + bit) / 8U] &
		    (1U << ((first_bit + bit) % 8U)))
			value |= 1ULL << bit;
	return value;
}

static void image_set_value(uint8_t *image, uint8_t *mask,
			    const struct cw_ec_entry_offset *offset,
			    uint64_t value)
{
	uint32_t first_bit = offset->global_offset * 8U +
			     offset->bit_position;
	uint32_t bit;

	for (bit = 0; bit < offset->bit_length; bit++) {
		uint8_t bit_mask = 1U << ((first_bit + bit) % 8U);
		uint32_t byte = (first_bit + bit) / 8U;

		mask[byte] |= bit_mask;
		if (value & (1ULL << bit))
			image[byte] |= bit_mask;
		else
			image[byte] &= ~bit_mask;
	}
}

static int submit_record(cw_ec_handle *h, const struct record *record)
{
	switch (record->kind) {
	case RECORD_SLAVE:
		return lib_ret(cw_ec_config_add_slave(h, &record->slave));
	case RECORD_SYNC:
		return lib_ret(cw_ec_config_add_sync(h, &record->sync));
	case RECORD_PDO:
		return lib_ret(cw_ec_config_add_pdo(h, &record->pdo));
	case RECORD_ENTRY:
		return lib_ret(cw_ec_config_add_entry(h, &record->entry));
	case RECORD_DC:
		return lib_ret(cw_ec_config_add_dc(h, &record->dc));
	case RECORD_DC_POLICY:
		return lib_ret(cw_ec_config_add_dc_policy(h, &record->dc_policy));
	case RECORD_DOMAIN:
		return lib_ret(cw_ec_config_add_domain(h, &record->domain));
	case RECORD_DOMAIN_ASSIGNMENT:
		return lib_ret(cw_ec_config_add_domain_assignment(
			h, &record->domain_assignment));
	default:
		return 0;
	}
}

static int submit_config(cw_ec_handle *h, const char *path)
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
		if (parsed > 0 && submit_record(h, &record) < 0) {
			fprintf(stderr, "%s:%u: kernel rejected record: %s\n",
				path, line_number, strerror(errno));
			fclose(file);
			return -1;
		}
	}
	fclose(file);
	return 0;
}

static int print_offsets(cw_ec_handle *h, const char *path)
{
	char line[LINE_MAX_BYTES];
	struct record record;
	FILE *file = fopen(path, "r");

	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		struct cw_ec_entry_offset offset;

		if (parse_record(line, &record) <= 0 ||
		    record.kind != RECORD_ENTRY || !record.entry.entry_id)
			continue;
		memset(&offset, 0, sizeof(offset));
		offset.struct_size = sizeof(offset);
		offset.api_major = CW_EC_API_VERSION_MAJOR;
		offset.entry_id = record.entry.entry_id;
		if (lib_ret(cw_ec_get_entry_offset(h, &offset)) < 0) {
			fprintf(stderr, "entry %" PRIu32 ": offset lookup: %s\n",
				offset.entry_id, strerror(errno));
			fclose(file);
			return -1;
		}
		printf("entry %" PRIu32 " object 0x%04" PRIx16 ":%02" PRIx8
		       " offset=%" PRIu32 " bit=%" PRIu8 " length=%" PRIu8
		       "\n",
		       record.entry.entry_id, record.entry.index,
		       record.entry.subindex, offset.global_offset,
		       offset.bit_position, offset.bit_length);
	}
	fclose(file);
	return 0;
}

static int configure_handle(cw_ec_handle *h, const char *path,
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
	if (lib_ret(cw_ec_config_begin(h)) < 0 ||
	    submit_config(h, path) < 0)
		return -1;
	if (lib_ret(cw_ec_config_validate(h, &validate)) < 0) {
		fprintf(stderr, "cw_ec_config: validation failed: %s\n",
			strerror(errno));
		return -1;
	}
	if (lib_ret(cw_ec_config_apply(h, &apply)) < 0) {
		fprintf(stderr,
			"cw_ec_config: apply failed: %s; kind=%u id=%" PRIu32
			"\n",
			strerror(errno), apply.failed_object_kind,
			apply.failed_config_id);
		return -1;
	}
	if (lib_ret(cw_ec_domain_create(h, &domain)) < 0) {
		fprintf(stderr,
			"cw_ec_config: domain registration failed: %s; id=%"
			PRIu32 "\n",
			strerror(errno), domain.failed_config_id);
		return -1;
	}
	if (!suppress_offset_output && print_offsets(h, path))
		return -1;
	*validated = validate;
	return 0;
}

static int entry_is_single_bit_output(const char *path, uint32_t entry_id)
{
	FILE *stream;
	char *line = NULL;
	size_t capacity = 0;
	uint32_t pdo_id = 0;
	uint32_t sync_id = 0;
	int result = 0;

	stream = fopen(path, "r");
	if (!stream)
		return -1;
	while (getline(&line, &capacity, stream) >= 0) {
		struct record record;

		if (parse_record(line, &record) < 0)
			goto out;
		if (record.kind == RECORD_ENTRY &&
		    record.entry.entry_id == entry_id) {
			if (record.entry.bit_length != 1 || pdo_id)
				goto out;
			pdo_id = record.entry.pdo_config_id;
		}
	}
	if (!pdo_id || ferror(stream))
		goto out;
	rewind(stream);
	while (getline(&line, &capacity, stream) >= 0) {
		struct record record;

		if (parse_record(line, &record) < 0)
			goto out;
		if (record.kind == RECORD_PDO &&
		    record.pdo.config_id == pdo_id) {
			if (sync_id)
				goto out;
			sync_id = record.pdo.sync_config_id;
		}
	}
	if (!sync_id || ferror(stream))
		goto out;
	rewind(stream);
	while (getline(&line, &capacity, stream) >= 0) {
		struct record record;

		if (parse_record(line, &record) < 0)
			goto out;
		if (record.kind == RECORD_SYNC &&
		    record.sync.config_id == sync_id) {
			if (result ||
			    record.sync.direction != CW_EC_DIR_OUTPUT)
				goto out;
			result = 1;
		}
	}
	if (ferror(stream))
		result = 0;
out:
	free(line);
	fclose(stream);
	return result;
}

static int pulse_entry(const char *path, uint32_t period_ns,
		       uint32_t entry_id, uint32_t pulse_ms,
		       const char *device)
{
	struct cw_ec_config_validate validate;
	struct cw_ec_cycle_activate activate = {
		.struct_size = sizeof(activate),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.cycle_period_ns = period_ns,
	};
	struct cw_ec_entry_offset offset = {
		.struct_size = sizeof(offset),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.entry_id = entry_id,
	};
	struct cw_ec_io_status io_status = {
		.struct_size = sizeof(io_status),
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
	struct cw_ec_cycle_deactivate deactivate = {
		.struct_size = sizeof(deactivate),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	uint8_t *data = NULL;
	uint8_t *mask = NULL;
	bool active = false;
	bool armed = false;
	unsigned int attempts;
	cw_ec_handle *h = NULL;
	int ret = 1;

	if (!getenv("CW_EC_NONZERO_OUTPUT_AUTHORIZED") ||
	    strcmp(getenv("CW_EC_NONZERO_OUTPUT_AUTHORIZED"), "YES")) {
		fprintf(stderr,
			"cw_ec_config: set CW_EC_NONZERO_OUTPUT_AUTHORIZED=YES only for an approved, physically safe output\n");
		return 2;
	}
	if (entry_is_single_bit_output(path, entry_id) != 1) {
		fprintf(stderr,
			"cw_ec_config: entry %" PRIu32
			" is not one unique single-bit output in the configuration\n",
			entry_id);
		return 2;
	}
	if (open_handle(device, &h) < 0) {
		fprintf(stderr, "cw_ec_config: cannot open %s: %s\n",
			device, strerror(errno));
		return 1;
	}
	if (configure_handle(h, path, &validate))
		goto out;
	if (lib_ret(cw_ec_get_entry_offset(h, &offset)) < 0) {
		fprintf(stderr, "cw_ec_config: entry %" PRIu32
			" offset lookup failed: %s\n", entry_id,
			strerror(errno));
		goto out;
	}
	if (offset.bit_length != 1) {
		fprintf(stderr, "cw_ec_config: pulse entry must be exactly one bit\n");
		goto out;
	}
	if (offset.bit_position >= 8) {
		fprintf(stderr, "cw_ec_config: invalid pulse bit position\n");
		goto out;
	}
	if (lib_ret(cw_ec_cycle_activate(h, activate.cycle_period_ns, activate.flags, &activate)) < 0) {
		fprintf(stderr, "cw_ec_config: activation failed: %s\n",
			strerror(errno));
		goto out;
	}
	active = true;
	for (attempts = 0; attempts < 100; attempts++) {
		io_status.struct_size = sizeof(io_status);
		io_status.api_major = CW_EC_API_VERSION_MAJOR;
		if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0) {
			fprintf(stderr, "cw_ec_config: IO status failed: %s\n",
				strerror(errno));
			goto out;
		}
		if (io_status.bus_healthy)
			break;
		usleep(50000);
	}
	if (!io_status.bus_healthy) {
		fprintf(stderr, "cw_ec_config: bus did not become healthy\n");
		goto out;
	}
	printf("active topology: responding=%" PRIu32
	       " configured=%" PRIu32 " online=%" PRIu32
	       " operational=%" PRIu32 "\n",
	       io_status.slaves_responding, io_status.configured_slave_count,
	       io_status.configured_slaves_online,
	       io_status.configured_slaves_operational);
	data = calloc(activate.domain_size, 1);
	mask = calloc(activate.domain_size, 1);
	if (!data || !mask) {
		fprintf(stderr, "cw_ec_config: allocate pulse image: %s\n",
			strerror(errno));
		goto out;
	}
	if (offset.global_offset >= activate.domain_size) {
		fprintf(stderr, "cw_ec_config: entry offset is outside image\n");
		goto out;
	}
	data[offset.global_offset] = (uint8_t)(1U << offset.bit_position);
	mask[offset.global_offset] = data[offset.global_offset];
	output.data_ptr = (uintptr_t)data;
	output.mask_ptr = (uintptr_t)mask;
	output.data_size = activate.domain_size;
	output.config_generation = io_status.config_generation;
	if (lib_ret(cw_ec_publish_output(h, (const void *)(uintptr_t)output.data_ptr, (const void *)(uintptr_t)output.mask_ptr, output.data_size, &output)) < 0) {
		fprintf(stderr, "cw_ec_config: pulse publication failed: %s\n",
			strerror(errno));
		goto out;
	}
	arm.config_generation = output.config_generation;
	arm.output_sequence = output.output_sequence;
	if (lib_ret(cw_ec_arm_output(h, &arm)) < 0) {
		fprintf(stderr, "cw_ec_config: pulse arm failed: %s\n",
			strerror(errno));
		goto out;
	}
	armed = true;
	io_status.struct_size = sizeof(io_status);
	io_status.api_major = CW_EC_API_VERSION_MAJOR;
	if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0 ||
	    !io_status.outputs_armed) {
		fprintf(stderr,
			"cw_ec_config: pulse was not reported armed\n");
		goto out;
	}
	printf("PULSE: entry=%" PRIu32 " offset=%" PRIu32
	       " bit=%u duration=%" PRIu32 " ms\n",
	       entry_id, offset.global_offset, offset.bit_position, pulse_ms);
	fflush(stdout);
	usleep((useconds_t)pulse_ms * 1000U);
	disarm.config_generation = output.config_generation;
	if (lib_ret(cw_ec_disarm_output(h, &disarm)) < 0) {
		fprintf(stderr, "cw_ec_config: pulse disarm failed: %s\n",
			strerror(errno));
		goto out;
	}
	armed = false;
	io_status.struct_size = sizeof(io_status);
	io_status.api_major = CW_EC_API_VERSION_MAJOR;
	if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0 ||
	    io_status.outputs_armed || !io_status.rearm_required) {
		fprintf(stderr,
			"cw_ec_config: pulse disarm state was not latched\n");
		goto out;
	}
	printf("PULSE: synchronously disarmed; output returned to zero\n");
	if (lib_ret(cw_ec_cycle_deactivate(h, &deactivate)) < 0) {
		fprintf(stderr, "cw_ec_config: deactivation failed: %s\n",
			strerror(errno));
		goto out;
	}
	active = false;
	ret = 0;
out:
	if (armed) {
		disarm.config_generation = output.config_generation;
		if (lib_ret(cw_ec_disarm_output(h, &disarm)) < 0)
			fprintf(stderr,
				"cw_ec_config: cleanup disarm failed: %s\n",
				strerror(errno));
	}
	if (active)
		fprintf(stderr, "cw_ec_config: closing active pulse session for cleanup\n");
	free(mask);
	free(data);
	cw_ec_close(h);
	return ret;
}

static int print_slave_statuses(cw_ec_handle *h, const char *path,
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
		if (lib_ret(cw_ec_get_config_slave_status(h, &status)) < 0) {
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

static int print_domain_statuses(cw_ec_handle *h, const char *path,
				 uint64_t generation)
{
	FILE *stream;
	char *line = NULL;
	size_t capacity = 0;
	int ret = -1;

	stream = fopen(path, "r");
	if (!stream)
		return -1;
	while (getline(&line, &capacity, stream) >= 0) {
		struct cw_ec_domain_status status = {
			.struct_size = sizeof(status),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.config_generation = generation,
		};
		struct record record;

		if (parse_record(line, &record) < 0)
			goto out;
		if (record.kind != RECORD_DOMAIN)
			continue;
		status.domain_config_id = record.domain.config_id;
		if (lib_ret(cw_ec_get_domain_status(h, &status)) < 0) {
			fprintf(stderr,
				"cw_ec_config: domain status %" PRIu32
				" failed: %s\n",
				status.domain_config_id, strerror(errno));
			goto out;
		}
		printf("domain status: id=%" PRIu32 " base=%" PRIu32
		       " size=%" PRIu32 " wc=%" PRIu32
		       " wc_state=%u valid=%u faults=0x%08" PRIx32 "\n",
		       status.domain_config_id, status.base_offset,
		       status.domain_size, status.working_counter,
		       status.working_counter_state, status.data_valid,
		       status.current_faults);
	}
	ret = ferror(stream) ? -1 : 0;
out:
	free(line);
	fclose(stream);
	return ret;
}

static int first_domain_id(const char *path, uint32_t *domain_id)
{
	FILE *stream;
	char *line = NULL;
	size_t capacity = 0;
	int ret = 0;

	*domain_id = UINT32_MAX;
	stream = fopen(path, "r");
	if (!stream)
		return -1;
	while (getline(&line, &capacity, stream) >= 0) {
		struct record record;

		if (parse_record(line, &record) < 0) {
			ret = -1;
			break;
		}
		if (record.kind == RECORD_DOMAIN) {
			*domain_id = record.domain.config_id;
			break;
		}
	}
	if (ferror(stream))
		ret = -1;
	free(line);
	fclose(stream);
	return ret;
}

static int prepare(const char *path, const char *device)
{
	struct cw_ec_config_validate validate;
	cw_ec_handle *h = NULL;
	int ret = 1;

	if (open_handle(device, &h) < 0) {
		fprintf(stderr, "cw_ec_config: cannot open %s: %s\n",
			device, strerror(errno));
		return 1;
	}
	if (configure_handle(h, path, &validate))
		goto out;
	printf("prepared %u slave(s), %u sync manager(s), %u PDO(s), "
	       "%u entry/entries; master was not activated\n",
	       validate.slave_count, validate.sync_count, validate.pdo_count,
	       validate.entry_count);
	ret = 0;
out:
	cw_ec_close(h);
	return ret;
}

static int cycle(const char *path, uint32_t period_ns,
		 unsigned int duration_seconds, bool arm_zero,
		 bool lease_zero, bool hold_zero, bool monitor, bool active_abi,
		 bool require_healthy, uint32_t target_period_ns,
		 bool exchange_each_wake, uint32_t history_depth,
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
	struct cw_ec_cycle_info cycle_info = {
		.struct_size = sizeof(cycle_info),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_cycle_wait cycle_wait = {
		.struct_size = sizeof(cycle_wait),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.timeout_ms = 1000,
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
	struct cw_ec_output_lease_config lease_config = {
		.struct_size = sizeof(lease_config),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.cycle_budget = 100,
	};
	struct cw_ec_output_lease_renew lease_renew = {
		.struct_size = sizeof(lease_renew),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_output_lease_status lease_status = {
		.struct_size = sizeof(lease_status),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_input_history_config history_config = {
		.struct_size = sizeof(history_config),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	uint8_t *snapshot_data = NULL;
	uint8_t *output_data = NULL;
	uint8_t *output_mask = NULL;
	struct cw_ec_cycle_deactivate deactivate = {
		.struct_size = sizeof(deactivate),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	cw_ec_handle *h = NULL;
	bool active = false;
	bool held_armed = false;
	int ret = 1;

	if (open_handle(device, &h) < 0) {
		fprintf(stderr, "cw_ec_config: cannot open %s: %s\n",
			device, strerror(errno));
		return 1;
	}
	if (configure_handle(h, path, &validate))
		goto out;
	if (history_depth) {
		io_status.struct_size = sizeof(io_status);
		io_status.api_major = CW_EC_API_VERSION_MAJOR;
		if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0) {
			fprintf(stderr,
				"cw_ec_config: pre-activation history status failed: %s\n",
				strerror(errno));
			goto out;
		}
		history_config.config_generation =
			io_status.config_generation;
		history_config.depth = history_depth;
		if (lib_ret(cw_ec_configure_input_history(h, &history_config)) < 0) {
			fprintf(stderr,
				"cw_ec_config: input history configuration failed: %s\n",
				strerror(errno));
			goto out;
		}
	}
	if (lease_zero) {
		io_status.struct_size = sizeof(io_status);
		io_status.api_major = CW_EC_API_VERSION_MAJOR;
		if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0) {
			fprintf(stderr,
				"cw_ec_config: pre-activation IO status failed: %s\n",
				strerror(errno));
			goto out;
		}
		lease_config.config_generation =
			io_status.config_generation;
		if (lib_ret(cw_ec_configure_output_lease(h, &lease_config)) < 0) {
			fprintf(stderr,
				"cw_ec_config: output lease configuration failed: %s\n",
				strerror(errno));
			goto out;
		}
	}
	if (lib_ret(cw_ec_cycle_activate(h, activate.cycle_period_ns, activate.flags, &activate)) < 0) {
		fprintf(stderr, "cw_ec_config: activation failed: %s\n",
			strerror(errno));
		goto out;
	}
	active = true;
	printf("activated zero-output domain: size=%" PRIu32
	       " period=%" PRIu32 " ns for %u second(s)\n",
	       activate.domain_size, period_ns, duration_seconds);
	if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0) {
		fprintf(stderr, "cw_ec_config: initial IO status failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (lib_ret(cw_ec_cycle_info(h, &cycle_info)) < 0) {
		fprintf(stderr, "cw_ec_config: initial cycle info failed: %s\n",
			strerror(errno));
		goto out;
	}
	cycle_wait.config_generation = io_status.config_generation;
	cycle_wait.after_cycle_index = cycle_info.cycle_index;
	if (lib_ret(cw_ec_cycle_wait(h, &cycle_wait)) < 0) {
		fprintf(stderr, "cw_ec_config: wait for cycle failed: %s\n",
			strerror(errno));
		goto out;
	}
	if (cycle_wait.cycle.cycle_index == cycle_wait.after_cycle_index ||
	    cycle_wait.cycle.cycle_period_ns != period_ns ||
	    (cycle_wait.cycle.actual_wake_time_ns >=
		     cycle_wait.cycle.scheduled_time_ns ?
	     (int64_t)(cycle_wait.cycle.actual_wake_time_ns -
		       cycle_wait.cycle.scheduled_time_ns) :
	     -(int64_t)(cycle_wait.cycle.scheduled_time_ns -
			cycle_wait.cycle.actual_wake_time_ns)) !=
		    cycle_wait.cycle.wake_lateness_ns) {
		fprintf(stderr,
			"cw_ec_config: incoherent cycle timing result\n");
		goto out;
	}
	printf("cycle timing: cycle=%" PRIu64 " scheduled=%" PRIu64
	       " actual=%" PRIu64 " lateness=%" PRId64
	       " ns input=%" PRIu64 " output_consumed=%" PRIu64
	       " stale=%" PRIu64 " missed=%" PRIu64 "\n",
	       (uint64_t)cycle_wait.cycle.cycle_index,
	       (uint64_t)cycle_wait.cycle.scheduled_time_ns,
	       (uint64_t)cycle_wait.cycle.actual_wake_time_ns,
	       (int64_t)cycle_wait.cycle.wake_lateness_ns,
	       (uint64_t)cycle_wait.cycle.input_sequence,
	       (uint64_t)cycle_wait.cycle.output_sequence_consumed,
	       (uint64_t)cycle_wait.cycle.stale_output_cycles,
	       (uint64_t)cycle_wait.cycle.missed_deadlines);
	if (hold_zero || arm_zero || lease_zero || monitor || active_abi ||
	    require_healthy || target_period_ns || history_depth) {
		unsigned int attempts;

		for (attempts = 0; attempts < 100; attempts++) {
			io_status.struct_size = sizeof(io_status);
			io_status.api_major = CW_EC_API_VERSION_MAJOR;
			if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0) {
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
				"cw_ec_config: bus did not become healthy within five seconds\n");
			if (print_slave_statuses(h, path,
						 io_status.config_generation))
				fprintf(stderr,
					"cw_ec_config: failed to report slave status\n");
			goto out;
		}
	}
	if (target_period_ns) {
		struct cw_ec_cycle_period_update update = {
			.struct_size = sizeof(update),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.config_generation = io_status.config_generation,
			.cycle_period_ns = target_period_ns,
		};

		if (lib_ret(cw_ec_cycle_set_period(h, &update)) < 0) {
			fprintf(stderr,
				"cw_ec_config: cycle period update failed: %s\n",
				strerror(errno));
			goto out;
		}
		if (update.applied_period_ns != target_period_ns) {
			fprintf(stderr,
				"cw_ec_config: kernel acknowledged unexpected period %" PRIu32 " ns\n",
				update.applied_period_ns);
			goto out;
		}
		period_ns = target_period_ns;
		printf("cycle period changed at boundary after cycle=%" PRIu64
		       " to %" PRIu32 " ns\n",
		       (uint64_t)update.effective_after_cycle,
		       update.applied_period_ns);
	}
	if (require_healthy) {
		printf("READY: strict-health timing interval started\n");
		fflush(stdout);
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
		if (lib_ret(cw_ec_publish_output(h, (const void *)(uintptr_t)output.data_ptr, (const void *)(uintptr_t)output.mask_ptr, output.data_size, &output)) < 0) {
			fprintf(stderr,
				"cw_ec_config: zero hold publication failed: %s\n",
				strerror(errno));
			goto out;
		}
		arm.config_generation = output.config_generation;
		arm.output_sequence = output.output_sequence;
		if (lib_ret(cw_ec_arm_output(h, &arm)) < 0) {
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
	if (active_abi) {
		uint8_t byte = 0;
		uint32_t domain_id;
		struct cw_ec_input_snapshot invalid_snapshot = {
			.struct_size = sizeof(invalid_snapshot),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.data_ptr = (uintptr_t)&byte,
			.data_capacity = activate.domain_size - 1U,
		};
		struct cw_ec_output_publish invalid_output = {
			.struct_size = sizeof(invalid_output),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.data_ptr = (uintptr_t)&byte,
			.mask_ptr = (uintptr_t)&byte,
			.data_size = activate.domain_size,
			.config_generation = io_status.config_generation - 1U,
		};
		struct cw_ec_output_arm invalid_arm = {
			.struct_size = sizeof(invalid_arm),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.config_generation = io_status.config_generation - 1U,
			.output_sequence = 1,
		};
		struct cw_ec_output_disarm invalid_disarm = {
			.struct_size = sizeof(invalid_disarm),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.config_generation = io_status.config_generation - 1U,
		};
		struct cw_ec_cycle_activate duplicate_activate = activate;
		struct cw_ec_domain_status invalid_domain = {
			.struct_size = sizeof(invalid_domain),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.config_generation = io_status.config_generation - 1U,
		};
		struct cw_ec_cycle_info invalid_cycle_info = {
			.struct_size = sizeof(invalid_cycle_info),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.reserved1 = 1,
		};
		struct cw_ec_cycle_wait invalid_cycle_wait = {
			.struct_size = sizeof(invalid_cycle_wait),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.config_generation =
				io_status.config_generation - 1U,
			.timeout_ms = 100,
		};
		struct cw_ec_cycle_period_update invalid_period_update = {
			.struct_size = sizeof(invalid_period_update),
			.api_major = CW_EC_API_VERSION_MAJOR,
			.config_generation =
				io_status.config_generation - 1U,
			.cycle_period_ns = period_ns,
		};

		if (first_domain_id(path, &domain_id))
			goto out;
		invalid_domain.domain_config_id = domain_id;

		if (expect_ioctl_errno(h, CW_EC_IOC_GET_INPUT_SNAPSHOT,
				       &invalid_snapshot, ENOSPC,
				       "undersized snapshot") ||
		    invalid_snapshot.data_size != activate.domain_size)
			goto out;
		invalid_snapshot.flags = 1;
		invalid_snapshot.data_capacity = activate.domain_size;
		if (expect_ioctl_errno(h, CW_EC_IOC_GET_INPUT_SNAPSHOT,
				       &invalid_snapshot, EINVAL,
				       "snapshot flags"))
			goto out;
		if (expect_ioctl_errno(h, CW_EC_IOC_PUBLISH_OUTPUT,
				       &invalid_output, ESTALE,
				       "stale output generation"))
			goto out;
		invalid_output.config_generation =
			io_status.config_generation;
		invalid_output.data_size = activate.domain_size - 1U;
		if (expect_ioctl_errno(h, CW_EC_IOC_PUBLISH_OUTPUT,
				       &invalid_output, EMSGSIZE,
				       "wrong output size"))
			goto out;
		if (expect_ioctl_errno(h, CW_EC_IOC_ARM_OUTPUTS,
				       &invalid_arm, ESTALE,
				       "stale arm generation"))
			goto out;
		invalid_arm.config_generation = io_status.config_generation;
		if (expect_ioctl_errno(h, CW_EC_IOC_ARM_OUTPUTS,
				       &invalid_arm, ESTALE,
				       "unknown output sequence"))
			goto out;
		if (expect_ioctl_errno(h, CW_EC_IOC_DISARM_OUTPUTS,
				       &invalid_disarm, ESTALE,
				       "stale disarm generation"))
			goto out;
		if (expect_ioctl_errno(h, CW_EC_IOC_CYCLE_ACTIVATE,
				       &duplicate_activate, EBUSY,
				       "duplicate activation"))
			goto out;
		if (expect_ioctl_errno(h, CW_EC_IOC_CYCLE_GET_INFO,
				       &invalid_cycle_info, EINVAL,
				       "cycle info reserved fields"))
			goto out;
		if (expect_ioctl_errno(h, CW_EC_IOC_CYCLE_WAIT,
				       &invalid_cycle_wait, ESTALE,
				       "stale cycle wait generation"))
			goto out;
		if (expect_ioctl_errno(h, CW_EC_IOC_CYCLE_SET_PERIOD,
				       &invalid_period_update, ESTALE,
				       "stale period-update generation"))
			goto out;
		invalid_cycle_wait.config_generation =
			io_status.config_generation;
		invalid_cycle_wait.after_cycle_index = UINT64_MAX;
		if (expect_ioctl_errno(h, CW_EC_IOC_CYCLE_WAIT,
				       &invalid_cycle_wait, EINVAL,
				       "future cycle wait index"))
			goto out;
		if (expect_ioctl_errno(h, CW_EC_IOC_GET_DOMAIN_STATUS,
				       &invalid_domain, ESTALE,
				       "stale domain generation"))
			goto out;
		invalid_domain.config_generation =
			io_status.config_generation;
		invalid_domain.domain_config_id = 0;
		if (expect_ioctl_errno(h, CW_EC_IOC_GET_DOMAIN_STATUS,
				       &invalid_domain, ENOENT,
				       "unknown domain ID"))
			goto out;
		invalid_domain.domain_config_id = domain_id;
		invalid_domain.reserved0[0] = 1;
		if (expect_ioctl_errno(h, CW_EC_IOC_GET_DOMAIN_STATUS,
				       &invalid_domain, EINVAL,
				       "domain status reserved fields"))
			goto out;
		invalid_domain.reserved0[0] = 0;
		if (lib_ret(cw_ec_get_domain_status(h, &invalid_domain)) < 0 ||
		    !invalid_domain.active || !invalid_domain.data_valid) {
			fprintf(stderr,
				"cw_ec_config: active domain status validation failed: %s\n",
				errno ? strerror(errno) : "invalid state");
			goto out;
		}
		printf("PASS: active hostile-ABI checks left outputs disarmed\n");
	}
	if (history_depth) {
		struct cw_ec_input_history_record *records;
		struct cw_ec_input_history_batch batch;
		struct timespec now;
		uint8_t *history_data;
		uint64_t cursor = 0;
		uint64_t total_records = 0;
		uint64_t total_dropped = 0;
		uint64_t end_ns;
		uint32_t max_records =
			history_depth < CW_EC_INPUT_HISTORY_BATCH_MAX ?
				history_depth :
				CW_EC_INPUT_HISTORY_BATCH_MAX;

		records = calloc(max_records, sizeof(*records));
		history_data = calloc((size_t)max_records,
				      activate.domain_size);
		if (!records || !history_data) {
			fprintf(stderr,
				"cw_ec_config: allocate history batch: %s\n",
				strerror(errno));
			free(records);
			free(history_data);
			goto out;
		}
		memset(&batch, 0, sizeof(batch));
		batch.struct_size = sizeof(batch);
		batch.api_major = CW_EC_API_VERSION_MAJOR;
		batch.config_generation = io_status.config_generation;
		batch.records_ptr = (uintptr_t)records;
		batch.data_ptr = (uintptr_t)history_data;
		batch.max_records = max_records;
		batch.data_capacity =
			max_records * activate.domain_size;
		if (lib_ret(cw_ec_get_input_history_batch(h, &batch)) < 0) {
			fprintf(stderr,
				"cw_ec_config: initial history cursor failed: %s\n",
				strerror(errno));
			free(records);
			free(history_data);
			goto out;
		}
		cursor = batch.latest_cycle_index;
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
			free(records);
			free(history_data);
			goto out;
		}
		end_ns = (uint64_t)now.tv_sec * 1000000000ULL +
			 (uint64_t)now.tv_nsec +
			 (uint64_t)duration_seconds * 1000000000ULL;
		for (;;) {
			uint64_t now_ns;

			usleep(10000);
			do {
				uint32_t i;

				memset(&batch, 0, sizeof(batch));
				batch.struct_size = sizeof(batch);
				batch.api_major =
					CW_EC_API_VERSION_MAJOR;
				batch.config_generation =
					io_status.config_generation;
				batch.after_cycle_index = cursor;
				batch.records_ptr =
					(uintptr_t)records;
				batch.data_ptr =
					(uintptr_t)history_data;
				batch.max_records = max_records;
				batch.data_capacity =
					max_records * activate.domain_size;
				if (lib_ret(cw_ec_get_input_history_batch(h, &batch)) < 0) {
					fprintf(stderr,
						"cw_ec_config: history batch failed: %s\n",
						strerror(errno));
					free(records);
					free(history_data);
					goto out;
				}
				for (i = 0; i < batch.record_count; i++) {
					if (records[i].cycle_index <= cursor ||
					    (i &&
					     records[i].cycle_index <=
						     records[i - 1U]
							     .cycle_index)) {
						fprintf(stderr,
							"cw_ec_config: unordered history record\n");
						free(records);
						free(history_data);
						goto out;
					}
				}
				total_records += batch.record_count;
				total_dropped += batch.dropped_records;
				if (batch.record_count)
					cursor =
						batch.last_cycle_index;
			} while (batch.record_count == max_records);
			if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
				free(records);
				free(history_data);
				goto out;
			}
			now_ns = (uint64_t)now.tv_sec * 1000000000ULL +
				 (uint64_t)now.tv_nsec;
			if (now_ns >= end_ns)
				break;
		}
		printf("input history: depth=%" PRIu32
		       " records=%" PRIu64 " cursor=%" PRIu64
		       " dropped_records=%" PRIu64
		       " capture_drops=%" PRIu64
		       " image_size=%" PRIu32 "\n",
		       history_depth, total_records, cursor,
		       total_dropped,
		       (uint64_t)batch.capture_drop_count,
		       batch.image_size);
		free(records);
		free(history_data);
	} else if (exchange_each_wake) {
		struct timespec now;
		uint64_t end_ns;
		uint64_t previous_cycle;
		uint64_t wakes = 0;
		uint64_t skipped = 0;
		uint64_t first_cycle = 0;
		uint64_t first_scheduled_ns = 0;
		uint64_t first_actual_ns = 0;
		uint64_t last_scheduled_ns = 0;
		uint64_t last_actual_ns = 0;
		struct latency_stats kernel_latency = { 0 };
		struct latency_stats user_latency = { 0 };

		snapshot_data = calloc(activate.domain_size, 1);
		output_data = calloc(activate.domain_size, 1);
		output_mask = malloc(activate.domain_size);
		if (!snapshot_data || !output_data || !output_mask) {
			fprintf(stderr,
				"cw_ec_config: allocate exchange images: %s\n",
				strerror(errno));
			goto out;
		}
		memset(output_mask, 0xff, activate.domain_size);
		snapshot.data_ptr = (uintptr_t)snapshot_data;
		snapshot.data_capacity = activate.domain_size;
		output.data_ptr = (uintptr_t)output_data;
		output.mask_ptr = (uintptr_t)output_mask;
		output.data_size = activate.domain_size;
		output.config_generation = io_status.config_generation;
		memset(&cycle_info, 0, sizeof(cycle_info));
		cycle_info.struct_size = sizeof(cycle_info);
		cycle_info.api_major = CW_EC_API_VERSION_MAJOR;
		if (lib_ret(cw_ec_cycle_info(h, &cycle_info)) < 0) {
			fprintf(stderr,
				"cw_ec_config: exchange initial cycle info failed: %s\n",
				strerror(errno));
			goto out;
		}
		cycle_wait.config_generation = io_status.config_generation;
		cycle_wait.timeout_ms = 1000;
		cycle_wait.after_cycle_index = cycle_info.cycle_index;
		previous_cycle = cycle_wait.after_cycle_index;
		if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
			fprintf(stderr, "cw_ec_config: clock_gettime: %s\n",
				strerror(errno));
			goto out;
		}
		end_ns = (uint64_t)now.tv_sec * 1000000000ULL +
			 (uint64_t)now.tv_nsec +
			 (uint64_t)duration_seconds * 1000000000ULL;
		for (;;) {
			int64_t observation_lateness;
			uint64_t observation_ns;
			uint64_t now_ns;

			if (lib_ret(cw_ec_cycle_wait(h, &cycle_wait)) < 0) {
				fprintf(stderr,
					"cw_ec_config: exchange cycle wait failed: %s\n",
					strerror(errno));
				goto out;
			}
			if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
				fprintf(stderr,
					"cw_ec_config: exchange observation clock failed: %s\n",
					strerror(errno));
				goto out;
			}
			observation_ns =
				(uint64_t)now.tv_sec * 1000000000ULL +
				(uint64_t)now.tv_nsec;
			observation_lateness =
				observation_ns >=
					 cycle_wait.cycle.scheduled_time_ns ?
				(int64_t)(
					observation_ns -
					cycle_wait.cycle.scheduled_time_ns) :
				-(int64_t)(
					cycle_wait.cycle.scheduled_time_ns -
					observation_ns);
			latency_add(&kernel_latency,
				    cycle_wait.cycle.wake_lateness_ns);
			latency_add(&user_latency, observation_lateness);
			if (!first_cycle) {
				first_cycle = cycle_wait.cycle.cycle_index;
				first_scheduled_ns =
					cycle_wait.cycle.scheduled_time_ns;
				first_actual_ns =
					cycle_wait.cycle.actual_wake_time_ns;
			}
			last_scheduled_ns =
				cycle_wait.cycle.scheduled_time_ns;
			last_actual_ns =
				cycle_wait.cycle.actual_wake_time_ns;
			if (cycle_wait.cycle.cycle_index > previous_cycle + 1U)
				skipped += cycle_wait.cycle.cycle_index -
					   previous_cycle - 1U;
			previous_cycle = cycle_wait.cycle.cycle_index;
			cycle_wait.after_cycle_index = previous_cycle;
			if (lib_ret(cw_ec_get_input_snapshot(h, &snapshot, (void *)(uintptr_t)snapshot.data_ptr, snapshot.data_capacity)) < 0) {
				fprintf(stderr,
					"cw_ec_config: exchange input snapshot failed: %s\n",
					strerror(errno));
				goto out;
			}
			if (lib_ret(cw_ec_publish_output(h, (const void *)(uintptr_t)output.data_ptr, (const void *)(uintptr_t)output.mask_ptr, output.data_size, &output)) < 0) {
				fprintf(stderr,
					"cw_ec_config: disarmed exchange publication failed: %s\n",
					strerror(errno));
				goto out;
			}
			wakes++;
			if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
				goto out;
			now_ns = (uint64_t)now.tv_sec * 1000000000ULL +
				 (uint64_t)now.tv_nsec;
			if (now_ns >= end_ns)
				break;
		}
		printf("user exchange: wakes=%" PRIu64
		       " skipped_cycles=%" PRIu64
		       " last_cycle=%" PRIu64
		       " input_sequence=%" PRIu64
		       " output_sequence=%" PRIu64 " armed=0\n",
		       wakes, skipped, previous_cycle,
		       (uint64_t)snapshot.input_sequence,
		       (uint64_t)output.output_sequence);
		print_latency_stats("kernel wake", &kernel_latency);
		print_latency_stats("user observation", &user_latency);
		if (previous_cycle > first_cycle) {
			uint64_t intervals = previous_cycle - first_cycle;
			uint64_t expected_span =
				intervals * (uint64_t)period_ns;
			uint64_t scheduled_span =
				last_scheduled_ns - first_scheduled_ns;
			uint64_t actual_span =
				last_actual_ns - first_actual_ns;
			int64_t grid_error =
				scheduled_span >= expected_span ?
					(int64_t)(scheduled_span -
						  expected_span) :
					-(int64_t)(expected_span -
						   scheduled_span);

			printf("exchange clock: intervals=%" PRIu64
			       " expected_span=%" PRIu64
			       " ns scheduled_span=%" PRIu64
			       " ns grid_error=%" PRId64
			       " ns actual_mean_period=%.3f ns\n",
			       intervals, expected_span, scheduled_span,
			       grid_error,
			       (double)actual_span / (double)intervals);
		}
	} else if (monitor) {
		unsigned int samples = duration_seconds * 4U;
		struct cw_ec_io_status previous = { 0 };
		bool have_previous = false;

		printf("READY: disarmed per-slave monitoring started\n");
		fflush(stdout);
		while (samples--) {
			io_status.struct_size = sizeof(io_status);
			io_status.api_major = CW_EC_API_VERSION_MAJOR;
			if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0) {
				fprintf(stderr,
					"cw_ec_config: monitored IO status failed: %s\n",
					strerror(errno));
				goto out;
			}
			if (!have_previous ||
			    io_status.bus_healthy != previous.bus_healthy ||
			    io_status.outputs_armed != previous.outputs_armed ||
			    io_status.rearm_required != previous.rearm_required ||
			    io_status.current_faults != previous.current_faults ||
			    io_status.configured_slaves_online !=
				    previous.configured_slaves_online ||
			    io_status.configured_slaves_operational !=
				    previous.configured_slaves_operational) {
				printf("monitor IO: healthy=%u armed=%u"
				       " rearm_required=%u faults=0x%08" PRIx32
				       " online=%" PRIu32
				       " operational=%" PRIu32 "\n",
				       io_status.bus_healthy,
				       io_status.outputs_armed,
				       io_status.rearm_required,
				       io_status.current_faults,
				       io_status.configured_slaves_online,
				       io_status.configured_slaves_operational);
				if (print_slave_statuses(
					    h, path,
					    io_status.config_generation))
					goto out;
				if (print_domain_statuses(
					    h, path,
					    io_status.config_generation))
					goto out;
				fflush(stdout);
				previous = io_status;
				have_previous = true;
			}
			usleep(250000);
		}
	} else if (!exchange_each_wake) {
		while (duration_seconds)
			duration_seconds = sleep(duration_seconds);
	}
	if (lib_ret(cw_ec_cycle_status(h, &status)) < 0) {
		fprintf(stderr, "cw_ec_config: status failed: %s\n",
			strerror(errno));
		goto out;
	}
	printf("cycle status: active=%u period=%" PRIu32 " ns cycles=%" PRIu64
	       " errors=%" PRIu64 " overruns=%" PRIu64
	       " maximum_lateness=%" PRIu64 " ns wc=%" PRIu32
	       " wc_state=%u last_result=%" PRId32 "\n",
	       status.active, status.cycle_period_ns,
	       (uint64_t)status.cycle_count,
	       (uint64_t)status.cycle_error_count,
	       (uint64_t)status.cycle_overrun_count,
	       (uint64_t)status.maximum_lateness_ns,
	       status.working_counter, status.working_counter_state,
	       status.last_cycle_result);
	cycle_info.struct_size = sizeof(cycle_info);
	cycle_info.api_major = CW_EC_API_VERSION_MAJOR;
	cycle_info.flags = 0;
	cycle_info.reserved0 = 0;
	cycle_info.reserved1 = 0;
	if (lib_ret(cw_ec_cycle_info(h, &cycle_info)) < 0) {
		fprintf(stderr, "cw_ec_config: cycle info failed: %s\n",
			strerror(errno));
		goto out;
	}
	printf("latest cycle: generation=%" PRIu64 " cycle=%" PRIu64
	       " scheduled=%" PRIu64 " actual=%" PRIu64
	       " lateness=%" PRId64 " ns input=%" PRIu64
	       " output_consumed=%" PRIu64 " stale=%" PRIu64
	       " missed=%" PRIu64 " wc=%" PRIu32 " wc_state=%u"
	       " armed=%u healthy=%u result=%" PRId32 "\n",
	       (uint64_t)cycle_info.config_generation,
	       (uint64_t)cycle_info.cycle_index,
	       (uint64_t)cycle_info.scheduled_time_ns,
	       (uint64_t)cycle_info.actual_wake_time_ns,
	       (int64_t)cycle_info.wake_lateness_ns,
	       (uint64_t)cycle_info.input_sequence,
	       (uint64_t)cycle_info.output_sequence_consumed,
	       (uint64_t)cycle_info.stale_output_cycles,
	       (uint64_t)cycle_info.missed_deadlines,
	       cycle_info.working_counter, cycle_info.working_counter_state,
	       cycle_info.outputs_armed, cycle_info.bus_healthy,
	       cycle_info.cycle_result);
	if (lib_ret(cw_ec_get_dc_status(h, &dc_status)) < 0) {
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
	if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0) {
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
	if (print_slave_statuses(h, path, io_status.config_generation))
		goto out;
	if (print_domain_statuses(h, path, io_status.config_generation))
		goto out;
	if (held_armed) {
		disarm.config_generation = output.config_generation;
		if (lib_ret(cw_ec_disarm_output(h, &disarm)) < 0) {
			fprintf(stderr,
				"cw_ec_config: held-output disarm failed: %s\n",
				strerror(errno));
			goto out;
		}
		held_armed = false;
		printf("held zero-output shadow synchronously disarmed\n");
	} else if (!exchange_each_wake) {
		output_data = malloc(activate.domain_size);
		output_mask = malloc(activate.domain_size);
		if (!output_data || !output_mask) {
			fprintf(stderr, "cw_ec_config: allocate output image: %s\n",
				strerror(errno));
			goto out;
		}
		memset(output_data, (arm_zero || lease_zero) ? 0x00 : 0xff,
		       activate.domain_size);
		memset(output_mask, 0xff, activate.domain_size);
		output.data_ptr = (uintptr_t)output_data;
		output.mask_ptr = (uintptr_t)output_mask;
		output.data_size = activate.domain_size;
		output.config_generation = io_status.config_generation;
		if (lib_ret(cw_ec_publish_output(h, (const void *)(uintptr_t)output.data_ptr, (const void *)(uintptr_t)output.mask_ptr, output.data_size, &output)) < 0) {
			fprintf(stderr, "cw_ec_config: output publish failed: %s\n",
				strerror(errno));
			goto out;
		}
		printf("published masked output shadow: generation=%" PRIu64
		       " sequence=%" PRIu64 "%s\n",
		       (uint64_t)output.config_generation,
		       (uint64_t)output.output_sequence,
		       (arm_zero || lease_zero) ?
			       " (zero-arm test pending)" :
				  " (outputs remain disarmed)");
		if (lease_zero) {
			uint64_t input_before = io_status.input_sequence;

			arm.config_generation = output.config_generation;
			arm.output_sequence = output.output_sequence;
			if (expect_ioctl_errno(h, CW_EC_IOC_ARM_OUTPUTS,
					       &arm, EAGAIN,
					       "arm without lease renewal"))
				goto out;
			lease_renew.config_generation =
				output.config_generation;
			if (lib_ret(cw_ec_renew_output_lease(h, &lease_renew)) < 0 ||
			    lease_renew.remaining_cycles != 100 ||
			    lease_renew.renewal_count != 1) {
				fprintf(stderr,
					"cw_ec_config: initial lease renewal failed: %s\n",
					errno ? strerror(errno) : "invalid result");
				goto out;
			}
			if (lib_ret(cw_ec_arm_output(h, &arm)) < 0) {
				fprintf(stderr,
					"cw_ec_config: leased zero arm failed: %s\n",
					strerror(errno));
				goto out;
			}
			usleep((useconds_t)(period_ns / 1000U) * 150U);
			io_status.struct_size = sizeof(io_status);
			io_status.api_major = CW_EC_API_VERSION_MAJOR;
			if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0 ||
			    io_status.outputs_armed ||
			    !io_status.rearm_required ||
			    !(io_status.current_faults &
			      CW_EC_IO_FAULT_CONTROLLER_STALE) ||
			    io_status.input_sequence <= input_before) {
				fprintf(stderr,
					"cw_ec_config: lease expiry did not gate outputs while inputs continued\n");
				goto out;
			}
			lease_status.config_generation =
				output.config_generation;
			if (lib_ret(cw_ec_get_output_lease_status(h, &lease_status)) < 0 ||
			    lease_status.valid ||
			    lease_status.remaining_cycles ||
			    lease_status.expiry_count != 1) {
				fprintf(stderr,
					"cw_ec_config: expired lease status invalid: %s\n",
					errno ? strerror(errno) : "invalid result");
				goto out;
			}
			printf("PASS: lease expiry zero-gated outputs and cyclic inputs continued\n");
			memset(&lease_renew, 0, sizeof(lease_renew));
			lease_renew.struct_size = sizeof(lease_renew);
			lease_renew.api_major = CW_EC_API_VERSION_MAJOR;
			lease_renew.config_generation =
				output.config_generation;
			if (lib_ret(cw_ec_renew_output_lease(h, &lease_renew)) < 0)
				goto out;
			if (expect_ioctl_errno(h, CW_EC_IOC_ARM_OUTPUTS,
					       &arm, EAGAIN,
					       "stale publication after lease expiry"))
				goto out;
			if (lib_ret(cw_ec_publish_output(h, (const void *)(uintptr_t)output.data_ptr, (const void *)(uintptr_t)output.mask_ptr, output.data_size, &output)) < 0)
				goto out;
			arm.output_sequence = output.output_sequence;
			disarm.config_generation = output.config_generation;
			if (lib_ret(cw_ec_arm_output(h, &arm)) < 0 ||
			    lib_ret(cw_ec_disarm_output(h, &disarm)) < 0) {
				fprintf(stderr,
					"cw_ec_config: lease recovery arm/disarm failed: %s\n",
					strerror(errno));
				goto out;
			}
			printf("PASS: renewal plus fresh zero publication allowed explicit re-arm\n");
		} else if (arm_zero) {
			arm.config_generation = output.config_generation;
			arm.output_sequence = output.output_sequence;
			if (lib_ret(cw_ec_arm_output(h, &arm)) < 0) {
				fprintf(stderr,
					"cw_ec_config: zero-output arm failed: %s\n",
					strerror(errno));
				goto out;
			}
			usleep((useconds_t)(period_ns / 1000U) * 10U);
			io_status.struct_size = sizeof(io_status);
			io_status.api_major = CW_EC_API_VERSION_MAJOR;
			if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0 ||
			    !io_status.outputs_armed) {
				fprintf(stderr,
					"cw_ec_config: zero-output arm was not reported active\n");
				goto out;
			}
			cycle_info.struct_size = sizeof(cycle_info);
			cycle_info.api_major = CW_EC_API_VERSION_MAJOR;
			if (lib_ret(cw_ec_cycle_info(h, &cycle_info)) < 0 ||
			    cycle_info.output_sequence_consumed !=
				    output.output_sequence ||
			    !cycle_info.stale_output_cycles) {
				fprintf(stderr,
					"cw_ec_config: armed output generation reuse was not reported\n");
				goto out;
			}
			printf("zero-output shadow armed at sequence=%" PRIu64 "\n",
			       (uint64_t)output.output_sequence);
			printf("armed reuse reported: consumed=%" PRIu64
			       " stale_cycles=%" PRIu64 "\n",
			       (uint64_t)
				       cycle_info.output_sequence_consumed,
			       (uint64_t)cycle_info.stale_output_cycles);
			disarm.config_generation = output.config_generation;
			if (lib_ret(cw_ec_disarm_output(h, &disarm)) < 0) {
				fprintf(stderr,
					"cw_ec_config: synchronous disarm failed: %s\n",
					strerror(errno));
				goto out;
			}
			io_status.struct_size = sizeof(io_status);
			io_status.api_major = CW_EC_API_VERSION_MAJOR;
			if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0 ||
			    io_status.outputs_armed ||
			    !io_status.rearm_required) {
				fprintf(stderr,
					"cw_ec_config: disarm state was not latched\n");
				goto out;
			}
			printf("zero-output shadow synchronously disarmed; fresh publication required\n");
			errno = 0;
			if (lib_ret(cw_ec_arm_output(h, &arm)) == 0 ||
			    errno != EAGAIN) {
				fprintf(stderr,
					"cw_ec_config: stale arm was not rejected with EAGAIN\n");
				goto out;
			}
			printf("stale output sequence correctly rejected after disarm\n");
			if (lib_ret(cw_ec_publish_output(h, (const void *)(uintptr_t)output.data_ptr, (const void *)(uintptr_t)output.mask_ptr, output.data_size, &output)) < 0) {
				fprintf(stderr,
					"cw_ec_config: fresh zero publication failed: %s\n",
					strerror(errno));
				goto out;
			}
			arm.output_sequence = output.output_sequence;
			if (lib_ret(cw_ec_arm_output(h, &arm)) < 0) {
				fprintf(stderr,
					"cw_ec_config: fresh zero arm failed: %s\n",
					strerror(errno));
				goto out;
			}
			printf("fresh zero-output sequence=%" PRIu64
			       " accepted after disarm\n",
			       (uint64_t)output.output_sequence);
			if (lib_ret(cw_ec_disarm_output(h, &disarm)) < 0) {
				fprintf(stderr,
					"cw_ec_config: final synchronous disarm failed: %s\n",
					strerror(errno));
				goto out;
			}
		}
	}
	usleep((useconds_t)(period_ns / 1000U) * 10U);
	if (!snapshot_data)
		snapshot_data = calloc(activate.domain_size, 1);
	if (!snapshot_data) {
		fprintf(stderr, "cw_ec_config: allocate input snapshot: %s\n",
			strerror(errno));
		goto out;
	}
	snapshot.data_ptr = (uintptr_t)snapshot_data;
	snapshot.data_capacity = activate.domain_size;
	if (lib_ret(cw_ec_get_input_snapshot(h, &snapshot, (void *)(uintptr_t)snapshot.data_ptr, snapshot.data_capacity)) < 0) {
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
	if (lib_ret(cw_ec_cycle_deactivate(h, &deactivate)) < 0) {
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
	cw_ec_close(h);
	return ret;
}

static void io_print_help(void)
{
	printf("commands:\n"
	       "  list                         list configured process entries\n"
	       "  read ENTRY_ID                read one value from the latest image\n"
	       "  watch ENTRY_ID COUNT MS      repeatedly read one value\n"
	       "  set ENTRY_ID VALUE           stage one output value\n"
	       "  zero                         stage zero for every output entry\n"
	       "  publish                      publish staged values, still disarmed\n"
	       "  arm                          explicitly enable the last publication\n"
	       "  disarm                       synchronously zero-gate outputs\n"
	       "  status                       show bus and output state\n"
	       "  help                         show this command list\n"
	       "  quit                         disarm, deactivate, and exit\n");
}

static int io_read_entry(cw_ec_handle *h, uint32_t domain_size, uint8_t *snapshot_data,
			 struct io_entry *entry)
{
	struct cw_ec_input_snapshot snapshot = {
		.struct_size = sizeof(snapshot),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.data_ptr = (uintptr_t)snapshot_data,
		.data_capacity = domain_size,
	};
	uint64_t value;

	if (entry->offset.bit_length > 64U) {
		fprintf(stderr,
			"cw_ec_io: entry width %u exceeds scalar display limit\n",
			entry->offset.bit_length);
		return -1;
	}
	if (lib_ret(cw_ec_get_input_snapshot(h, &snapshot, (void *)(uintptr_t)snapshot.data_ptr, snapshot.data_capacity)) < 0) {
		fprintf(stderr, "cw_ec_io: input snapshot failed: %s\n",
			strerror(errno));
		return -1;
	}
	value = image_get_value(snapshot_data, &entry->offset);
	printf("entry=%" PRIu32 " object=0x%04" PRIx16 ":%02" PRIx8
	       " direction=%s value=%" PRIu64 " (0x%" PRIx64
	       ") cycle=%" PRIu64 " sequence=%" PRIu64 "\n",
	       entry->cfg.entry_id, entry->cfg.index, entry->cfg.subindex,
	       entry->direction == CW_EC_DIR_INPUT ? "input" : "output",
	       value, value, (uint64_t)snapshot.cycle_count,
	       (uint64_t)snapshot.input_sequence);
	return 0;
}

static void io_print_status(cw_ec_handle *h)
{
	struct cw_ec_io_status status = {
		.struct_size = sizeof(status),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_cycle_status cycle = {
		.struct_size = sizeof(cycle),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};

	if (lib_ret(cw_ec_get_io_status(h, &status)) < 0 ||
	    lib_ret(cw_ec_cycle_status(h, &cycle)) < 0) {
		fprintf(stderr, "cw_ec_io: status failed: %s\n",
			strerror(errno));
		return;
	}
	printf("status: active=%u period=%" PRIu32 " ns cycles=%" PRIu64
	       " healthy=%u armed=%u rearm_required=%u faults=0x%08" PRIx32
	       " online=%" PRIu32 "/%" PRIu32
	       " operational=%" PRIu32 "/%" PRIu32 "\n",
	       cycle.active, cycle.cycle_period_ns,
	       (uint64_t)cycle.cycle_count, status.bus_healthy,
	       status.outputs_armed, status.rearm_required,
	       status.current_faults, status.configured_slaves_online,
	       status.configured_slave_count,
	       status.configured_slaves_operational,
	       status.configured_slave_count);
}

static int interactive_io(const char *path, uint32_t period_ns,
			  const char *device)
{
	struct cw_ec_config_validate validate;
	struct cw_ec_cycle_activate activate = {
		.struct_size = sizeof(activate),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.cycle_period_ns = period_ns,
	};
	struct cw_ec_cycle_deactivate deactivate = {
		.struct_size = sizeof(deactivate),
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
	struct cw_ec_io_status io_status = {
		.struct_size = sizeof(io_status),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct io_metadata metadata;
	struct counts counts;
	uint8_t *snapshot_data = NULL;
	uint8_t *output_data = NULL;
	uint8_t *output_mask = NULL;
	char command[256];
	bool active = false;
	bool armed = false;
	bool staged = false;
	bool published = false;
	cw_ec_handle *h = NULL;
	int ret = 1;
	uint32_t i;

	if (scan_config(path, &counts) ||
	    load_io_metadata(path, &counts, &metadata)) {
		fprintf(stderr, "cw_ec_io: cannot load configuration metadata\n");
		return 1;
	}
	if (open_handle(device, &h) < 0) {
		fprintf(stderr, "cw_ec_io: cannot open %s: %s\n",
			device, strerror(errno));
		goto out;
	}
	suppress_offset_output = true;
	if (configure_handle(h, path, &validate))
		goto out;
	for (i = 0; i < metadata.entry_count; i++) {
		metadata.entries[i].offset.struct_size =
			sizeof(metadata.entries[i].offset);
		metadata.entries[i].offset.api_major =
			CW_EC_API_VERSION_MAJOR;
		metadata.entries[i].offset.entry_id =
			metadata.entries[i].cfg.entry_id;
		if (lib_ret(cw_ec_get_entry_offset(h, &metadata.entries[i].offset)) < 0) {
			fprintf(stderr,
				"cw_ec_io: offset lookup for entry %" PRIu32
				" failed: %s\n",
				metadata.entries[i].cfg.entry_id,
				strerror(errno));
			goto out;
		}
	}
	if (lib_ret(cw_ec_cycle_activate(h, activate.cycle_period_ns, activate.flags, &activate)) < 0) {
		fprintf(stderr, "cw_ec_io: activation failed: %s\n",
			strerror(errno));
		goto out;
	}
	active = true;
	snapshot_data = calloc(activate.domain_size, 1);
	output_data = calloc(activate.domain_size, 1);
	output_mask = calloc(activate.domain_size, 1);
	if (!snapshot_data || !output_data || !output_mask) {
		fprintf(stderr, "cw_ec_io: allocate process images: %s\n",
			strerror(errno));
		goto out;
	}
	for (i = 0; i < 100; i++) {
		io_status.struct_size = sizeof(io_status);
		io_status.api_major = CW_EC_API_VERSION_MAJOR;
		if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0)
			goto out;
		if (io_status.bus_healthy)
			break;
		usleep(50000);
	}
	printf("cw_ec_io: active, outputs disarmed, image=%" PRIu32
	       " bytes, entries=%" PRIu32 "\n",
	       activate.domain_size, metadata.entry_count);
	if (!io_status.bus_healthy)
		printf("cw_ec_io: warning: bus is not fully healthy; reads remain available but arm will fail\n");
	io_print_help();

	while (printf("cw-ec> "), fflush(stdout),
	       fgets(command, sizeof(command), stdin)) {
		char *name;
		char *arg1;
		char *arg2;
		char *arg3;
		struct io_entry *entry;
		uint64_t value;

		name = strtok(command, " \t\r\n");
		arg1 = strtok(NULL, " \t\r\n");
		arg2 = strtok(NULL, " \t\r\n");
		arg3 = strtok(NULL, " \t\r\n");
		if (!name)
			continue;
		if (!strcmp(name, "quit") || !strcmp(name, "exit")) {
			ret = 0;
			break;
		}
		if (!strcmp(name, "help")) {
			io_print_help();
			continue;
		}
		if (!strcmp(name, "status")) {
			io_print_status(h);
			continue;
		}
		if (!strcmp(name, "list")) {
			for (i = 0; i < metadata.entry_count; i++)
				printf("%" PRIu32
				       " 0x%04" PRIx16 ":%02" PRIx8
				       " %-6s bits=%u offset=%" PRIu32
				       ".%u\n",
				       metadata.entries[i].cfg.entry_id,
				       metadata.entries[i].cfg.index,
				       metadata.entries[i].cfg.subindex,
				       metadata.entries[i].direction ==
						       CW_EC_DIR_INPUT ?
					       "input" : "output",
				       metadata.entries[i].offset.bit_length,
				       metadata.entries[i].offset.global_offset,
				       metadata.entries[i].offset.bit_position);
			continue;
		}
		if (!strcmp(name, "read") || !strcmp(name, "watch")) {
			uint64_t entry_id;
			uint64_t count = 1;
			uint64_t interval_ms = 100;

			if (!arg1 ||
			    parse_u64(arg1, UINT32_MAX, &entry_id) ||
			    !entry_id) {
				printf("usage: %s ENTRY_ID%s\n", name,
				       !strcmp(name, "watch") ?
					       " COUNT INTERVAL_MS" : "");
				continue;
			}
			if (!strcmp(name, "watch") &&
			    (!arg2 || !arg3 ||
			     parse_u64(arg2, 1000000, &count) || !count ||
			     parse_u64(arg3, 60000, &interval_ms))) {
				printf("usage: watch ENTRY_ID COUNT INTERVAL_MS\n");
				continue;
			}
			entry = find_io_entry(&metadata, entry_id);
			if (!entry) {
				printf("unknown entry ID\n");
				continue;
			}
			while (count--) {
				if (io_read_entry(h, activate.domain_size,
						  snapshot_data, entry))
					break;
				if (count)
					usleep((useconds_t)interval_ms * 1000U);
			}
			continue;
		}
		if (!strcmp(name, "set")) {
			uint64_t entry_id;

			if (armed) {
				printf("disarm before staging another value\n");
				continue;
			}
			if (!arg1 || !arg2 ||
			    parse_u64(arg1, UINT32_MAX, &entry_id) ||
			    parse_u64(arg2, UINT64_MAX, &value)) {
				printf("usage: set ENTRY_ID VALUE\n");
				continue;
			}
			entry = find_io_entry(&metadata, entry_id);
			if (!entry || entry->direction != CW_EC_DIR_OUTPUT) {
				printf("entry is not a configured output\n");
				continue;
			}
			if (entry->offset.bit_length > 64U ||
			    (entry->offset.bit_length < 64U &&
			     value >=
				     (1ULL << entry->offset.bit_length))) {
				printf("value does not fit the entry width\n");
				continue;
			}
			image_set_value(output_data, output_mask,
					&entry->offset, value);
			staged = true;
			published = false;
			printf("staged entry %" PRIu32 " = %" PRIu64
			       "; outputs remain disarmed\n",
			       entry->cfg.entry_id, value);
			continue;
		}
		if (!strcmp(name, "zero")) {
			if (armed) {
				printf("disarm before staging zero\n");
				continue;
			}
			memset(output_data, 0, activate.domain_size);
			memset(output_mask, 0, activate.domain_size);
			for (i = 0; i < metadata.entry_count; i++)
				if (metadata.entries[i].direction ==
				    CW_EC_DIR_OUTPUT)
					image_set_value(
						output_data, output_mask,
						&metadata.entries[i].offset, 0);
			staged = true;
			published = false;
			printf("staged zero for all configured outputs\n");
			continue;
		}
		if (!strcmp(name, "publish")) {
			if (armed) {
				printf("disarm before publishing another image\n");
				continue;
			}
			if (!staged) {
				printf("no staged output values\n");
				continue;
			}
			io_status.struct_size = sizeof(io_status);
			io_status.api_major = CW_EC_API_VERSION_MAJOR;
			if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0) {
				printf("status failed: %s\n", strerror(errno));
				continue;
			}
			output.data_ptr = (uintptr_t)output_data;
			output.mask_ptr = (uintptr_t)output_mask;
			output.data_size = activate.domain_size;
			output.config_generation =
				io_status.config_generation;
			if (lib_ret(cw_ec_publish_output(h, (const void *)(uintptr_t)output.data_ptr, (const void *)(uintptr_t)output.mask_ptr, output.data_size, &output)) < 0) {
				printf("publish failed: %s\n", strerror(errno));
				continue;
			}
			published = true;
			printf("published sequence=%" PRIu64
			       "; outputs remain disarmed\n",
			       (uint64_t)output.output_sequence);
			continue;
		}
		if (!strcmp(name, "arm")) {
			const char *authorized =
				getenv("CW_EC_NONZERO_OUTPUT_AUTHORIZED");

			if (!published) {
				printf("publish a staged image before arm\n");
				continue;
			}
			if (!authorized || strcmp(authorized, "YES")) {
				printf("arm refused: start cw_ec_io with CW_EC_NONZERO_OUTPUT_AUTHORIZED=YES only after physical safety approval\n");
				continue;
			}
			arm.config_generation = output.config_generation;
			arm.output_sequence = output.output_sequence;
			if (lib_ret(cw_ec_arm_output(h, &arm)) < 0) {
				printf("arm failed: %s\n", strerror(errno));
				continue;
			}
			armed = true;
			printf("ARMED sequence=%" PRIu64 "\n",
			       (uint64_t)output.output_sequence);
			continue;
		}
		if (!strcmp(name, "disarm")) {
			io_status.struct_size = sizeof(io_status);
			io_status.api_major = CW_EC_API_VERSION_MAJOR;
			if (lib_ret(cw_ec_get_io_status(h, &io_status)) < 0)
				continue;
			if (!io_status.outputs_armed) {
				armed = false;
				printf("outputs are already disarmed\n");
				continue;
			}
			disarm.config_generation =
				io_status.config_generation;
			if (lib_ret(cw_ec_disarm_output(h, &disarm)) < 0) {
				printf("disarm failed: %s\n", strerror(errno));
				continue;
			}
			armed = false;
			published = false;
			printf("outputs synchronously disarmed\n");
			continue;
		}
		printf("unknown command; enter help\n");
	}
	if (feof(stdin))
		ret = 0;

out:
	if (h && active) {
		io_status.struct_size = sizeof(io_status);
		io_status.api_major = CW_EC_API_VERSION_MAJOR;
		if (lib_ret(cw_ec_get_io_status(h, &io_status)) == 0 &&
		    io_status.outputs_armed) {
			disarm.config_generation =
				io_status.config_generation;
			if (lib_ret(cw_ec_disarm_output(h, &disarm)) < 0)
				fprintf(stderr,
					"cw_ec_io: cleanup disarm failed: %s\n",
					strerror(errno));
		}
		if (lib_ret(cw_ec_cycle_deactivate(h, &deactivate)) < 0)
			fprintf(stderr,
				"cw_ec_io: cleanup deactivation failed: %s\n",
				strerror(errno));
	}
	cw_ec_close(h);
	free(snapshot_data);
	free(output_data);
	free(output_mask);
	free_io_metadata(&metadata);
	suppress_offset_output = false;
	return ret;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/cw_ethercat0";
	struct counts counts;
	uint64_t duration;
	uint64_t entry_id;
	uint64_t period;
	uint64_t target_period;
	uint64_t history_depth;

	if (argc < 3 || argc > 7) {
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
	if (!strcmp(argv[1], "io") && (argc == 4 || argc == 5)) {
		if (parse_u64(argv[3], CW_EC_CYCLE_PERIOD_MAX_NS, &period) ||
		    period < CW_EC_CYCLE_PERIOD_MIN_NS) {
			fprintf(stderr, "cw_ec_io: invalid cycle period\n");
			return 2;
		}
		if (argc == 5)
			device = argv[4];
		return interactive_io(argv[2], period, device);
	}
	if (!strcmp(argv[1], "pulse-entry") &&
	    (argc == 6 || argc == 7)) {
		if (parse_u64(argv[3], CW_EC_CYCLE_PERIOD_MAX_NS, &period) ||
		    period < CW_EC_CYCLE_PERIOD_MIN_NS ||
		    parse_u64(argv[4], UINT32_MAX, &entry_id) || !entry_id ||
		    parse_u64(argv[5], 5000, &duration) || !duration) {
			fprintf(stderr,
				"cw_ec_config: invalid period, entry ID, or pulse duration\n");
			return 2;
		}
		if (argc == 7)
			device = argv[6];
		return pulse_entry(argv[2], period, entry_id, duration,
				   device);
	}
	if ((!strcmp(argv[1], "cycle-rate") ||
	     !strcmp(argv[1], "cycle-exchange-rate")) &&
	    (argc == 6 || argc == 7)) {
		if (parse_u64(argv[3], CW_EC_CYCLE_PERIOD_MAX_NS, &period) ||
		    period < CW_EC_CYCLE_PERIOD_MIN_NS ||
		    parse_u64(argv[4], CW_EC_CYCLE_PERIOD_MAX_NS,
			      &target_period) ||
		    target_period < CW_EC_CYCLE_PERIOD_MIN_NS ||
		    parse_u64(argv[5], 3600, &duration) || !duration) {
			fprintf(stderr,
				"cw_ec_config: invalid start period, target period, or duration\n");
			return 2;
		}
		if (argc == 7)
			device = argv[6];
		return cycle(argv[2], period, duration, false, false, false,
			     false, false, true, target_period,
			     !strcmp(argv[1], "cycle-exchange-rate"), 0,
			     device);
	}
	if (!strcmp(argv[1], "cycle-history") &&
	    (argc == 6 || argc == 7)) {
		if (parse_u64(argv[3], CW_EC_CYCLE_PERIOD_MAX_NS, &period) ||
		    period < CW_EC_CYCLE_PERIOD_MIN_NS ||
		    parse_u64(argv[4], CW_EC_INPUT_HISTORY_DEPTH_MAX,
			      &history_depth) ||
		    !history_depth ||
		    parse_u64(argv[5], 3600, &duration) || !duration) {
			fprintf(stderr,
				"cw_ec_config: invalid period, history depth, or duration\n");
			return 2;
		}
		if (argc == 7)
			device = argv[6];
		return cycle(argv[2], period, duration, false, false, false,
			     false, false, true, 0, false, history_depth,
			     device);
	}
	if ((!strcmp(argv[1], "cycle") ||
	     !strcmp(argv[1], "cycle-strict") ||
	     !strcmp(argv[1], "cycle-zero-arm") ||
	     !strcmp(argv[1], "cycle-zero-lease") ||
	     !strcmp(argv[1], "cycle-zero-hold") ||
	     !strcmp(argv[1], "cycle-monitor") ||
	     !strcmp(argv[1], "cycle-abi")) &&
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
			     !strcmp(argv[1], "cycle-zero-lease"),
			     !strcmp(argv[1], "cycle-zero-hold"),
			     !strcmp(argv[1], "cycle-monitor"),
			     !strcmp(argv[1], "cycle-abi"),
			     !strcmp(argv[1], "cycle-strict"), 0, false,
			     0, device);
	}
	usage(argv[0]);
	return 2;
}
