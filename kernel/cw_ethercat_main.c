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

static atomic_t cw_ec_control_open = ATOMIC_INIT(0);

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
