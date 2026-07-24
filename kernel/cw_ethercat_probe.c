// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal EtherLab external-application probe.
 *
 * This intentionally acquires and releases master 0 during module
 * initialization. It does not retain ownership or perform cyclic I/O.
 */

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/module.h>

#include <ecrt.h>

#define CW_EC_PROBE_NAME "cw_ethercat_probe"

static int __init cw_ec_probe_init(void)
{
	ec_master_t *master;
	unsigned int runtime_magic;

	runtime_magic = ecrt_version_magic();
	if (runtime_magic != ECRT_VERSION_MAGIC) {
		pr_err(CW_EC_PROBE_NAME
		       ": EtherLab API mismatch: module header=0x%x runtime=0x%x\n",
		       ECRT_VERSION_MAGIC, runtime_magic);
		return -EPROTO;
	}

	pr_info(CW_EC_PROBE_NAME
		": requesting EtherLab master 0 (API magic 0x%x)\n",
		runtime_magic);

	master = ecrt_request_master(0);
	if (!master) {
		pr_err(CW_EC_PROBE_NAME
		       ": failed to request EtherLab master 0\n");
		return -EBUSY;
	}

	pr_info(CW_EC_PROBE_NAME
		": acquired EtherLab master 0; releasing immediately\n");
	ecrt_release_master(master);
	pr_info(CW_EC_PROBE_NAME
		": released EtherLab master 0 successfully\n");

	return 0;
}

static void __exit cw_ec_probe_exit(void)
{
	pr_info(CW_EC_PROBE_NAME ": unloaded\n");
}

module_init(cw_ec_probe_init);
module_exit(cw_ec_probe_exit);

MODULE_AUTHOR("latproc");
MODULE_DESCRIPTION("Minimal EtherLab master acquisition/release probe");
MODULE_LICENSE("GPL");
