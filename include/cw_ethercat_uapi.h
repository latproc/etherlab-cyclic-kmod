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
#define CW_EC_API_VERSION_MINOR 6U

#define CW_EC_CYCLE_PERIOD_MIN_NS 100000U
#define CW_EC_CYCLE_PERIOD_MAX_NS 1000000000U

#define CW_EC_SLAVE_NAME_LEN 64U
#define CW_EC_SETUP_SDO_DATA_MAX 256U
#define CW_EC_SETUP_SDO_MAX 256U
#define CW_EC_SETUP_SDO_TOTAL_MAX 16384U
#define CW_EC_CONFIG_SLAVE_MAX 256U
#define CW_EC_CONFIG_SYNC_MAX 1024U
#define CW_EC_CONFIG_PDO_MAX 4096U
#define CW_EC_CONFIG_ENTRY_MAX 16384U
#define CW_EC_CONFIG_DC_MAX CW_EC_CONFIG_SLAVE_MAX

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
	CW_EC_CONFIG_OBJECT_DC = 5,
	CW_EC_CONFIG_OBJECT_DC_POLICY = 6,
};

enum cw_ec_dc_reference_mode {
	CW_EC_DC_REFERENCE_DISABLED = 0,
	CW_EC_DC_REFERENCE_AUTO = 1,
	CW_EC_DC_REFERENCE_EXPLICIT = 2,
};

enum cw_ec_io_fault {
	CW_EC_IO_FAULT_NONE = 0,
	CW_EC_IO_FAULT_MASTER_STATE = 1U << 0,
	CW_EC_IO_FAULT_LINK_DOWN = 1U << 1,
	CW_EC_IO_FAULT_SLAVE_STATE = 1U << 2,
	CW_EC_IO_FAULT_SLAVE_OFFLINE = 1U << 3,
	CW_EC_IO_FAULT_SLAVE_NOT_OPERATIONAL = 1U << 4,
	CW_EC_IO_FAULT_DOMAIN_INCOMPLETE = 1U << 5,
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

struct cw_ec_config_dc {
	__u16 struct_size;
	__u16 api_major;
	__u32 config_id;
	__u32 slave_config_id;
	__u16 assign_activate;
	__u16 reserved0;
	__u32 sync0_cycle_ns;
	__s32 sync0_shift_ns;
	__u32 sync1_cycle_ns;
	__s32 sync1_shift_ns;
	__u32 flags;
};

struct cw_ec_config_dc_policy {
	__u16 struct_size;
	__u16 api_major;
	__u8 reference_mode;
	__u8 reserved0[3];
	__u32 reference_slave_config_id;
	__u32 flags;
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

struct cw_ec_domain_create {
	__u16 struct_size;
	__u16 api_major;
	__u32 failed_config_id;
	__s32 result;
	__u32 entry_count;
	__u32 reserved;
};

struct cw_ec_entry_offset {
	__u16 struct_size;
	__u16 api_major;
	__u32 entry_id;
	__u32 domain_offset;
	__u8 bit_position;
	__u8 bit_length;
	__u8 reserved[2];
};

struct cw_ec_cycle_activate {
	__u16 struct_size;
	__u16 api_major;
	__u32 cycle_period_ns;
	__u32 flags;
	__u32 domain_size;
	__s32 result;
};

struct cw_ec_cycle_status {
	__u16 struct_size;
	__u16 api_major;
	__u8 active;
	__u8 reserved0[3];
	__u32 cycle_period_ns;
	__u32 domain_size;
	__u32 working_counter;
	__u8 working_counter_state;
	__u8 reserved1[3];
	__s32 last_cycle_result;
	__u64 cycle_count;
	__u64 cycle_error_count;
	__u64 cycle_overrun_count;
	__u64 maximum_lateness_ns;
};

struct cw_ec_cycle_deactivate {
	__u16 struct_size;
	__u16 api_major;
	__u32 reserved;
	__s32 result;
	__u32 reserved1;
};

struct cw_ec_dc_status {
	__u16 struct_size;
	__u16 api_major;
	__u8 enabled;
	__u8 reference_valid;
	__u8 monitor_pending;
	__u8 reserved0;
	__s32 last_reference_result;
	__s32 last_difference_ns;
	__s32 cycle_adjustment_ns;
	__u32 last_maximum_deviation_ns;
	__u32 maximum_deviation_ns;
	__u64 reference_read_error_count;
	__u64 reference_resume_count;
	__u64 monitor_success_count;
	__u64 monitor_timeout_count;
};

struct cw_ec_io_status {
	__u16 struct_size;
	__u16 api_major;
	__u8 bus_healthy;
	__u8 outputs_armed;
	__u8 rearm_required;
	__u8 link_up;
	__u32 current_faults;
	__u32 last_latched_faults;
	__u32 slaves_responding;
	__u32 configured_slave_count;
	__u32 configured_slaves_online;
	__u32 configured_slaves_operational;
	__u32 domain_size;
	__u32 reserved0;
	__u64 config_generation;
	__u64 fault_count;
	__u64 input_sequence;
	__u64 output_sequence;
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
#define CW_EC_IOC_DOMAIN_CREATE \
	_IOWR(CW_EC_IOC_MAGIC, 0x27, struct cw_ec_domain_create)
#define CW_EC_IOC_GET_ENTRY_OFFSET \
	_IOWR(CW_EC_IOC_MAGIC, 0x28, struct cw_ec_entry_offset)
#define CW_EC_IOC_CONFIG_ADD_DC \
	_IOW(CW_EC_IOC_MAGIC, 0x29, struct cw_ec_config_dc)
#define CW_EC_IOC_CONFIG_SET_DC_POLICY \
	_IOW(CW_EC_IOC_MAGIC, 0x2a, struct cw_ec_config_dc_policy)
#define CW_EC_IOC_CYCLE_ACTIVATE \
	_IOWR(CW_EC_IOC_MAGIC, 0x30, struct cw_ec_cycle_activate)
#define CW_EC_IOC_CYCLE_GET_STATUS \
	_IOWR(CW_EC_IOC_MAGIC, 0x31, struct cw_ec_cycle_status)
#define CW_EC_IOC_CYCLE_DEACTIVATE \
	_IOWR(CW_EC_IOC_MAGIC, 0x32, struct cw_ec_cycle_deactivate)
#define CW_EC_IOC_CYCLE_GET_DC_STATUS \
	_IOWR(CW_EC_IOC_MAGIC, 0x33, struct cw_ec_dc_status)
#define CW_EC_IOC_GET_IO_STATUS \
	_IOWR(CW_EC_IOC_MAGIC, 0x40, struct cw_ec_io_status)

#endif /* CW_ETHERCAT_UAPI_H */
