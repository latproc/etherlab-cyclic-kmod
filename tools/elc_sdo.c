// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elc_ethercat.h"

static void usage(const char *program)
{
	fprintf(stderr,
		"usage:\n"
		"  %s write POSITION TYPE INDEX SUBINDEX VALUE [DEVICE]\n"
		"  %s read POSITION INDEX SUBINDEX MAX_BYTES [DEVICE]\n"
		"  %s recipe FILE [DEVICE]\n"
		"  %s stage FILE [DEVICE]\n"
		"  %s validate POSITION TYPE INDEX SUBINDEX VALUE\n"
		"\n"
		"TYPE: u8, s8, u16, s16, u32, s32, bytes\n"
		"VALUE: integer (base 0), or an even-length hex string for bytes\n",
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
	    length / 2 > ELC_SETUP_SDO_DATA_MAX)
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
			struct elc_setup_sdo *request)
{
	uint64_t unsigned_value;
	int64_t signed_value;

	if (!strcmp(type, "u8")) {
		if (parse_u64(text, UINT8_MAX, &unsigned_value))
			return -1;
		request->type = ELC_SDO_U8;
		request->data_len = 1;
		request->data[0] = unsigned_value;
	} else if (!strcmp(type, "s8")) {
		if (parse_s64(text, INT8_MIN, INT8_MAX, &signed_value))
			return -1;
		request->type = ELC_SDO_S8;
		request->data_len = 1;
		request->data[0] = (uint8_t)(int8_t)signed_value;
	} else if (!strcmp(type, "u16")) {
		if (parse_u64(text, UINT16_MAX, &unsigned_value))
			return -1;
		request->type = ELC_SDO_U16;
		request->data_len = 2;
		request->data[0] = unsigned_value;
		request->data[1] = unsigned_value >> 8;
	} else if (!strcmp(type, "s16")) {
		uint16_t encoded;

		if (parse_s64(text, INT16_MIN, INT16_MAX, &signed_value))
			return -1;
		encoded = (uint16_t)(int16_t)signed_value;
		request->type = ELC_SDO_S16;
		request->data_len = 2;
		request->data[0] = encoded;
		request->data[1] = encoded >> 8;
	} else if (!strcmp(type, "u32")) {
		if (parse_u64(text, UINT32_MAX, &unsigned_value))
			return -1;
		request->type = ELC_SDO_U32;
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
		request->type = ELC_SDO_S32;
		request->data_len = 4;
		request->data[0] = encoded;
		request->data[1] = encoded >> 8;
		request->data[2] = encoded >> 16;
		request->data[3] = encoded >> 24;
	} else if (!strcmp(type, "bytes")) {
		request->type = ELC_SDO_BYTES;
		if (parse_bytes(text, request->data, &request->data_len))
			return -1;
	} else {
		return -1;
	}

	return 0;
}

