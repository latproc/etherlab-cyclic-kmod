/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Public C API for libelcethercat — thin, policy-free userspace client for
 * the elc_ethercat kernel transport UAPI.
 *
 * Normative wire structures and ioctls: elc_ethercat_uapi.h
 * Design contract: docs/libelcethercat.md
 *
 * Licensed under LGPL-2.1-or-later (see LICENSE.LGPL-2.1) so non-public
 * applications may link libelcethercat without adopting GPL for their own
 * code. The kernel module remains GPL-2.0-only.
 */

#ifndef ELC_ETHERCAT_H
#define ELC_ETHERCAT_H

#include "elc_ethercat_uapi.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Opaque control-device owner. One handle maps to one exclusive
 * /dev/elc_ethercat* file descriptor (EtherLab master 0).
 * Not thread-safe unless the caller serialises all use of a given handle.
 */
typedef struct elc_handle elc_handle;

/* Stable application entry identity for one applied configuration generation. */
typedef uint32_t elc_entry_id;

typedef struct elc_slave_summary {
	uint16_t position;
	uint16_t alias;
	uint32_t vendor_id;
	uint32_t product_code;
	uint32_t revision_number;
	uint32_t serial_number;
	uint8_t al_state;
	uint8_t error_flag;
	char name[ELC_SLAVE_NAME_LEN];
} elc_slave_summary;

/* ---- open / negotiate / close ---- */

/*
 * Open device_path (or /dev/elc_ethercat0 when NULL). Claims the transport
 * control fd and EtherLab master 0. Returns 0 and *out on success, or -errno.
 * EBUSY means another application already owns master 0.
 */
int elc_open(const char *device_path, elc_handle **out);

/* Close and free. Safe when h is NULL. Kernel gates outputs and releases the master. */
void elc_close(elc_handle *h);

int elc_get_api_version(elc_handle *h, struct elc_api_version *v);
int elc_get_capabilities(elc_handle *h, struct elc_capabilities *c);

/* Fail if major mismatches or kernel minor is older than min_minor. */
int elc_require_api(elc_handle *h, uint16_t major, uint16_t min_minor);

/* Underlying fd for poll/select/tests only. Do not close. */
int elc_fd(const elc_handle *h);

/* ---- discovery ---- */

int elc_get_master_info(elc_handle *h, struct elc_master_info *info);
int elc_get_slave_info(elc_handle *h, uint16_t position,
			 struct elc_slave_info *info);

/*
 * Fill summaries for positions 0..slave_count-1 up to cap entries.
 * *count receives the total slave_count from master info (may exceed cap).
 * Does not wait for scan_busy; the caller decides when the bus is ready.
 */
int elc_list_slaves(elc_handle *h, elc_slave_summary *buf, size_t cap,
		      size_t *count);

/* ---- ordered setup SDOs (pre-activation) ---- */

int elc_setup_begin(elc_handle *h);
int elc_setup_add_sdo(elc_handle *h, const struct elc_setup_sdo *sdo);
int elc_setup_apply(elc_handle *h, struct elc_setup_apply *result);
int elc_setup_reset(elc_handle *h);
int elc_sdo_upload(elc_handle *h, struct elc_sdo_upload *req);

/* ---- transactional configuration ---- */

int elc_config_begin(elc_handle *h);
int elc_config_add_slave(elc_handle *h,
			   const struct elc_config_slave *slave);
int elc_config_add_sync(elc_handle *h, const struct elc_config_sync *sync);
int elc_config_add_pdo(elc_handle *h, const struct elc_config_pdo *pdo);
int elc_config_add_entry(elc_handle *h,
			   const struct elc_config_entry *entry);
int elc_config_add_dc(elc_handle *h, const struct elc_config_dc *dc);
int elc_config_add_dc_policy(elc_handle *h,
			       const struct elc_config_dc_policy *policy);
int elc_config_add_domain(elc_handle *h,
			    const struct elc_config_domain *domain);
int elc_config_add_domain_assignment(
	elc_handle *h, const struct elc_config_domain_assignment *asgn);
