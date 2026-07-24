// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <ecrt.h>

#include "cw_ethercat_uapi.h"

#define CW_EC_NAME "cw_ethercat"

struct cw_ec_file {
	ec_master_t *master;
	struct mutex lock;
	struct list_head setup_sdos;
	u32 setup_count;
	u32 setup_bytes;
	u32 last_sequence;
	bool setup_started;
	bool setup_applied;
	struct list_head config_slaves;
	struct list_head config_syncs;
	struct list_head config_pdos;
	struct list_head config_entries;
	u32 config_slave_count;
	u32 config_sync_count;
	u32 config_pdo_count;
	u32 config_entry_count;
	bool config_started;
	bool config_validated;
};

struct cw_ec_setup_entry {
	struct list_head node;
	u32 sequence;
	u16 position;
	u16 index;
	u8 subindex;
	u8 type;
	u16 data_len;
	u8 data[];
};

struct cw_ec_config_node {
	struct list_head node;
	u32 config_id;
};

struct cw_ec_slave_node {
	struct cw_ec_config_node common;
	struct cw_ec_config_slave cfg;
};

struct cw_ec_sync_node {
	struct cw_ec_config_node common;
	struct cw_ec_config_sync cfg;
};

struct cw_ec_pdo_node {
	struct cw_ec_config_node common;
	struct cw_ec_config_pdo cfg;
};

struct cw_ec_entry_node {
	struct cw_ec_config_node common;
	struct cw_ec_config_entry cfg;
};

static atomic_t cw_ec_control_open = ATOMIC_INIT(0);

static int cw_ec_check_header(u16 struct_size, u16 api_major,
			      size_t expected_size);

static void cw_ec_setup_clear(struct cw_ec_file *ctx)
{
	struct cw_ec_setup_entry *entry;
	struct cw_ec_setup_entry *next;

	list_for_each_entry_safe(entry, next, &ctx->setup_sdos, node) {
		list_del(&entry->node);
		kfree(entry);
	}
	ctx->setup_count = 0;
	ctx->setup_bytes = 0;
	ctx->last_sequence = 0;
	ctx->setup_started = false;
	ctx->setup_applied = false;
}

static void cw_ec_config_clear(struct cw_ec_file *ctx)
{
	struct cw_ec_config_node *entry;
	struct cw_ec_config_node *next;

#define CW_EC_CLEAR_CONFIG_LIST(name) \
	list_for_each_entry_safe(entry, next, &ctx->name, node) { \
		list_del(&entry->node); \
		kfree(entry); \
	}

	CW_EC_CLEAR_CONFIG_LIST(config_entries);
	CW_EC_CLEAR_CONFIG_LIST(config_pdos);
	CW_EC_CLEAR_CONFIG_LIST(config_syncs);
	CW_EC_CLEAR_CONFIG_LIST(config_slaves);
#undef CW_EC_CLEAR_CONFIG_LIST

	ctx->config_slave_count = 0;
	ctx->config_sync_count = 0;
	ctx->config_pdo_count = 0;
	ctx->config_entry_count = 0;
	ctx->config_started = false;
	ctx->config_validated = false;
}

static int cw_ec_open(struct inode *inode, struct file *file)
{
	struct cw_ec_file *ctx;
	ec_master_t *master;

	if (atomic_cmpxchg(&cw_ec_control_open, 0, 1) != 0)
		return -EBUSY;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		atomic_set(&cw_ec_control_open, 0);
		return -ENOMEM;
	}

	master = ecrt_request_master(0);
	if (!master) {
		kfree(ctx);
		atomic_set(&cw_ec_control_open, 0);
		return -EBUSY;
	}

	ctx->master = master;
	mutex_init(&ctx->lock);
	INIT_LIST_HEAD(&ctx->setup_sdos);
	INIT_LIST_HEAD(&ctx->config_slaves);
	INIT_LIST_HEAD(&ctx->config_syncs);
	INIT_LIST_HEAD(&ctx->config_pdos);
	INIT_LIST_HEAD(&ctx->config_entries);
	file->private_data = ctx;
	nonseekable_open(inode, file);

	pr_info(CW_EC_NAME ": control owner acquired master 0\n");
	return 0;
}

