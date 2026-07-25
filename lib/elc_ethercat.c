// SPDX-License-Identifier: GPL-2.0-only
/*
 * libelcethercat — thin ioctl client for the elc_ethercat transport.
 * No machine policy, XML, or product identities.
 */

#define _GNU_SOURCE

#include "elc_ethercat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

struct elc_handle {
	int fd;
	struct elc_api_version version;
};

static int elc_ioctl(elc_handle *h, unsigned long request, void *arg)
{
	if (!h || h->fd < 0)
		return -EINVAL;
	if (ioctl(h->fd, request, arg) < 0)
		return -errno;
	return 0;
}

static int elc_check_ptr(const void *p)
{
	return p ? 0 : -EINVAL;
}

void elc_init_api_header(void *obj, size_t struct_size)
{
	struct {
		uint16_t struct_size;
		uint16_t api_major;
	} *hdr;

	if (!obj || struct_size < sizeof(*hdr))
		return;
	memset(obj, 0, struct_size);
	hdr = obj;
	hdr->struct_size = (uint16_t)struct_size;
	hdr->api_major = ELC_API_VERSION_MAJOR;
}

int elc_open(const char *device_path, elc_handle **out)
{
	const char *path = device_path ? device_path : "/dev/elc_ethercat0";
	elc_handle *h;
	int fd;
	int ret;

	if (!out)
		return -EINVAL;

	*out = NULL;
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -errno;

	h = calloc(1, sizeof(*h));
	if (!h) {
		close(fd);
		return -ENOMEM;
	}
	h->fd = fd;

	memset(&h->version, 0, sizeof(h->version));
	ret = elc_ioctl(h, ELC_IOC_GET_API_VERSION, &h->version);
	if (ret) {
		elc_close(h);
		return ret;
	}
	if (h->version.struct_size != sizeof(h->version) ||
	    h->version.major != ELC_API_VERSION_MAJOR) {
		elc_close(h);
		return -EPROTO;
	}

	*out = h;
	return 0;
}

void elc_close(elc_handle *h)
{
	if (!h)
		return;
	if (h->fd >= 0)
		close(h->fd);
	h->fd = -1;
	free(h);
}

int elc_get_api_version(elc_handle *h, struct elc_api_version *v)
{
	int ret;

	if ((ret = elc_check_ptr(v)))
		return ret;
	memset(v, 0, sizeof(*v));
	ret = elc_ioctl(h, ELC_IOC_GET_API_VERSION, v);
	if (ret)
		return ret;
	if (h)
		h->version = *v;
	return 0;
}

int elc_get_capabilities(elc_handle *h, struct elc_capabilities *c)
{
	int ret;

	if ((ret = elc_check_ptr(c)))
		return ret;
	elc_init_api_header(c, sizeof(*c));
	return elc_ioctl(h, ELC_IOC_GET_CAPABILITIES, c);
}

int elc_require_api(elc_handle *h, uint16_t major, uint16_t min_minor)
{
	struct elc_api_version v;
	int ret;

	if (!h)
		return -EINVAL;
	ret = elc_get_api_version(h, &v);
	if (ret)
		return ret;
	if (v.major != major)
		return -EPROTO;
	if (v.minor < min_minor)
		return -EPROTONOSUPPORT;
	return 0;
}

int elc_fd(const elc_handle *h)
{
	if (!h || h->fd < 0)
		return -EINVAL;
	return h->fd;
}

int elc_get_master_info(elc_handle *h, struct elc_master_info *info)
{
	int ret;

	if ((ret = elc_check_ptr(info)))
		return ret;
	elc_init_api_header(info, sizeof(*info));
	return elc_ioctl(h, ELC_IOC_GET_MASTER_INFO, info);
}

int elc_get_slave_info(elc_handle *h, uint16_t position,
			 struct elc_slave_info *info)
{
	int ret;

	if ((ret = elc_check_ptr(info)))
		return ret;
	elc_init_api_header(info, sizeof(*info));
	info->position = position;
	return elc_ioctl(h, ELC_IOC_GET_SLAVE_INFO, info);
}

