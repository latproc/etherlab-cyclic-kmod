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
#define CW_EC_API_VERSION_MINOR 3U

#define CW_EC_SLAVE_NAME_LEN 64U
#define CW_EC_SETUP_SDO_DATA_MAX 256U
#define CW_EC_SETUP_SDO_MAX 256U
#define CW_EC_SETUP_SDO_TOTAL_MAX 16384U
#define CW_EC_CONFIG_SLAVE_MAX 256U
#define CW_EC_CONFIG_SYNC_MAX 1024U
#define CW_EC_CONFIG_PDO_MAX 4096U
#define CW_EC_CONFIG_ENTRY_MAX 16384U

enum cw_ec_sdo_type {
	CW_EC_SDO_U8 = 1,
	CW_EC_SDO_S8 = 2,
	CW_EC_SDO_U16 = 3,
	CW_EC_SDO_S16 = 4,
	CW_EC_SDO_U32 = 5,
	CW_EC_SDO_S32 = 6,
	CW_EC_SDO_BYTES = 7,
};

enum cw_ec_direction {
	CW_EC_DIR_OUTPUT = 1,
	CW_EC_DIR_INPUT = 2,
};

enum cw_ec_watchdog_mode {
	CW_EC_WD_DEFAULT = 0,
	CW_EC_WD_ENABLE = 1,
	CW_EC_WD_DISABLE = 2,
};

enum cw_ec_config_object_kind {
	CW_EC_CONFIG_OBJECT_NONE = 0,
	CW_EC_CONFIG_OBJECT_SLAVE = 1,
	CW_EC_CONFIG_OBJECT_SYNC = 2,
	CW_EC_CONFIG_OBJECT_PDO = 3,
	CW_EC_CONFIG_OBJECT_ENTRY = 4,
};

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

struct cw_ec_setup_begin {
	__u16 struct_size;
	__u16 api_major;
	__u32 reserved;
};

struct cw_ec_setup_sdo {
	__u16 struct_size;
	__u16 api_major;
	__u32 sequence;
	__u16 position;
	__u16 index;
	__u8 subindex;
	__u8 type;
	__u16 data_len;
	__u8 data[CW_EC_SETUP_SDO_DATA_MAX];
};

struct cw_ec_setup_apply {
	__u16 struct_size;
	__u16 api_major;
	__u32 operation_count;
	__u32 completed_count;
	__u32 failed_sequence;
	__u16 failed_position;
	__u16 failed_index;
	__u8 failed_subindex;
	__u8 reserved0[3];
	__s32 result;
	__u32 abort_code;
};

struct cw_ec_sdo_upload {
	__u16 struct_size;
	__u16 api_major;
	__u16 position;
	__u16 index;
	__u8 subindex;
	__u8 reserved0;
	__u16 requested_len;
	__u16 result_len;
	__u16 reserved1;
	__s32 result;
	__u32 abort_code;
	__u8 data[CW_EC_SETUP_SDO_DATA_MAX];
};

struct cw_ec_config_begin {
	__u16 struct_size;
	__u16 api_major;
	__u32 reserved;
};

struct cw_ec_config_slave {
	__u16 struct_size;
	__u16 api_major;
	__u32 config_id;
	__u16 alias;
	__u16 position;
	__u32 vendor_id;
	__u32 product_code;
	__u32 revision_number;
	__u32 flags;
};

struct cw_ec_config_sync {
	__u16 struct_size;
	__u16 api_major;
	__u32 config_id;
	__u32 slave_config_id;
	__u8 sync_index;
	__u8 direction;
	__u8 watchdog_mode;
	__u8 reserved;
};

struct cw_ec_config_pdo {
	__u16 struct_size;
	__u16 api_major;
	__u32 config_id;
	__u32 sync_config_id;
	__u16 pdo_index;
	__u16 reserved;
};

struct cw_ec_config_entry {
	__u16 struct_size;
	__u16 api_major;
	__u32 config_id;
	__u32 pdo_config_id;
	__u32 entry_id;
	__u16 index;
	__u8 subindex;
	__u8 bit_length;
};

struct cw_ec_config_validate {
	__u16 struct_size;
	__u16 api_major;
	__u32 slave_count;
	__u32 sync_count;
	__u32 pdo_count;
	__u32 entry_count;
	__s32 result;
	__u32 reserved;
};

struct cw_ec_config_apply {
	__u16 struct_size;
	__u16 api_major;
	__u32 failed_config_id;
	__u8 failed_object_kind;
	__u8 reserved0[3];
	__s32 result;
	__u32 reserved1;
};

#define CW_EC_IOC_MAGIC 0xec

#define CW_EC_IOC_GET_API_VERSION \
	_IOR(CW_EC_IOC_MAGIC, 0x00, struct cw_ec_api_version)
#define CW_EC_IOC_GET_MASTER_INFO \
	_IOR(CW_EC_IOC_MAGIC, 0x01, struct cw_ec_master_info)
#define CW_EC_IOC_GET_SLAVE_INFO \
	_IOWR(CW_EC_IOC_MAGIC, 0x02, struct cw_ec_slave_info)
#define CW_EC_IOC_SETUP_BEGIN \
	_IOW(CW_EC_IOC_MAGIC, 0x10, struct cw_ec_setup_begin)
#define CW_EC_IOC_SETUP_ADD_SDO \
	_IOW(CW_EC_IOC_MAGIC, 0x11, struct cw_ec_setup_sdo)
#define CW_EC_IOC_SETUP_APPLY \
	_IOWR(CW_EC_IOC_MAGIC, 0x12, struct cw_ec_setup_apply)
#define CW_EC_IOC_SETUP_RESET \
	_IOW(CW_EC_IOC_MAGIC, 0x13, struct cw_ec_setup_begin)
#define CW_EC_IOC_SDO_UPLOAD \
	_IOWR(CW_EC_IOC_MAGIC, 0x14, struct cw_ec_sdo_upload)
#define CW_EC_IOC_CONFIG_BEGIN \
	_IOW(CW_EC_IOC_MAGIC, 0x20, struct cw_ec_config_begin)
#define CW_EC_IOC_CONFIG_ADD_SLAVE \
	_IOW(CW_EC_IOC_MAGIC, 0x21, struct cw_ec_config_slave)
#define CW_EC_IOC_CONFIG_ADD_SYNC \
	_IOW(CW_EC_IOC_MAGIC, 0x22, struct cw_ec_config_sync)
#define CW_EC_IOC_CONFIG_ADD_PDO \
	_IOW(CW_EC_IOC_MAGIC, 0x23, struct cw_ec_config_pdo)
#define CW_EC_IOC_CONFIG_ADD_ENTRY \
	_IOW(CW_EC_IOC_MAGIC, 0x24, struct cw_ec_config_entry)
#define CW_EC_IOC_CONFIG_VALIDATE \
	_IOWR(CW_EC_IOC_MAGIC, 0x25, struct cw_ec_config_validate)
#define CW_EC_IOC_CONFIG_APPLY \
	_IOWR(CW_EC_IOC_MAGIC, 0x26, struct cw_ec_config_apply)

#endif /* CW_ETHERCAT_UAPI_H */