static int cw_ec_release(struct inode *inode, struct file *file)
{
	struct cw_ec_file *ctx = file->private_data;

	if (ctx) {
		mutex_lock(&ctx->lock);
		cw_ec_setup_clear(ctx);
		cw_ec_config_clear(ctx);
		if (ctx->master) {
			ecrt_release_master(ctx->master);
			ctx->master = NULL;
		}
		mutex_unlock(&ctx->lock);
		kfree(ctx);
		file->private_data = NULL;
	}

	atomic_set(&cw_ec_control_open, 0);
	pr_info(CW_EC_NAME ": control owner released master 0\n");
	return 0;
}

static bool cw_ec_config_id_exists(struct list_head *head, u32 config_id)
{
	struct cw_ec_config_node *entry;

	list_for_each_entry(entry, head, node) {
		if (entry->config_id == config_id)
			return true;
	}
	return false;
}

static long cw_ec_config_begin(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_config_begin request;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = cw_ec_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (request.reserved)
		return -EINVAL;

	mutex_lock(&ctx->lock);
	cw_ec_config_clear(ctx);
	ctx->config_started = true;
	mutex_unlock(&ctx->lock);
	return 0;
}

static long cw_ec_config_add_slave(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_slave_node *node;
	int ret;

	node = kzalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
		return -ENOMEM;
	if (copy_from_user(&node->cfg, argp, sizeof(node->cfg))) {
		ret = -EFAULT;
		goto out;
	}
	ret = cw_ec_check_header(node->cfg.struct_size, node->cfg.api_major,
				 sizeof(node->cfg));
	if (ret)
		goto out;
	if (!node->cfg.config_id || !node->cfg.vendor_id ||
	    !node->cfg.product_code || node->cfg.flags) {
		ret = -EINVAL;
		goto out;
	}
	node->common.config_id = node->cfg.config_id;

	mutex_lock(&ctx->lock);
	if (!ctx->config_started || ctx->config_validated) {
		ret = -EINVAL;
	} else if (ctx->config_slave_count >= CW_EC_CONFIG_SLAVE_MAX) {
		ret = -E2BIG;
	} else if (cw_ec_config_id_exists(&ctx->config_slaves,
					  node->cfg.config_id)) {
		ret = -EEXIST;
	} else {
		list_add_tail(&node->common.node, &ctx->config_slaves);
		ctx->config_slave_count++;
		node = NULL;
		ret = 0;
	}
	mutex_unlock(&ctx->lock);
out:
	kfree(node);
	return ret;
}

#define CW_EC_CONFIG_ADD_CHILD(function_name, node_type, cfg_member, list_name, \
			       count_name, max_count, validate_expr) \
static long function_name(struct cw_ec_file *ctx, void __user *argp) \
{ \
	struct node_type *node; \
	int ret; \
	node = kzalloc(sizeof(*node), GFP_KERNEL); \
	if (!node) \
		return -ENOMEM; \
	if (copy_from_user(&node->cfg_member, argp, sizeof(node->cfg_member))) { \
		ret = -EFAULT; \
		goto out; \
	} \
	ret = cw_ec_check_header(node->cfg_member.struct_size, \
				 node->cfg_member.api_major, \
				 sizeof(node->cfg_member)); \
	if (ret) \
		goto out; \
	if (!node->cfg_member.config_id || (validate_expr)) { \
		ret = -EINVAL; \
		goto out; \
	} \
	node->common.config_id = node->cfg_member.config_id; \
	mutex_lock(&ctx->lock); \
	if (!ctx->config_started || ctx->config_validated) \
		ret = -EINVAL; \
	else if (ctx->count_name >= (max_count)) \
		ret = -E2BIG; \
	else if (cw_ec_config_id_exists(&ctx->list_name, \
					 node->cfg_member.config_id)) \
		ret = -EEXIST; \
	else { \
		list_add_tail(&node->common.node, &ctx->list_name); \
		ctx->count_name++; \
		node = NULL; \
		ret = 0; \
	} \
	mutex_unlock(&ctx->lock); \
out: \
	kfree(node); \
	return ret; \
}