int elc_list_slaves(elc_handle *h, elc_slave_summary *buf, size_t cap,
		      size_t *count)
{
	struct elc_master_info master;
	size_t i;
	size_t n;
	int ret;

	if (!count)
		return -EINVAL;
	if (cap && !buf)
		return -EINVAL;

	ret = elc_get_master_info(h, &master);
	if (ret)
		return ret;

	*count = master.slave_count;
	n = master.slave_count;
	if (n > cap)
		n = cap;

	for (i = 0; i < n; i++) {
		struct elc_slave_info slave;

		ret = elc_get_slave_info(h, (uint16_t)i, &slave);
		if (ret)
			return ret;
		buf[i].position = slave.position;
		buf[i].alias = slave.alias;
		buf[i].vendor_id = slave.vendor_id;
		buf[i].product_code = slave.product_code;
		buf[i].revision_number = slave.revision_number;
		buf[i].serial_number = slave.serial_number;
		buf[i].al_state = slave.al_state;
		buf[i].error_flag = slave.error_flag;
		memcpy(buf[i].name, slave.name, sizeof(buf[i].name));
	}
	return 0;
}

int elc_setup_begin(elc_handle *h)
{
	struct elc_setup_begin req;

	elc_init_api_header(&req, sizeof(req));
	return elc_ioctl(h, ELC_IOC_SETUP_BEGIN, &req);
}

int elc_setup_add_sdo(elc_handle *h, const struct elc_setup_sdo *sdo)
{
	struct elc_setup_sdo copy;
	int ret;

	if ((ret = elc_check_ptr(sdo)))
		return ret;
	copy = *sdo;
	if (!copy.struct_size)
		copy.struct_size = sizeof(copy);
	if (!copy.api_major)
		copy.api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_SETUP_ADD_SDO, &copy);
}

int elc_setup_apply(elc_handle *h, struct elc_setup_apply *result)
{
	int ret;

	if ((ret = elc_check_ptr(result)))
		return ret;
	elc_init_api_header(result, sizeof(*result));
	return elc_ioctl(h, ELC_IOC_SETUP_APPLY, result);
}

int elc_setup_reset(elc_handle *h)
{
	struct elc_setup_begin req;

	elc_init_api_header(&req, sizeof(req));
	return elc_ioctl(h, ELC_IOC_SETUP_RESET, &req);
}

int elc_sdo_upload(elc_handle *h, struct elc_sdo_upload *req)
{
	int ret;

	if ((ret = elc_check_ptr(req)))
		return ret;
	if (!req->struct_size)
		req->struct_size = sizeof(*req);
	if (!req->api_major)
		req->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_SDO_UPLOAD, req);
}

int elc_config_begin(elc_handle *h)
{
	struct elc_config_begin req;

	elc_init_api_header(&req, sizeof(req));
	return elc_ioctl(h, ELC_IOC_CONFIG_BEGIN, &req);
}

#define ELC_CONFIG_ADD_IMPL(name, type, ioc)                                 \
	int elc_##name(elc_handle *h, const type *obj)                     \
	{                                                                      \
		type copy;                                                     \
		int ret;                                                       \
                                                                               \
		if ((ret = elc_check_ptr(obj)))                              \
			return ret;                                            \
		copy = *obj;                                                   \
		if (!copy.struct_size)                                         \
			copy.struct_size = sizeof(copy);                       \
		if (!copy.api_major)                                           \
			copy.api_major = ELC_API_VERSION_MAJOR;              \
		return elc_ioctl(h, ioc, &copy);                             \
	}

ELC_CONFIG_ADD_IMPL(config_add_slave, struct elc_config_slave,
		      ELC_IOC_CONFIG_ADD_SLAVE)
ELC_CONFIG_ADD_IMPL(config_add_sync, struct elc_config_sync,
		      ELC_IOC_CONFIG_ADD_SYNC)
ELC_CONFIG_ADD_IMPL(config_add_pdo, struct elc_config_pdo,
		      ELC_IOC_CONFIG_ADD_PDO)
