/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Public C API for libcwethercat — thin, policy-free userspace client for
 * the cw_ethercat kernel transport UAPI.
 *
 * Normative wire structures and ioctls: cw_ethercat_uapi.h
 * Design contract: docs/libcwethercat.md
 */

#ifndef CW_ETHERCAT_H
#define CW_ETHERCAT_H

#include "cw_ethercat_uapi.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque control-device owner. One handle maps to one exclusive
 * /dev/cw_ethercat* file descriptor (EtherLab master 0).
 * Not thread-safe unless the caller serialises all use of a given handle.
 */
typedef struct cw_ec_handle cw_ec_handle;

/* Stable application entry identity for one applied configuration generation. */
typedef uint32_t cw_ec_entry_id;

typedef struct cw_ec_slave_summary {
	uint16_t position;
	uint16_t alias;
	uint32_t vendor_id;
	uint32_t product_code;
	uint32_t revision_number;
	uint32_t serial_number;
	uint8_t al_state;
	uint8_t error_flag;
	char name[CW_EC_SLAVE_NAME_LEN];
} cw_ec_slave_summary;

/* ---- open / negotiate / close ---- */

/*
 * Open device_path (or /dev/cw_ethercat0 when NULL). Claims the transport
 * control fd and EtherLab master 0. Returns 0 and *out on success, or -errno.
 * EBUSY means another application already owns master 0.
 */
int cw_ec_open(const char *device_path, cw_ec_handle **out);

/* Close and free. Safe when h is NULL. Kernel gates outputs and releases the master. */
void cw_ec_close(cw_ec_handle *h);

int cw_ec_get_api_version(cw_ec_handle *h, struct cw_ec_api_version *v);
int cw_ec_get_capabilities(cw_ec_handle *h, struct cw_ec_capabilities *c);

/* Fail if major mismatches or kernel minor is older than min_minor. */
int cw_ec_require_api(cw_ec_handle *h, uint16_t major, uint16_t min_minor);

/* Underlying fd for poll/select/tests only. Do not close. */
int cw_ec_fd(const cw_ec_handle *h);

/* ---- discovery ---- */

int cw_ec_get_master_info(cw_ec_handle *h, struct cw_ec_master_info *info);
int cw_ec_get_slave_info(cw_ec_handle *h, uint16_t position,
			 struct cw_ec_slave_info *info);

/*
 * Fill summaries for positions 0..slave_count-1 up to cap entries.
 * *count receives the total slave_count from master info (may exceed cap).
 * Does not wait for scan_busy; the caller decides when the bus is ready.
 */
int cw_ec_list_slaves(cw_ec_handle *h, cw_ec_slave_summary *buf, size_t cap,
		      size_t *count);

/* ---- ordered setup SDOs (pre-activation) ---- */

int cw_ec_setup_begin(cw_ec_handle *h);
int cw_ec_setup_add_sdo(cw_ec_handle *h, const struct cw_ec_setup_sdo *sdo);
int cw_ec_setup_apply(cw_ec_handle *h, struct cw_ec_setup_apply *result);
int cw_ec_setup_reset(cw_ec_handle *h);
int cw_ec_sdo_upload(cw_ec_handle *h, struct cw_ec_sdo_upload *req);

/* ---- transactional configuration ---- */

int cw_ec_config_begin(cw_ec_handle *h);
int cw_ec_config_add_slave(cw_ec_handle *h,
			   const struct cw_ec_config_slave *slave);
int cw_ec_config_add_sync(cw_ec_handle *h, const struct cw_ec_config_sync *sync);
int cw_ec_config_add_pdo(cw_ec_handle *h, const struct cw_ec_config_pdo *pdo);
int cw_ec_config_add_entry(cw_ec_handle *h,
			   const struct cw_ec_config_entry *entry);
int cw_ec_config_add_dc(cw_ec_handle *h, const struct cw_ec_config_dc *dc);
int cw_ec_config_add_dc_policy(cw_ec_handle *h,
			       const struct cw_ec_config_dc_policy *policy);
int cw_ec_config_add_domain(cw_ec_handle *h,
			    const struct cw_ec_config_domain *domain);
int cw_ec_config_add_domain_assignment(
	cw_ec_handle *h, const struct cw_ec_config_domain_assignment *asgn);
