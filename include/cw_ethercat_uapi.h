/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef CW_ETHERCAT_UAPI_H
#define CW_ETHERCAT_UAPI_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <linux/ioctl.h>
#include <linux/types.h>
#endif

#define CW_EC_API_VERSION_MAJOR 0U
#define CW_EC_API_VERSION_MINOR 1U

#define CW_EC_SLAVE_NAME_LEN 64U

struct cw_ec_api_version {
	__u16 struct_size;
	__u16 major;
	__u16 minor;
	__u16 reserved;
};

struct cw_ec_master_info {
	__u16 struct_size;
	__u16 api_major;
	__u16 api_minor;
	__u16 reserved0;
	__u32 slave_count;
	__u8 link_up;
	__u8 scan_busy;
	__u8 reserved1[2];
	__u64 application_time;
};

struct cw_ec_slave_info {
	__u16 struct_size;
	__u16 api_major;
	__u16 position;
	__u16 alias;
	__u32 vendor_id;
	__u32 product_code;
	__u32 revision_number;
	__u32 serial_number;
	__s16 current_on_ebus_ma;
	__u8 al_state;
	__u8 error_flag;
	__u8 sync_count;
	__u8 reserved0;
	__u16 sdo_count;
	__u8 reserved1[2];
	char name[CW_EC_SLAVE_NAME_LEN];
};

#define CW_EC_IOC_MAGIC 0xec

#define CW_EC_IOC_GET_API_VERSION \
	_IOR(CW_EC_IOC_MAGIC, 0x00, struct cw_ec_api_version)
#define CW_EC_IOC_GET_MASTER_INFO \
	_IOR(CW_EC_IOC_MAGIC, 0x01, struct cw_ec_master_info)
#define CW_EC_IOC_GET_SLAVE_INFO \
	_IOWR(CW_EC_IOC_MAGIC, 0x02, struct cw_ec_slave_info)

#endif /* CW_ETHERCAT_UAPI_H */