ELC_CONFIG_ADD_IMPL(config_add_entry, struct elc_config_entry,
		      ELC_IOC_CONFIG_ADD_ENTRY)
ELC_CONFIG_ADD_IMPL(config_add_dc, struct elc_config_dc,
		      ELC_IOC_CONFIG_ADD_DC)
ELC_CONFIG_ADD_IMPL(config_add_dc_policy, struct elc_config_dc_policy,
		      ELC_IOC_CONFIG_SET_DC_POLICY)
ELC_CONFIG_ADD_IMPL(config_add_domain, struct elc_config_domain,
		      ELC_IOC_CONFIG_ADD_DOMAIN)
ELC_CONFIG_ADD_IMPL(config_add_domain_assignment,
		      struct elc_config_domain_assignment,
		      ELC_IOC_CONFIG_ASSIGN_DOMAIN)

int elc_config_validate(elc_handle *h, struct elc_config_validate *result)
{
	int ret;

	if ((ret = elc_check_ptr(result)))
		return ret;
	elc_init_api_header(result, sizeof(*result));
	return elc_ioctl(h, ELC_IOC_CONFIG_VALIDATE, result);
}

int elc_config_apply(elc_handle *h, struct elc_config_apply *result)
{
	int ret;

	if ((ret = elc_check_ptr(result)))
		return ret;
	elc_init_api_header(result, sizeof(*result));
	return elc_ioctl(h, ELC_IOC_CONFIG_APPLY, result);
}

int elc_domain_create(elc_handle *h, struct elc_domain_create *req)
{
	int ret;

	if ((ret = elc_check_ptr(req)))
		return ret;
	elc_init_api_header(req, sizeof(*req));
	return elc_ioctl(h, ELC_IOC_DOMAIN_CREATE, req);
}

int elc_get_entry_offset(elc_handle *h, struct elc_entry_offset *io)
{
	int ret;

	if ((ret = elc_check_ptr(io)))
		return ret;
	if (!io->struct_size)
		io->struct_size = sizeof(*io);
	if (!io->api_major)
		io->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_GET_ENTRY_OFFSET, io);
}

int elc_cycle_activate(elc_handle *h, uint32_t period_ns, uint32_t flags,
			 struct elc_cycle_activate *out)
{
	struct elc_cycle_activate local;
	struct elc_cycle_activate *req = out ? out : &local;
	int ret;

	elc_init_api_header(req, sizeof(*req));
	req->cycle_period_ns = period_ns;
	req->flags = flags;
	ret = elc_ioctl(h, ELC_IOC_CYCLE_ACTIVATE, req);
	if (ret)
		return ret;
	if (req->result < 0)
		return req->result;
	return 0;
}

int elc_cycle_deactivate(elc_handle *h, struct elc_cycle_deactivate *out)
{
	struct elc_cycle_deactivate local;
	struct elc_cycle_deactivate *req = out ? out : &local;
	int ret;

	elc_init_api_header(req, sizeof(*req));
	ret = elc_ioctl(h, ELC_IOC_CYCLE_DEACTIVATE, req);
	if (ret)
		return ret;
	if (req->result < 0)
		return req->result;
	return 0;
}

int elc_cycle_status(elc_handle *h, struct elc_cycle_status *st)
{
	int ret;

	if ((ret = elc_check_ptr(st)))
		return ret;
	elc_init_api_header(st, sizeof(*st));
	return elc_ioctl(h, ELC_IOC_CYCLE_GET_STATUS, st);
}

int elc_cycle_wait(elc_handle *h, struct elc_cycle_wait *wait)
{
	int ret;

	if ((ret = elc_check_ptr(wait)))
		return ret;
	if (!wait->struct_size)
		wait->struct_size = sizeof(*wait);
	if (!wait->api_major)
		wait->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_CYCLE_WAIT, wait);
}