CW_EC_CONFIG_ADD_CHILD(cw_ec_config_add_sync, cw_ec_sync_node, cfg,
		       config_syncs, config_sync_count, CW_EC_CONFIG_SYNC_MAX,
		       !node->cfg.slave_config_id ||
		       node->cfg.sync_index >= EC_MAX_SYNC_MANAGERS ||
		       (node->cfg.direction != CW_EC_DIR_OUTPUT &&
			node->cfg.direction != CW_EC_DIR_INPUT) ||
		       node->cfg.watchdog_mode > CW_EC_WD_DISABLE ||
		       node->cfg.reserved)

CW_EC_CONFIG_ADD_CHILD(cw_ec_config_add_pdo, cw_ec_pdo_node, cfg,
		       config_pdos, config_pdo_count, CW_EC_CONFIG_PDO_MAX,
		       !node->cfg.sync_config_id || !node->cfg.pdo_index ||
		       node->cfg.reserved)

CW_EC_CONFIG_ADD_CHILD(cw_ec_config_add_entry, cw_ec_entry_node, cfg,
		       config_entries, config_entry_count,
		       CW_EC_CONFIG_ENTRY_MAX,
		       !node->cfg.pdo_config_id || !node->cfg.entry_id ||
		       !node->cfg.index || !node->cfg.bit_length)

#undef CW_EC_CONFIG_ADD_CHILD

static long cw_ec_config_validate(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_config_validate result;
	struct cw_ec_entry_node *entry;
	struct cw_ec_sync_node *sync;
	struct cw_ec_pdo_node *pdo;
	struct cw_ec_slave_node *slave;
	int ret = 0;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = cw_ec_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = CW_EC_API_VERSION_MAJOR;

	mutex_lock(&ctx->lock);
	if (!ctx->config_started || !ctx->config_slave_count) {
		ret = -EINVAL;
		goto out;
	}
	list_for_each_entry(sync, &ctx->config_syncs, common.node) {
		struct cw_ec_sync_node *other;

		if (!cw_ec_config_id_exists(&ctx->config_slaves,
					    sync->cfg.slave_config_id)) {
			ret = -ENOENT;
			goto out;
		}
		list_for_each_entry(other, &ctx->config_syncs, common.node) {
			if (other != sync &&
			    other->cfg.slave_config_id ==
				    sync->cfg.slave_config_id &&
			    other->cfg.sync_index == sync->cfg.sync_index) {
				ret = -EEXIST;
				goto out;
			}
		}
	}
	list_for_each_entry(pdo, &ctx->config_pdos, common.node) {
		struct cw_ec_pdo_node *other;

		if (!cw_ec_config_id_exists(&ctx->config_syncs,
					    pdo->cfg.sync_config_id)) {
			ret = -ENOENT;
			goto out;
		}
		list_for_each_entry(other, &ctx->config_pdos, common.node) {
			if (other != pdo &&
			    other->cfg.sync_config_id == pdo->cfg.sync_config_id &&
			    other->cfg.pdo_index == pdo->cfg.pdo_index) {
				ret = -EEXIST;
				goto out;
			}
		}
	}
	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		struct cw_ec_entry_node *other;

		if (!cw_ec_config_id_exists(&ctx->config_pdos,
					    entry->cfg.pdo_config_id)) {
			ret = -ENOENT;
			goto out;
		}
		list_for_each_entry(other, &ctx->config_entries, common.node) {
			if (other != entry &&
			    other->cfg.entry_id == entry->cfg.entry_id) {
				ret = -EEXIST;
				goto out;
			}
			if (other != entry &&
			    other->cfg.pdo_config_id == entry->cfg.pdo_config_id &&
			    other->cfg.index == entry->cfg.index &&
			    other->cfg.subindex == entry->cfg.subindex) {
				ret = -EEXIST;
				goto out;
			}
		}
	}
	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		struct cw_ec_slave_node *other;

		list_for_each_entry(other, &ctx->config_slaves, common.node) {
			if (other != slave && other->cfg.alias == slave->cfg.alias &&
			    other->cfg.position == slave->cfg.position) {
				ret = -EEXIST;
				goto out;
			}
		}
	}
	ctx->config_validated = true;