int elc_config_validate(elc_handle *h, struct elc_config_validate *result);
int elc_config_apply(elc_handle *h, struct elc_config_apply *result);

/* ---- domains and entry offsets ---- */

int elc_domain_create(elc_handle *h, struct elc_domain_create *req);
int elc_get_entry_offset(elc_handle *h, struct elc_entry_offset *io);

/* ---- cyclic engine ---- */

int elc_cycle_activate(elc_handle *h, uint32_t period_ns, uint32_t flags,
			 struct elc_cycle_activate *out);
int elc_cycle_deactivate(elc_handle *h, struct elc_cycle_deactivate *out);
int elc_cycle_status(elc_handle *h, struct elc_cycle_status *st);
int elc_cycle_wait(elc_handle *h, struct elc_cycle_wait *wait);
int elc_cycle_info(elc_handle *h, struct elc_cycle_info *info);
int elc_cycle_dc_info(elc_handle *h, struct elc_cycle_dc_info *info);
int elc_cycle_set_period(elc_handle *h,
			   struct elc_cycle_period_update *upd);

/* ---- process images and output gate ---- */

/*
 * Read a coherent global input image into buf (capacity len).
 * On success, snap is filled and data_size bytes are written to buf.
 */
int elc_get_input_snapshot(elc_handle *h, struct elc_input_snapshot *snap,
			     void *buf, size_t len);

/*
 * Publish output image and update mask of size len. Does not arm.
 * On success, pub receives the assigned output_sequence.
 *
 * API 0.17+: if pub is non-NULL, flags and domain_config_id are preserved.
 * domain_config_id == 0 publishes the full global image (len must match
 * global size); non-zero targets that domain segment (len must match the
 * domain size). arm->flags / disarm->flags use the same 0 = all /
 * non-zero = domain_config_id convention.
 */
int elc_publish_output(elc_handle *h, const void *image, const void *mask,
			 size_t len, struct elc_output_publish *pub);

int elc_arm_output(elc_handle *h, struct elc_output_arm *arm);
int elc_disarm_output(elc_handle *h, struct elc_output_disarm *disarm);
int elc_get_io_status(elc_handle *h, struct elc_io_status *st);

/* Optional output lease (API 0.14+ / CAP_OUTPUT_LEASE). */
int elc_configure_output_lease(elc_handle *h,
				 struct elc_output_lease_config *cfg);
int elc_renew_output_lease(elc_handle *h,
			     struct elc_output_lease_renew *renew);
int elc_get_output_lease_status(elc_handle *h,
				  struct elc_output_lease_status *st);

/* Optional input history ring (API 0.16+ / CAP_INPUT_HISTORY). */
int elc_configure_input_history(elc_handle *h,
				  struct elc_input_history_config *cfg);
int elc_get_input_history_batch(elc_handle *h,
				  struct elc_input_history_batch *batch);

/* ---- status helpers ---- */

int elc_get_config_slave_status(elc_handle *h,
				  struct elc_config_slave_status *st);
int elc_get_domain_status(elc_handle *h, struct elc_domain_status *st);
int elc_get_dc_status(elc_handle *h, struct elc_dc_status *st);

/* ---- policy-free convenience builders ---- */

/* Example pure ID scheme: (slave_config_id << 16) | local_index. */
elc_entry_id elc_make_entry_id(uint32_t slave_config_id,
				   uint16_t local_index);

int elc_fill_config_slave(struct elc_config_slave *out, uint32_t config_id,
			    uint16_t position, uint16_t alias,
			    uint32_t vendor_id, uint32_t product_code,
			    uint32_t revision_number, uint32_t flags);

/*
 * Zero a UAPI structure and set struct_size + api_major for the common header.
 * struct_size must be sizeof(*obj). Safe no-op helpers for callers that build
 * records before passing them into add_* functions.
 */
void elc_init_api_header(void *obj, size_t struct_size);

#ifdef __cplusplus
}
#endif

#endif /* ELC_ETHERCAT_H */