int elc_cycle_info(elc_handle *h, struct elc_cycle_info *info)
{
	int ret;

	if ((ret = elc_check_ptr(info)))
		return ret;
	elc_init_api_header(info, sizeof(*info));
	return elc_ioctl(h, ELC_IOC_CYCLE_GET_INFO, info);
}

int elc_cycle_dc_info(elc_handle *h, struct elc_cycle_dc_info *info)
{
	int ret;

	if ((ret = elc_check_ptr(info)))
		return ret;
	elc_init_api_header(info, sizeof(*info));
	return elc_ioctl(h, ELC_IOC_CYCLE_GET_DC_INFO, info);
}

int elc_cycle_set_period(elc_handle *h,
			   struct elc_cycle_period_update *upd)
{
	int ret;

	if ((ret = elc_check_ptr(upd)))
		return ret;
	if (!upd->struct_size)
		upd->struct_size = sizeof(*upd);
	if (!upd->api_major)
		upd->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_CYCLE_SET_PERIOD, upd);
}

int elc_get_input_snapshot(elc_handle *h, struct elc_input_snapshot *snap,
			     void *buf, size_t len)
{
	uint64_t generation = 0;
	uint32_t flags = 0;
	int ret;

	if ((ret = elc_check_ptr(snap)))
		return ret;
	if (!buf || !len || len > UINT32_MAX)
		return -EINVAL;

	/*
	 * Preserve caller-supplied generation/flags when present; otherwise
	 * bind to the current active generation via IO status.
	 */
	if (snap->config_generation)
		generation = snap->config_generation;
	flags = snap->flags;
	if (!generation) {
		struct elc_io_status io;

		ret = elc_get_io_status(h, &io);
		if (ret)
			return ret;
		generation = io.config_generation;
	}

	elc_init_api_header(snap, sizeof(*snap));
	snap->flags = flags;
	snap->data_ptr = (uint64_t)(uintptr_t)buf;
	snap->data_capacity = (uint32_t)len;
	snap->config_generation = generation;
	return elc_ioctl(h, ELC_IOC_GET_INPUT_SNAPSHOT, snap);
}

int elc_publish_output(elc_handle *h, const void *image, const void *mask,
			 size_t len, struct elc_output_publish *pub)
{
	struct elc_output_publish local;
	struct elc_output_publish *req = pub ? pub : &local;
	uint64_t generation = 0;
	uint32_t flags = 0;
	uint32_t domain_config_id = 0;
	int ret;

	if (!image || !mask || !len || len > UINT32_MAX)
		return -EINVAL;

	if (pub) {
		flags = pub->flags;
		domain_config_id = pub->domain_config_id;
	}

	if (pub && pub->config_generation)
		generation = pub->config_generation;
	else {
		struct elc_io_status io;

		ret = elc_get_io_status(h, &io);
		if (ret)
			return ret;
		generation = io.config_generation;
	}

	elc_init_api_header(req, sizeof(*req));
	req->flags = flags;
	req->domain_config_id = domain_config_id;
	req->data_ptr = (uint64_t)(uintptr_t)image;
	req->mask_ptr = (uint64_t)(uintptr_t)mask;
	req->data_size = (uint32_t)len;
	req->config_generation = generation;
	return elc_ioctl(h, ELC_IOC_PUBLISH_OUTPUT, req);
}

int elc_arm_output(elc_handle *h, struct elc_output_arm *arm)
{
	int ret;

	if ((ret = elc_check_ptr(arm)))
		return ret;
	if (!arm->struct_size)
		arm->struct_size = sizeof(*arm);
	if (!arm->api_major)
		arm->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_ARM_OUTPUTS, arm);
}

int elc_disarm_output(elc_handle *h, struct elc_output_disarm *disarm)
{
	int ret;

	if ((ret = elc_check_ptr(disarm)))
		return ret;
	if (!disarm->struct_size)
		disarm->struct_size = sizeof(*disarm);
	if (!disarm->api_major)
		disarm->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_DISARM_OUTPUTS, disarm);
}