static int prepare_request(char **argv, struct elc_setup_sdo *request)
{
	uint64_t value;

	memset(request, 0, sizeof(*request));
	request->struct_size = sizeof(*request);
	request->api_major = ELC_API_VERSION_MAJOR;
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

static int open_device(const char *device, elc_handle **out)
{
	int ret = elc_open(device, out);

	if (ret) {
		fprintf(stderr, "elc_sdo: cannot open %s: %s\n", device,
			strerror(-ret));
		return 1;
	}
	return 0;
}

static int execute_write(const char *device,
			 const struct elc_setup_sdo *request)
{
	struct elc_setup_apply apply;
	elc_handle *h = NULL;
	int ret;

	if (open_device(device, &h))
		return 1;

	ret = elc_setup_begin(h);
	if (ret) {
		fprintf(stderr, "elc_sdo: SETUP_BEGIN: %s\n",
			strerror(-ret));
		elc_close(h);
		return 1;
	}
	ret = elc_setup_add_sdo(h, request);
	if (ret) {
		fprintf(stderr, "elc_sdo: SETUP_ADD_SDO: %s\n",
			strerror(-ret));
		elc_close(h);
		return 1;
	}
	ret = elc_setup_apply(h, &apply);
	if (ret) {
		fprintf(stderr,
			"elc_sdo: write failed: %s; sequence=%" PRIu32
			" slave=%" PRIu16 " object=0x%04" PRIx16 ":%02" PRIx8
			" abort=0x%08" PRIx32 "\n",
			strerror(-ret), apply.failed_sequence,
			apply.failed_position, apply.failed_index,
			apply.failed_subindex, apply.abort_code);
		elc_close(h);
		return 1;
	}

	printf("wrote sequence 1 to slave %" PRIu16
	       " object 0x%04" PRIx16 ":%02" PRIx8 " (%" PRIu16 " bytes)\n",
	       request->position, request->index, request->subindex,
	       request->data_len);

	elc_close(h);
	return 0;
}

static int execute_read(const char *device, char **argv)
{
	struct elc_sdo_upload upload = {
		.struct_size = sizeof(upload),
		.api_major = ELC_API_VERSION_MAJOR,
	};
	elc_handle *h = NULL;
	uint64_t value;
	unsigned int i;
	int ret;

	if (parse_u64(argv[2], UINT16_MAX, &value))
		return 2;
	upload.position = value;
	if (parse_u64(argv[3], UINT16_MAX, &value) || !value)
		return 2;
	upload.index = value;
	if (parse_u64(argv[4], UINT8_MAX, &value))
		return 2;
	upload.subindex = value;
	if (parse_u64(argv[5], ELC_SETUP_SDO_DATA_MAX, &value) || !value)
		return 2;
	upload.requested_len = value;

	if (open_device(device, &h))
		return 1;

	ret = elc_sdo_upload(h, &upload);
	if (ret) {
		fprintf(stderr,
			"elc_sdo: read slave %" PRIu16 " object 0x%04" PRIx16
			":%02" PRIx8 " failed: %s; abort=0x%08" PRIx32 "\n",
			upload.position, upload.index, upload.subindex,
			strerror(-ret), upload.abort_code);
		elc_close(h);
		return 1;
	}

	printf("slave %" PRIu16 " 0x%04" PRIx16 ":%02" PRIx8 " = ",
	       upload.position, upload.index, upload.subindex);
	for (i = 0; i < upload.result_len; i++)
		printf("%02" PRIx8, upload.data[i]);
	printf(" (%" PRIu16 " bytes)\n", upload.result_len);

	elc_close(h);
	return 0;
}

static int execute_recipe(const char *device, const char *path, int apply_recipe)
{
	struct elc_setup_apply apply;
	char line[1024];
	unsigned int line_number = 0;
	unsigned int count = 0;
	uint32_t last_sequence = 0;
	FILE *stream;
	elc_handle *h = NULL;
	int ret;

	stream = fopen(path, "r");
	if (!stream) {
		fprintf(stderr, "elc_sdo: cannot open recipe %s: %s\n",
			path, strerror(errno));
		return 1;
	}

	if (open_device(device, &h)) {
		fclose(stream);
		return 1;
	}
	ret = elc_setup_begin(h);
	if (ret) {
		fprintf(stderr, "elc_sdo: SETUP_BEGIN: %s\n",
			strerror(-ret));
		fclose(stream);
		elc_close(h);
		return 1;
	}

	while (fgets(line, sizeof(line), stream)) {
		struct elc_setup_sdo request;
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
			fprintf(stderr, "elc_sdo: invalid recipe line %u\n",
				line_number);
			fclose(stream);
			elc_close(h);
			return 2;
		}

		fake_argv[2] = tokens[1];
		fake_argv[3] = tokens[2];
		fake_argv[4] = tokens[3];
		fake_argv[5] = tokens[4];
		fake_argv[6] = tokens[5];
		if (prepare_request(fake_argv, &request)) {
			fprintf(stderr, "elc_sdo: invalid recipe line %u\n",
				line_number);
			fclose(stream);
			elc_close(h);
			return 2;
		}
		request.sequence = sequence;

		ret = elc_setup_add_sdo(h, &request);
		if (ret) {
			fprintf(stderr,
				"elc_sdo: add recipe line %u: %s\n",
				line_number, strerror(-ret));
			fclose(stream);
			elc_close(h);
			return 1;
		}
		last_sequence = sequence;
		count++;
	}
	if (ferror(stream)) {
		fprintf(stderr, "elc_sdo: read recipe %s: %s\n",
			path, strerror(errno));
		fclose(stream);
		elc_close(h);
		return 1;
	}
	fclose(stream);

	if (!count) {
		fprintf(stderr, "elc_sdo: recipe contains no operations\n");
		elc_close(h);
		return 2;
	}

	if (apply_recipe) {
		ret = elc_setup_apply(h, &apply);
		if (ret) {
			fprintf(stderr,
				"elc_sdo: recipe failed: result=%" PRId32
				" sequence=%" PRIu32 " slave=%" PRIu16
				" object=0x%04" PRIx16 ":%02" PRIx8
				" abort=0x%08" PRIx32 "\n",
				apply.result, apply.failed_sequence,
				apply.failed_position, apply.failed_index,
				apply.failed_subindex, apply.abort_code);
			elc_close(h);
			return 1;
		}
	}

	printf("%s %u ordered SDO writes from %s\n",
	       apply_recipe ? "applied" : "staged without applying",
	       count, path);
	elc_close(h);
	return 0;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/elc_ethercat0";
	struct elc_setup_sdo request;
	int validate_only;

	if (argc < 2 ||
	    (strcmp(argv[1], "write") && strcmp(argv[1], "read") &&
	     strcmp(argv[1], "recipe") && strcmp(argv[1], "stage") &&
	     strcmp(argv[1], "validate"))) {
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
	if (!strcmp(argv[1], "recipe") || !strcmp(argv[1], "stage")) {
		if (argc != 3 && argc != 4) {
			usage(argv[0]);
			return 2;
		}
		if (argc == 4)
			device = argv[3];
		return execute_recipe(device, argv[2],
				      !strcmp(argv[1], "recipe"));
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
		fprintf(stderr, "elc_sdo: invalid position/type/object/value\n");
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
