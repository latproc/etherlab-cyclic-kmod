// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "cw_ethercat_uapi.h"

static void usage(const char *program)
{
	fprintf(stderr,
		"usage:\n"
		"  %s write POSITION TYPE INDEX SUBINDEX VALUE [DEVICE]\n"
		"  %s read POSITION INDEX SUBINDEX MAX_BYTES [DEVICE]\n"
		"  %s recipe FILE [DEVICE]\n"
		"  %s validate POSITION TYPE INDEX SUBINDEX VALUE\n"
		"\n"
		"TYPE: u8, s8, u16, s16, u32, s32, bytes\n"
		"VALUE: integer (base 0), or an even-length hex string for bytes\n",
		program, program, program, program);
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

static int parse_s64(const char *text, int64_t minimum, int64_t maximum,
		     int64_t *value)
{
	char *end;
	intmax_t parsed;

	if (!text[0])
		return -1;
	errno = 0;
	parsed = strtoimax(text, &end, 0);
	if (errno || *end || parsed < minimum || parsed > maximum)
		return -1;
	*value = parsed;
	return 0;
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static int parse_bytes(const char *text, uint8_t *data, uint16_t *data_len)
{
	size_t length = strlen(text);
	size_t i;

	if (length >= 2 && text[0] == '0' &&
	    (text[1] == 'x' || text[1] == 'X')) {
		text += 2;
		length -= 2;
	}
	if (!length || length % 2 ||
	    length / 2 > CW_EC_SETUP_SDO_DATA_MAX)
		return -1;

	for (i = 0; i < length / 2; i++) {
		int high = hex_nibble(text[i * 2]);
		int low = hex_nibble(text[i * 2 + 1]);

		if (high < 0 || low < 0)
			return -1;
		data[i] = (uint8_t)((high << 4) | low);
	}
	*data_len = length / 2;
	return 0;
}

static int encode_value(const char *type, const char *text,
			struct cw_ec_setup_sdo *request)
{
	uint64_t unsigned_value;
	int64_t signed_value;

	if (!strcmp(type, "u8")) {
		if (parse_u64(text, UINT8_MAX, &unsigned_value))
			return -1;
		request->type = CW_EC_SDO_U8;
		request->data_len = 1;
		request->data[0] = unsigned_value;
	} else if (!strcmp(type, "s8")) {
		if (parse_s64(text, INT8_MIN, INT8_MAX, &signed_value))
			return -1;
		request->type = CW_EC_SDO_S8;
		request->data_len = 1;
		request->data[0] = (uint8_t)(int8_t)signed_value;
	} else if (!strcmp(type, "u16")) {
		if (parse_u64(text, UINT16_MAX, &unsigned_value))
			return -1;
		request->type = CW_EC_SDO_U16;
		request->data_len = 2;
		request->data[0] = unsigned_value;
		request->data[1] = unsigned_value >> 8;
	} else if (!strcmp(type, "s16")) {
		uint16_t encoded;

		if (parse_s64(text, INT16_MIN, INT16_MAX, &signed_value))
			return -1;
		encoded = (uint16_t)(int16_t)signed_value;
		request->type = CW_EC_SDO_S16;
		request->data_len = 2;
		request->data[0] = encoded;
		request->data[1] = encoded >> 8;
	} else if (!strcmp(type, "u32")) {
		if (parse_u64(text, UINT32_MAX, &unsigned_value))
			return -1;
		request->type = CW_EC_SDO_U32;
		request->data_len = 4;
		request->data[0] = unsigned_value;
		request->data[1] = unsigned_value >> 8;
		request->data[2] = unsigned_value >> 16;
		request->data[3] = unsigned_value >> 24;
	} else if (!strcmp(type, "s32")) {
		uint32_t encoded;

		if (parse_s64(text, INT32_MIN, INT32_MAX, &signed_value))
			return -1;
		encoded = (uint32_t)(int32_t)signed_value;
		request->type = CW_EC_SDO_S32;
		request->data_len = 4;
		request->data[0] = encoded;
		request->data[1] = encoded >> 8;
		request->data[2] = encoded >> 16;
		request->data[3] = encoded >> 24;
	} else if (!strcmp(type, "bytes")) {
		request->type = CW_EC_SDO_BYTES;
		if (parse_bytes(text, request->data, &request->data_len))
			return -1;
	} else {
		return -1;
	}

	return 0;
}

static int prepare_request(char **argv, struct cw_ec_setup_sdo *request)
{
	uint64_t value;

	memset(request, 0, sizeof(*request));
	request->struct_size = sizeof(*request);
	request->api_major = CW_EC_API_VERSION_MAJOR;
	request->sequence = 1;

	if (parse_u64(argv[2], UINT16_MAX, &value))
		return -1;
	request->position = value;
	if (parse_u64(argv[4], UINT16_MAX, &value) || !value)
		return -1;
	request->index = value;
	if (parse_u64(argv[5], UINT8_MAX, &value))
		return -1;
	request->subindex = value;

	return encode_value(argv[3], argv[6], request);
}

static int execute_write(const char *device,
			 const struct cw_ec_setup_sdo *request)
{
	struct cw_ec_setup_begin begin = {
		.struct_size = sizeof(begin),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_setup_apply apply = {
		.struct_size = sizeof(apply),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	int saved_errno;
	int fd;

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "cw_ec_sdo: cannot open %s: %s\n",
			device, strerror(errno));
		return 1;
	}
	if (ioctl(fd, CW_EC_IOC_SETUP_BEGIN, &begin) < 0) {
		fprintf(stderr, "cw_ec_sdo: SETUP_BEGIN: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}
	if (ioctl(fd, CW_EC_IOC_SETUP_ADD_SDO, request) < 0) {
		fprintf(stderr, "cw_ec_sdo: SETUP_ADD_SDO: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}
	if (ioctl(fd, CW_EC_IOC_SETUP_APPLY, &apply) < 0) {
		saved_errno = errno;
		fprintf(stderr,
			"cw_ec_sdo: write failed: %s; sequence=%" PRIu32
			" slave=%" PRIu16 " object=0x%04" PRIx16 ":%02" PRIx8
			" abort=0x%08" PRIx32 "\n",
			strerror(saved_errno), apply.failed_sequence,
			apply.failed_position, apply.failed_index,
			apply.failed_subindex, apply.abort_code);
		close(fd);
		return 1;
	}

	printf("wrote sequence 1 to slave %" PRIu16
	       " object 0x%04" PRIx16 ":%02" PRIx8 " (%" PRIu16 " bytes)\n",
	       request->position, request->index, request->subindex,
	       request->data_len);

	if (close(fd) < 0) {
		fprintf(stderr, "cw_ec_sdo: close: %s\n", strerror(errno));
		return 1;
	}
	return 0;
}

static int execute_read(const char *device, char **argv)
{
	struct cw_ec_sdo_upload upload = {
		.struct_size = sizeof(upload),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	uint64_t value;
	unsigned int i;
	int saved_errno;
	int fd;

	if (parse_u64(argv[2], UINT16_MAX, &value))
		return 2;
	upload.position = value;
	if (parse_u64(argv[3], UINT16_MAX, &value) || !value)
		return 2;
	upload.index = value;
	if (parse_u64(argv[4], UINT8_MAX, &value))
		return 2;
	upload.subindex = value;
	if (parse_u64(argv[5], CW_EC_SETUP_SDO_DATA_MAX, &value) || !value)
		return 2;
	upload.requested_len = value;

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "cw_ec_sdo: cannot open %s: %s\n",
			device, strerror(errno));
		return 1;
	}
	if (ioctl(fd, CW_EC_IOC_SDO_UPLOAD, &upload) < 0) {
		saved_errno = errno;
		fprintf(stderr,
			"cw_ec_sdo: read slave %" PRIu16 " object 0x%04" PRIx16
			":%02" PRIx8 " failed: %s; abort=0x%08" PRIx32 "\n",
			upload.position, upload.index, upload.subindex,
			strerror(saved_errno), upload.abort_code);
		close(fd);
		return 1;
	}

	printf("slave %" PRIu16 " 0x%04" PRIx16 ":%02" PRIx8 " = ",
	       upload.position, upload.index, upload.subindex);
	for (i = 0; i < upload.result_len; i++)
		printf("%02" PRIx8, upload.data[i]);
	printf(" (%" PRIu16 " bytes)\n", upload.result_len);

	if (close(fd) < 0) {
		fprintf(stderr, "cw_ec_sdo: close: %s\n", strerror(errno));
		return 1;
	}
	return 0;
}

static int execute_recipe(const char *device, const char *path)
{
	struct cw_ec_setup_begin begin = {
		.struct_size = sizeof(begin),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	struct cw_ec_setup_apply apply = {
		.struct_size = sizeof(apply),
		.api_major = CW_EC_API_VERSION_MAJOR,
	};
	char line[1024];
	unsigned int line_number = 0;
	unsigned int count = 0;
	uint32_t last_sequence = 0;
	FILE *stream;
	int fd;

	stream = fopen(path, "r");
	if (!stream) {
		fprintf(stderr, "cw_ec_sdo: cannot open recipe %s: %s\n",
			path, strerror(errno));
		return 1;
	}

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "cw_ec_sdo: cannot open %s: %s\n",
			device, strerror(errno));
		fclose(stream);
		return 1;
	}
	if (ioctl(fd, CW_EC_IOC_SETUP_BEGIN, &begin) < 0) {
		fprintf(stderr, "cw_ec_sdo: SETUP_BEGIN: %s\n",
			strerror(errno));
		fclose(stream);
		close(fd);
		return 1;
	}

	while (fgets(line, sizeof(line), stream)) {
		struct cw_ec_setup_sdo request;
		char *tokens[6];
		char *cursor;
		char *extra;
		char *saveptr = NULL;
		char *fake_argv[7] = { NULL, NULL };
		uint64_t sequence;
		unsigned int token_count = 0;

		line_number++;
		cursor = line;
		while (*cursor == ' ' || *cursor == '\t')
			cursor++;
		if (!*cursor || *cursor == '\n' || *cursor == '#')
			continue;

		cursor = strtok_r(cursor, " \t\r\n", &saveptr);
		while (cursor && token_count < 6) {
			if (cursor[0] == '#')
				break;
			tokens[token_count++] = cursor;
			cursor = strtok_r(NULL, " \t\r\n", &saveptr);
		}
		extra = cursor;
		if (token_count != 6 ||
		    (extra && extra[0] != '#') ||
		    parse_u64(tokens[0], UINT32_MAX, &sequence) ||
		    !sequence || sequence <= last_sequence) {
			fprintf(stderr, "cw_ec_sdo: invalid recipe line %u\n",
				line_number);
			fclose(stream);
			close(fd);
			return 2;
		}

		fake_argv[2] = tokens[1];
		fake_argv[3] = tokens[2];
		fake_argv[4] = tokens[3];
		fake_argv[5] = tokens[4];
		fake_argv[6] = tokens[5];
		if (prepare_request(fake_argv, &request)) {
			fprintf(stderr, "cw_ec_sdo: invalid recipe line %u\n",
				line_number);
			fclose(stream);
			close(fd);
			return 2;
		}
		request.sequence = sequence;

		if (ioctl(fd, CW_EC_IOC_SETUP_ADD_SDO, &request) < 0) {
			fprintf(stderr,
				"cw_ec_sdo: add recipe line %u: %s\n",
				line_number, strerror(errno));
			fclose(stream);
			close(fd);
			return 1;
		}
		last_sequence = sequence;
		count++;
	}
	if (ferror(stream)) {
		fprintf(stderr, "cw_ec_sdo: read recipe %s: %s\n",
			path, strerror(errno));
		fclose(stream);
		close(fd);
		return 1;
	}
	fclose(stream);

	if (!count) {
		fprintf(stderr, "cw_ec_sdo: recipe contains no operations\n");
		close(fd);
		return 2;
	}

	if (ioctl(fd, CW_EC_IOC_SETUP_APPLY, &apply) < 0) {
		fprintf(stderr,
			"cw_ec_sdo: recipe failed: result=%" PRId32
			" sequence=%" PRIu32 " slave=%" PRIu16
			" object=0x%04" PRIx16 ":%02" PRIx8
			" abort=0x%08" PRIx32 "\n",
			apply.result, apply.failed_sequence,
			apply.failed_position, apply.failed_index,
			apply.failed_subindex, apply.abort_code);
		close(fd);
		return 1;
	}

	printf("applied %u ordered SDO writes from %s\n", count, path);
	if (close(fd) < 0) {
		fprintf(stderr, "cw_ec_sdo: close: %s\n", strerror(errno));
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/cw_ethercat0";
	struct cw_ec_setup_sdo request;
	int validate_only;

	if (argc < 2 ||
	    (strcmp(argv[1], "write") && strcmp(argv[1], "read") &&
	     strcmp(argv[1], "recipe") && strcmp(argv[1], "validate"))) {
		usage(argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "read")) {
		if (argc != 6 && argc != 7) {
			usage(argv[0]);
			return 2;
		}
		if (argc == 7)
			device = argv[6];
		return execute_read(device, argv);
	}
	if (!strcmp(argv[1], "recipe")) {
		if (argc != 3 && argc != 4) {
			usage(argv[0]);
			return 2;
		}
		if (argc == 4)
			device = argv[3];
		return execute_recipe(device, argv[2]);
	}
	validate_only = !strcmp(argv[1], "validate");
	if ((!validate_only && argc != 7 && argc != 8) ||
	    (validate_only && argc != 7)) {
		usage(argv[0]);
		return 2;
	}
	if (argc == 8)
		device = argv[7];

	if (prepare_request(argv, &request)) {
		fprintf(stderr, "cw_ec_sdo: invalid position/type/object/value\n");
		return 2;
	}

	if (validate_only) {
		printf("valid: slave %" PRIu16 " object 0x%04" PRIx16
		       ":%02" PRIx8 ", type %u, %" PRIu16 " bytes\n",
		       request.position, request.index, request.subindex,
		       request.type, request.data_len);
		return 0;
	}

	return execute_write(device, &request);
}