int elc_get_io_status(elc_handle *h, struct elc_io_status *st)
{
	int ret;

	if ((ret = elc_check_ptr(st)))
		return ret;
	elc_init_api_header(st, sizeof(*st));
	return elc_ioctl(h, ELC_IOC_GET_IO_STATUS, st);
}

int elc_configure_output_lease(elc_handle *h,
				 struct elc_output_lease_config *cfg)
{
	int ret;

	if ((ret = elc_check_ptr(cfg)))
		return ret;
	if (!cfg->struct_size)
		cfg->struct_size = sizeof(*cfg);
	if (!cfg->api_major)
		cfg->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_CONFIGURE_OUTPUT_LEASE, cfg);
}

int elc_renew_output_lease(elc_handle *h,
			     struct elc_output_lease_renew *renew)
{
	int ret;

	if ((ret = elc_check_ptr(renew)))
		return ret;
	if (!renew->struct_size)
		renew->struct_size = sizeof(*renew);
	if (!renew->api_major)
		renew->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_RENEW_OUTPUT_LEASE, renew);
}

int elc_get_output_lease_status(elc_handle *h,
				  struct elc_output_lease_status *st)
{
	int ret;

	if ((ret = elc_check_ptr(st)))
		return ret;
	elc_init_api_header(st, sizeof(*st));
	return elc_ioctl(h, ELC_IOC_GET_OUTPUT_LEASE_STATUS, st);
}

int elc_configure_input_history(elc_handle *h,
				  struct elc_input_history_config *cfg)
{
	int ret;

	if ((ret = elc_check_ptr(cfg)))
		return ret;
	if (!cfg->struct_size)
		cfg->struct_size = sizeof(*cfg);
	if (!cfg->api_major)
		cfg->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_CONFIGURE_INPUT_HISTORY, cfg);
}

int elc_get_input_history_batch(elc_handle *h,
				  struct elc_input_history_batch *batch)
{
	int ret;

	if ((ret = elc_check_ptr(batch)))
		return ret;
	if (!batch->struct_size)
		batch->struct_size = sizeof(*batch);
	if (!batch->api_major)
		batch->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_GET_INPUT_HISTORY_BATCH, batch);
}

int elc_get_config_slave_status(elc_handle *h,
				  struct elc_config_slave_status *st)
{
	int ret;

	if ((ret = elc_check_ptr(st)))
		return ret;
	if (!st->struct_size)
		st->struct_size = sizeof(*st);
	if (!st->api_major)
		st->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_GET_CONFIG_SLAVE_STATUS, st);
}

int elc_get_domain_status(elc_handle *h, struct elc_domain_status *st)
{
	int ret;

	if ((ret = elc_check_ptr(st)))
		return ret;
	if (!st->struct_size)
		st->struct_size = sizeof(*st);
	if (!st->api_major)
		st->api_major = ELC_API_VERSION_MAJOR;
	return elc_ioctl(h, ELC_IOC_GET_DOMAIN_STATUS, st);
}

int elc_get_dc_status(elc_handle *h, struct elc_dc_status *st)
{
	int ret;

	if ((ret = elc_check_ptr(st)))
		return ret;
	elc_init_api_header(st, sizeof(*st));
	return elc_ioctl(h, ELC_IOC_CYCLE_GET_DC_STATUS, st);
}

elc_entry_id elc_make_entry_id(uint32_t slave_config_id,
				   uint16_t local_index)
{
	return ((elc_entry_id)slave_config_id << 16) |
	       (elc_entry_id)local_index;
}

int elc_fill_config_slave(struct elc_config_slave *out, uint32_t config_id,
			    uint16_t position, uint16_t alias,
			    uint32_t vendor_id, uint32_t product_code,
			    uint32_t revision_number, uint32_t flags)
{
	if (!out)
		return -EINVAL;
	elc_init_api_header(out, sizeof(*out));
	out->config_id = config_id;
	out->position = position;
	out->alias = alias;
	out->vendor_id = vendor_id;
	out->product_code = product_code;
	out->revision_number = revision_number;
	out->flags = flags;
	return 0;
}
