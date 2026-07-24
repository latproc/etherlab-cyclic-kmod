// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#include <ecrt.h>

#include "cw_ethercat_uapi.h"

#define CW_EC_NAME "cw_ethercat"

struct cw_ec_file {
	ec_master_t *master;
};

static atomic_t cw_ec_control_open = ATOMIC_INIT(0);

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
	file->private_data = ctx;
	nonseekable_open(inode, file);

	pr_info(CW_EC_NAME ": control owner acquired master 0\n");
	return 0;
}

static int cw_ec_release(struct inode *inode, struct file *file)
{
	struct cw_ec_file *ctx = file->private_data;

	if (ctx) {
		if (ctx->master) {
			ecrt_release_master(ctx->master);
			ctx->master = NULL;
		}
		kfree(ctx);
		file->private_data = NULL;
	}

	atomic_set(&cw_ec_control_open, 0);
	pr_info(CW_EC_NAME ": control owner released master 0\n");
	return 0;
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
