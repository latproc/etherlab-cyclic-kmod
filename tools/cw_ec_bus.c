// SPDX-License-Identifier: GPL-2.0-only

#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cw_ethercat.h"

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
	cw_ec_handle *h = NULL;
	struct cw_ec_api_version version;
	struct cw_ec_capabilities capabilities;
	struct cw_ec_master_info master;
	cw_ec_slave_summary *slaves = NULL;
	size_t slave_count = 0;
	size_t i;
	int ret;

	if (argc > 2) {
		fprintf(stderr, "usage: %s [device]\n", argv[0]);
		return 2;
	}
	if (argc == 2)
		device = argv[1];

	ret = cw_ec_open(device, &h);
	if (ret) {
		fprintf(stderr, "cw_ec_bus: cannot open %s: %s\n", device,
			strerror(-ret));
		if (ret == -EBUSY)
			fprintf(stderr,
				"cw_ec_bus: EtherLab master 0 is already owned\n");
		return 1;
	}

	ret = cw_ec_require_api(h, CW_EC_API_VERSION_MAJOR,
				CW_EC_API_VERSION_MINOR);
	if (ret) {
		if (cw_ec_get_api_version(h, &version) == 0) {
			fprintf(stderr,
				"cw_ec_bus: incompatible API: kernel %u.%u, tool %u.%u\n",
				version.major, version.minor,
				CW_EC_API_VERSION_MAJOR,
				CW_EC_API_VERSION_MINOR);
		} else {
			fprintf(stderr, "cw_ec_bus: require API: %s\n",
				strerror(-ret));
		}
		cw_ec_close(h);
		return 1;
	}

	ret = cw_ec_get_api_version(h, &version);
	if (ret) {
		fprintf(stderr, "cw_ec_bus: GET_API_VERSION: %s\n",
			strerror(-ret));
		cw_ec_close(h);
		return 1;
	}

	ret = cw_ec_get_capabilities(h, &capabilities);
	if (ret) {
		fprintf(stderr, "cw_ec_bus: GET_CAPABILITIES: %s\n",
			strerror(-ret));
		cw_ec_close(h);
		return 1;
	}

	ret = cw_ec_get_master_info(h, &master);
	if (ret) {
		fprintf(stderr, "cw_ec_bus: GET_MASTER_INFO: %s\n",
			strerror(-ret));
		cw_ec_close(h);
		return 1;
	}

	printf("API: %u.%u\n", version.major, version.minor);
	printf("capabilities: 0x%016" PRIx64 "\n",
	       (uint64_t)capabilities.capabilities);
	printf("master: 0\n");
	printf("link: %s\n", master.link_up ? "up" : "down");
	printf("scan: %s\n", master.scan_busy ? "busy" : "complete");
	printf("slaves: %" PRIu32 "\n\n", master.slave_count);
	printf("pos  alias  vendor      product     revision    state   err  name\n");

	if (master.slave_count) {
		slaves = calloc(master.slave_count, sizeof(*slaves));
		if (!slaves) {
			fprintf(stderr, "cw_ec_bus: out of memory\n");
			cw_ec_close(h);
			return 1;
		}
		ret = cw_ec_list_slaves(h, slaves, master.slave_count,
					&slave_count);
		if (ret) {
			fprintf(stderr, "cw_ec_bus: list slaves: %s\n",
				strerror(-ret));
			free(slaves);
			cw_ec_close(h);
			return 1;
		}
	}

	for (i = 0; i < slave_count; i++) {
		printf("%-4" PRIu16 " %-6" PRIu16 " 0x%08" PRIx32
		       "  0x%08" PRIx32 "  0x%08" PRIx32 "  %-7s %-4u %s\n",
		       slaves[i].position, slaves[i].alias, slaves[i].vendor_id,
		       slaves[i].product_code, slaves[i].revision_number,
		       al_state_name(slaves[i].al_state), slaves[i].error_flag,
		       slaves[i].name);
	}

	free(slaves);
	cw_ec_close(h);
	return 0;
}