int cw_ec_config_validate(cw_ec_handle *h, struct cw_ec_config_validate *result);
int cw_ec_config_apply(cw_ec_handle *h, struct cw_ec_config_apply *result);

/* ---- domains and entry offsets ---- */

int cw_ec_domain_create(cw_ec_handle *h, struct cw_ec_domain_create *req);
int cw_ec_get_entry_offset(cw_ec_handle *h, struct cw_ec_entry_offset *io);

/* ---- cyclic engine ---- */

int cw_ec_cycle_activate(cw_ec_handle *h, uint32_t period_ns, uint32_t flags,
			 struct cw_ec_cycle_activate *out);
int cw_ec_cycle_deactivate(cw_ec_handle *h, struct cw_ec_cycle_deactivate *out);
int cw_ec_cycle_status(cw_ec_handle *h, struct cw_ec_cycle_status *st);
int cw_ec_cycle_wait(cw_ec_handle *h, struct cw_ec_cycle_wait *wait);
int cw_ec_cycle_info(cw_ec_handle *h, struct cw_ec_cycle_info *info);
int cw_ec_cycle_dc_info(cw_ec_handle *h, struct cw_ec_cycle_dc_info *info);
int cw_ec_cycle_set_period(cw_ec_handle *h,
			   struct cw_ec_cycle_period_update *upd);

/* ---- process images and output gate ---- */

/*
 * Read a coherent global input image into buf (capacity len).
 * On success, snap is filled and data_size bytes are written to buf.
 */
int cw_ec_get_input_snapshot(cw_ec_handle *h, struct cw_ec_input_snapshot *snap,
			     void *buf, size_t len);

/*
 * Publish full output image and update mask of size len.
 * Does not arm. On success, pub receives the assigned output_sequence.
 */
int cw_ec_publish_output(cw_ec_handle *h, const void *image, const void *mask,
			 size_t len, struct cw_ec_output_publish *pub);

int cw_ec_arm_output(cw_ec_handle *h, struct cw_ec_output_arm *arm);
int cw_ec_disarm_output(cw_ec_handle *h, struct cw_ec_output_disarm *disarm);
int cw_ec_get_io_status(cw_ec_handle *h, struct cw_ec_io_status *st);

/* Optional output lease (API 0.14+ / CAP_OUTPUT_LEASE). */
int cw_ec_configure_output_lease(cw_ec_handle *h,
				 struct cw_ec_output_lease_config *cfg);
int cw_ec_renew_output_lease(cw_ec_handle *h,
			     struct cw_ec_output_lease_renew *renew);
int cw_ec_get_output_lease_status(cw_ec_handle *h,
				  struct cw_ec_output_lease_status *st);

/* Optional input history ring (API 0.16+ / CAP_INPUT_HISTORY). */
int cw_ec_configure_input_history(cw_ec_handle *h,
				  struct cw_ec_input_history_config *cfg);
int cw_ec_get_input_history_batch(cw_ec_handle *h,
				  struct cw_ec_input_history_batch *batch);

/* ---- status helpers ---- */

int cw_ec_get_config_slave_status(cw_ec_handle *h,
				  struct cw_ec_config_slave_status *st);
int cw_ec_get_domain_status(cw_ec_handle *h, struct cw_ec_domain_status *st);
int cw_ec_get_dc_status(cw_ec_handle *h, struct cw_ec_dc_status *st);

/* ---- policy-free convenience builders ---- */

/* Example pure ID scheme: (slave_config_id << 16) | local_index. */
cw_ec_entry_id cw_ec_make_entry_id(uint32_t slave_config_id,
				   uint16_t local_index);

int cw_ec_fill_config_slave(struct cw_ec_config_slave *out, uint32_t config_id,
			    uint16_t position, uint16_t alias,
			    uint32_t vendor_id, uint32_t product_code,
			    uint32_t revision_number, uint32_t flags);

/*
 * Zero a UAPI structure and set struct_size + api_major for the common header.
 * struct_size must be sizeof(*obj). Safe no-op helpers for callers that build
 * records before passing them into add_* functions.
 */
void cw_ec_init_api_header(void *obj, size_t struct_size);

#ifdef __cplusplus
}
#endif

#endif /* CW_ETHERCAT_H */