out:
	result.slave_count = ctx->config_slave_count;
	result.sync_count = ctx->config_sync_count;
	result.pdo_count = ctx->config_pdo_count;
	result.entry_count = ctx->config_entry_count;
	result.result = ret;
	if (copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static int cw_ec_check_header(u16 struct_size, u16 api_major,
			      size_t expected_size)
{
	if (struct_size != expected_size)
		return -EINVAL;
	if (api_major != CW_EC_API_VERSION_MAJOR)
		return -EPROTONOSUPPORT;
	return 0;
}

static int cw_ec_validate_sdo_type(u8 type, u16 data_len)
{
	switch (type) {
	case CW_EC_SDO_U8:
	case CW_EC_SDO_S8:
		return data_len == 1 ? 0 : -EINVAL;
	case CW_EC_SDO_U16:
	case CW_EC_SDO_S16:
		return data_len == 2 ? 0 : -EINVAL;
	case CW_EC_SDO_U32:
	case CW_EC_SDO_S32:
		return data_len == 4 ? 0 : -EINVAL;
	case CW_EC_SDO_BYTES:
		return data_len > 0 ? 0 : -EINVAL;
	default:
		return -EINVAL;
	}
}

static long cw_ec_setup_begin(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_setup_begin request;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = cw_ec_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;

	mutex_lock(&ctx->lock);
	cw_ec_setup_clear(ctx);
	ctx->setup_started = true;
	mutex_unlock(&ctx->lock);
	return 0;
}

static long cw_ec_setup_reset(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_setup_begin request;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = cw_ec_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;

	mutex_lock(&ctx->lock);
	cw_ec_setup_clear(ctx);
	mutex_unlock(&ctx->lock);
	return 0;
}

static long cw_ec_setup_add_sdo(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_setup_sdo request;
	struct cw_ec_setup_entry *entry;
	size_t allocation_size;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = cw_ec_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (!request.sequence || request.data_len > CW_EC_SETUP_SDO_DATA_MAX)
		return -EINVAL;
	ret = cw_ec_validate_sdo_type(request.type, request.data_len);
	if (ret)
		return ret;

	allocation_size = struct_size(entry, data, request.data_len);
	entry = kzalloc(allocation_size, GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->sequence = request.sequence;
	entry->position = request.position;
	entry->index = request.index;
	entry->subindex = request.subindex;
	entry->type = request.type;
	entry->data_len = request.data_len;
	memcpy(entry->data, request.data, request.data_len);

	mutex_lock(&ctx->lock);
	if (!ctx->setup_started || ctx->setup_applied) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (ctx->setup_count >= CW_EC_SETUP_SDO_MAX ||
	    request.data_len > CW_EC_SETUP_SDO_TOTAL_MAX - ctx->setup_bytes) {
		ret = -E2BIG;
		goto out_unlock;
	}
	if (request.sequence == ctx->last_sequence) {
		ret = -EEXIST;
		goto out_unlock;
	}
	if (request.sequence < ctx->last_sequence) {
		ret = -EINVAL;
		goto out_unlock;
	}

	list_add_tail(&entry->node, &ctx->setup_sdos);
	ctx->setup_count++;
	ctx->setup_bytes += request.data_len;
	ctx->last_sequence = request.sequence;
	entry = NULL;
	ret = 0;

out_unlock:
	mutex_unlock(&ctx->lock);
	kfree(entry);
	return ret;
}

static long cw_ec_setup_apply(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_setup_apply result;
	struct cw_ec_setup_entry *entry;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = cw_ec_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = CW_EC_API_VERSION_MAJOR;

	mutex_lock(&ctx->lock);
	if (!ctx->setup_started || ctx->setup_applied || !ctx->setup_count) {
		ret = -EINVAL;
		result.result = ret;
		goto out_copy;
	}

	/*
	 * An attempted batch is never implicitly retryable: earlier writes may
	 * have reached the slave before a later write failed. Userspace must
	 * explicitly begin and submit a new batch after inspecting the result.
	 */
	ctx->setup_applied = true;
	result.operation_count = ctx->setup_count;
	list_for_each_entry(entry, &ctx->setup_sdos, node) {
		u32 abort_code = 0;

		ret = ecrt_master_sdo_download(ctx->master, entry->position,
					       entry->index, entry->subindex,
					       entry->data, entry->data_len,
					       &abort_code);
		if (ret) {
			result.failed_sequence = entry->sequence;
			result.failed_position = entry->position;
			result.failed_index = entry->index;
			result.failed_subindex = entry->subindex;
			result.result = ret;
			result.abort_code = abort_code;
			goto out_copy;
		}
		result.completed_count++;
	}

	ret = 0;

out_copy:
	if (copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static long cw_ec_sdo_upload(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_sdo_upload result;
	size_t result_size = 0;
	u32 abort_code = 0;
	u16 requested_len;
	u16 position;
	u16 index;
	u8 subindex;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = cw_ec_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (!result.index || !result.requested_len ||
	    result.requested_len > CW_EC_SETUP_SDO_DATA_MAX)
		return -EINVAL;

	position = result.position;
	index = result.index;
	subindex = result.subindex;
	requested_len = result.requested_len;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = CW_EC_API_VERSION_MAJOR;
	result.position = position;
	result.index = index;
	result.subindex = subindex;
	result.requested_len = requested_len;

	mutex_lock(&ctx->lock);
	ret = ecrt_master_sdo_upload(ctx->master, position, index, subindex,
				     result.data, requested_len, &result_size,
				     &abort_code);
	mutex_unlock(&ctx->lock);

	if (result_size > requested_len ||
	    result_size > CW_EC_SETUP_SDO_DATA_MAX) {
		ret = -EOVERFLOW;
		result_size = 0;
	}
	result.result = ret;
	result.abort_code = abort_code;
	result.result_len = result_size;

	if (copy_to_user(argp, &result, sizeof(result)))
		return -EFAULT;
	return ret;
}

static long cw_ec_get_api_version(void __user *argp)
{
	struct cw_ec_api_version version = {
		.struct_size = sizeof(version),
		.major = CW_EC_API_VERSION_MAJOR,
		.minor = CW_EC_API_VERSION_MINOR,
	};

	if (copy_to_user(argp, &version, sizeof(version)))
		return -EFAULT;

	return 0;
}

static long cw_ec_get_master_info(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_master_info info = {
		.struct_size = sizeof(info),
		.api_major = CW_EC_API_VERSION_MAJOR,
		.api_minor = CW_EC_API_VERSION_MINOR,
	};
	ec_master_info_t ec_info = {};
	int ret;

	ret = ecrt_master(ctx->master, &ec_info);
	if (ret)
		return ret;

	if (ec_info.slave_count > U32_MAX)
		return -EOVERFLOW;

	info.slave_count = ec_info.slave_count;
	info.link_up = !!ec_info.link_up;
	info.scan_busy = !!ec_info.scan_busy;
	info.application_time = ec_info.app_time;

	if (copy_to_user(argp, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static long cw_ec_get_slave_info(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_slave_info info;
	ec_slave_info_t ec_info = {};
	int ret;

	if (copy_from_user(&info, argp, sizeof(info)))
		return -EFAULT;

	if (info.struct_size != sizeof(info))
		return -EINVAL;
	if (info.api_major != CW_EC_API_VERSION_MAJOR)
		return -EPROTONOSUPPORT;

	ret = ecrt_master_get_slave(ctx->master, info.position, &ec_info);
	if (ret)
		return ret;

	memset(&info, 0, sizeof(info));
	info.struct_size = sizeof(info);
	info.api_major = CW_EC_API_VERSION_MAJOR;
	info.position = ec_info.position;
	info.alias = ec_info.alias;
	info.vendor_id = ec_info.vendor_id;
	info.product_code = ec_info.product_code;
	info.revision_number = ec_info.revision_number;
	info.serial_number = ec_info.serial_number;
	info.current_on_ebus_ma = ec_info.current_on_ebus;
	info.al_state = ec_info.al_state;
	info.error_flag = ec_info.error_flag;
	info.sync_count = ec_info.sync_count;
	info.sdo_count = ec_info.sdo_count;
	strscpy(info.name, ec_info.name, sizeof(info.name));

	if (copy_to_user(argp, &info, sizeof(info)))
		return -EFAULT;

	return 0;
}

static long cw_ec_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	struct cw_ec_file *ctx = file->private_data;
	void __user *argp = (void __user *)arg;

	if (!ctx || !ctx->master)
		return -ENODEV;

	if (_IOC_TYPE(cmd) != CW_EC_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case CW_EC_IOC_GET_API_VERSION:
		return cw_ec_get_api_version(argp);
	case CW_EC_IOC_GET_MASTER_INFO:
		return cw_ec_get_master_info(ctx, argp);
	case CW_EC_IOC_GET_SLAVE_INFO:
		return cw_ec_get_slave_info(ctx, argp);
	case CW_EC_IOC_SETUP_BEGIN:
		return cw_ec_setup_begin(ctx, argp);
	case CW_EC_IOC_SETUP_ADD_SDO:
		return cw_ec_setup_add_sdo(ctx, argp);
	case CW_EC_IOC_SETUP_APPLY:
		return cw_ec_setup_apply(ctx, argp);
	case CW_EC_IOC_SETUP_RESET:
		return cw_ec_setup_reset(ctx, argp);
	case CW_EC_IOC_SDO_UPLOAD:
		return cw_ec_sdo_upload(ctx, argp);
	case CW_EC_IOC_CONFIG_BEGIN:
		return cw_ec_config_begin(ctx, argp);
	case CW_EC_IOC_CONFIG_ADD_SLAVE:
		return cw_ec_config_add_slave(ctx, argp);
	case CW_EC_IOC_CONFIG_ADD_SYNC:
		return cw_ec_config_add_sync(ctx, argp);
	case CW_EC_IOC_CONFIG_ADD_PDO:
		return cw_ec_config_add_pdo(ctx, argp);
	case CW_EC_IOC_CONFIG_ADD_ENTRY:
		return cw_ec_config_add_entry(ctx, argp);
	case CW_EC_IOC_CONFIG_VALIDATE:
		return cw_ec_config_validate(ctx, argp);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations cw_ec_fops = {
	.owner = THIS_MODULE,
	.open = cw_ec_open,
	.release = cw_ec_release,
	.unlocked_ioctl = cw_ec_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = no_llseek,
};

static struct miscdevice cw_ec_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "cw_ethercat0",
	.fops = &cw_ec_fops,
	.mode = 0600,
};

static int __init cw_ec_init(void)
{
	unsigned int runtime_magic;
	int ret;

	runtime_magic = ecrt_version_magic();
	if (runtime_magic != ECRT_VERSION_MAGIC) {
		pr_err(CW_EC_NAME
		       ": EtherLab API mismatch: header=0x%x runtime=0x%x\n",
		       ECRT_VERSION_MAGIC, runtime_magic);
		return -EPROTO;
	}

	ret = misc_register(&cw_ec_miscdev);
	if (ret)
		return ret;

	pr_info(CW_EC_NAME ": registered /dev/%s (API %u.%u)\n",
		cw_ec_miscdev.name, CW_EC_API_VERSION_MAJOR,
		CW_EC_API_VERSION_MINOR);
	return 0;
}

static void __exit cw_ec_exit(void)
{
	misc_deregister(&cw_ec_miscdev);
	pr_info(CW_EC_NAME ": unloaded\n");
}

module_init(cw_ec_init);
module_exit(cw_ec_exit);

MODULE_AUTHOR("latproc");
MODULE_DESCRIPTION("Generic EtherLab cyclic transport");
MODULE_LICENSE("GPL");
