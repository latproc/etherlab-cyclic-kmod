/*
 * SPDX-License-Identifier: GPL-2.0-only OR LGPL-2.1-or-later
 *
 * Dual-licensed so the kernel module may use this header under GPL-2.0-only
 * while userspace (including proprietary) clients may include it under
 * LGPL-2.1-or-later when linking libelcethercat or speaking the ioctl ABI.
 * See LICENSE and LICENSE.LGPL-2.1.
 */
#ifndef ELC_ETHERCAT_UAPI_H
#define ELC_ETHERCAT_UAPI_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <linux/ioctl.h>
#include <linux/types.h>
#endif

#define ELC_API_VERSION_MAJOR 0U
#define ELC_API_VERSION_MINOR 17U

#define ELC_CYCLE_PERIOD_MIN_NS 100000U
#define ELC_CYCLE_PERIOD_MAX_NS 1000000000U

#define ELC_SLAVE_NAME_LEN 64U
#define ELC_SETUP_SDO_DATA_MAX 256U
#define ELC_SETUP_SDO_MAX 256U
#define ELC_SETUP_SDO_TOTAL_MAX 16384U
#define ELC_CONFIG_SLAVE_MAX 256U
#define ELC_CONFIG_SYNC_MAX 1024U
#define ELC_CONFIG_PDO_MAX 4096U
#define ELC_CONFIG_ENTRY_MAX 16384U
#define ELC_CONFIG_DC_MAX ELC_CONFIG_SLAVE_MAX
#define ELC_CONFIG_DOMAIN_MAX ELC_CONFIG_SLAVE_MAX
#define ELC_PROCESS_IMAGE_MAX (64U * 1024U)
#define ELC_CYCLE_WAIT_TIMEOUT_MAX_MS 60000U
#define ELC_OUTPUT_LEASE_CYCLES_MAX 1000000U
#define ELC_INPUT_HISTORY_DEPTH_MAX 4096U
#define ELC_INPUT_HISTORY_BATCH_MAX 256U
#define ELC_INPUT_HISTORY_BYTES_MAX (16U * 1024U * 1024U)

#define ELC_CAP_COHERENT_PROCESS_IMAGE (1ULL << 0)
#define ELC_CAP_CYCLE_TIMING (1ULL << 1)
#define ELC_CAP_CYCLE_WAIT (1ULL << 2)
#define ELC_CAP_DC_DIAGNOSTICS (1ULL << 3)
#define ELC_CAP_OUTPUT_LEASE (1ULL << 4)
#define ELC_CAP_CYCLE_PERIOD_UPDATE (1ULL << 5)
#define ELC_CAP_INPUT_HISTORY (1ULL << 6)
#define ELC_CAP_CYCLE_DC_INFO (1ULL << 7)
#define ELC_CAP_DOMAIN_OUTPUT_AUTHORITY (1ULL << 8)

enum elc_sdo_type {
	ELC_SDO_U8 = 1,
	ELC_SDO_S8 = 2,
	ELC_SDO_U16 = 3,
	ELC_SDO_S16 = 4,
	ELC_SDO_U32 = 5,
	ELC_SDO_S32 = 6,
	ELC_SDO_BYTES = 7,
};

enum elc_direction {
	ELC_DIR_OUTPUT = 1,
	ELC_DIR_INPUT = 2,
};

enum elc_watchdog_mode {
	ELC_WD_DEFAULT = 0,
	ELC_WD_ENABLE = 1,
	ELC_WD_DISABLE = 2,
};

enum elc_config_object_kind {
	ELC_CONFIG_OBJECT_NONE = 0,
	ELC_CONFIG_OBJECT_SLAVE = 1,
	ELC_CONFIG_OBJECT_SYNC = 2,
	ELC_CONFIG_OBJECT_PDO = 3,
	ELC_CONFIG_OBJECT_ENTRY = 4,
	ELC_CONFIG_OBJECT_DC = 5,
	ELC_CONFIG_OBJECT_DC_POLICY = 6,
	ELC_CONFIG_OBJECT_DOMAIN = 7,
	ELC_CONFIG_OBJECT_DOMAIN_ASSIGNMENT = 8,
};

enum elc_dc_reference_mode {
	ELC_DC_REFERENCE_DISABLED = 0,
	ELC_DC_REFERENCE_AUTO = 1,
	ELC_DC_REFERENCE_EXPLICIT = 2,
};

enum elc_io_fault {
	ELC_IO_FAULT_NONE = 0,
	ELC_IO_FAULT_MASTER_STATE = 1U << 0,
	ELC_IO_FAULT_LINK_DOWN = 1U << 1,
	ELC_IO_FAULT_SLAVE_STATE = 1U << 2,
	ELC_IO_FAULT_SLAVE_OFFLINE = 1U << 3,
	ELC_IO_FAULT_SLAVE_NOT_OPERATIONAL = 1U << 4,
	ELC_IO_FAULT_DOMAIN_INCOMPLETE = 1U << 5,
	ELC_IO_FAULT_CONTROLLER_STALE = 1U << 6,
};

struct elc_api_version {
	__u16 struct_size;
	__u16 major;
	__u16 minor;
	__u16 reserved;
};

struct elc_capabilities {
	__u16 struct_size;
	__u16 api_major;
	__u32 reserved0;
	__u64 capabilities;
	__u64 reserved1[3];
};

struct elc_master_info {
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

struct elc_slave_info {
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
	char name[ELC_SLAVE_NAME_LEN];
};

struct elc_setup_begin {
	__u16 struct_size;
	__u16 api_major;
	__u32 reserved;
};

struct elc_setup_sdo {
	__u16 struct_size;
	__u16 api_major;
	__u32 sequence;
	__u16 position;
	__u16 index;
	__u8 subindex;
	__u8 type;
	__u16 data_len;
	__u8 data[ELC_SETUP_SDO_DATA_MAX];
};

struct elc_setup_apply {
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

struct elc_sdo_upload {
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
	__u8 data[ELC_SETUP_SDO_DATA_MAX];
};

struct elc_config_begin {
	__u16 struct_size;
	__u16 api_major;
	__u32 reserved;
};

struct elc_config_slave {
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

struct elc_config_sync {
	__u16 struct_size;
	__u16 api_major;
	__u32 config_id;
	__u32 slave_config_id;
	__u8 sync_index;
	__u8 direction;
	__u8 watchdog_mode;
	__u8 reserved;
};

struct elc_config_pdo {
	__u16 struct_size;
	__u16 api_major;
	__u32 config_id;
	__u32 sync_config_id;
	__u16 pdo_index;
	__u16 reserved;
};

struct elc_config_entry {
	__u16 struct_size;
	__u16 api_major;
	__u32 config_id;
	__u32 pdo_config_id;
	__u32 entry_id;
	__u16 index;
	__u8 subindex;
	__u8 bit_length;
};

struct elc_config_dc {
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

struct elc_config_dc_policy {
	__u16 struct_size;
	__u16 api_major;
	__u8 reference_mode;
	__u8 reserved0[3];
	__u32 reference_slave_config_id;
	__u32 flags;
};

struct elc_config_domain {
	__u16 struct_size;
	__u16 api_major;
	__u32 config_id;
	__u32 flags;
	__u32 reserved;
};

struct elc_config_domain_assignment {
	__u16 struct_size;
	__u16 api_major;
	__u32 config_id;
	__u32 slave_config_id;
	__u32 domain_config_id;
	__u32 flags;
};

struct elc_config_validate {
	__u16 struct_size;
	__u16 api_major;
	__u32 slave_count;
	__u32 sync_count;
	__u32 pdo_count;
	__u32 entry_count;
	__s32 result;
	__u32 reserved;
};

struct elc_config_apply {
	__u16 struct_size;
	__u16 api_major;
	__u32 failed_config_id;
	__u8 failed_object_kind;
	__u8 reserved0[3];
	__s32 result;
	__u32 reserved1;
};

struct elc_domain_create {
	__u16 struct_size;
	__u16 api_major;
	__u32 failed_config_id;
	__s32 result;
	__u32 entry_count;
	__u32 reserved;
};

struct elc_entry_offset {
	__u16 struct_size;
	__u16 api_major;
	__u32 entry_id;
	union {
		__u32 global_offset;
		/* Deprecated name retained as an ABI-neutral source alias. */
		__u32 domain_offset;
	};
	__u8 bit_position;
	__u8 bit_length;
	__u8 reserved[2];
};

struct elc_cycle_activate {
	__u16 struct_size;
	__u16 api_major;
	__u32 cycle_period_ns;
	__u32 flags;
	__u32 domain_size;
	__s32 result;
};

struct elc_cycle_status {
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

struct elc_cycle_period_update {
	__u16 struct_size;
	__u16 api_major;
	__u32 flags;
	__u64 config_generation;
	__u32 cycle_period_ns;
	__u32 applied_period_ns;
	__u64 effective_after_cycle;
};

struct elc_cycle_info {
	__u16 struct_size;
	__u16 api_major;
	__u32 flags;
	__u64 config_generation;
	__u64 cycle_index;
	__u64 cycle_period_ns;
	__u64 scheduled_time_ns;
	__u64 actual_wake_time_ns;
	__s64 wake_lateness_ns;
	__u64 input_sequence;
	__u64 output_sequence_consumed;
	__u64 missed_deadlines;
	__u64 stale_output_cycles;
	__u32 working_counter;
	__u8 working_counter_state;
	__u8 outputs_armed;
	__u8 bus_healthy;
	__u8 reserved0;
	__s32 cycle_result;
	__u32 reserved1;
};

/*
 * Coherent per-cycle record with DC motion-clock contract fields.
 * All fields are from the same cycle; DC fields are zero when DC is
 * not configured. The kernel publishes this atomically under the same
 * cycle_info_lock as the base cycle info.
 */
struct elc_cycle_dc_info {
	__u16 struct_size;
	__u16 api_major;
	__u32 flags;
	__u64 config_generation;
	__u64 cycle_index;
	__u64 cycle_period_ns;
	__u64 scheduled_time_ns;
	__u64 actual_wake_time_ns;
	__s64 wake_lateness_ns;
	__u64 input_sequence;
	__u64 output_sequence_consumed;
	__u64 missed_deadlines;
	__u64 stale_output_cycles;
	__u32 working_counter;
	__u8 working_counter_state;
	__u8 outputs_armed;
	__u8 bus_healthy;
	__u8 dc_enabled;
	__s32 cycle_result;
	__u32 reserved1;
	/* DC motion-clock contract fields */
	__u64 application_time_ns;
	__u8 dc_reference_valid;
	__u8 reserved2[3];
	__u32 dc_reference_sample;
	__s32 dc_phase_difference_ns;
	__s32 dc_applied_adjustment_ns;
};

/*
 * Wait for a cycle record newer than after_cycle_index in the exact active
 * configuration generation. The kernel returns a coherent cycle snapshot;
 * timeout_ms bounds an interruptible sleep and is never used by the RT task.
 */
struct elc_cycle_wait {
	__u16 struct_size;
	__u16 api_major;
	__u32 flags;
	__u64 config_generation;
	__u64 after_cycle_index;
	__u32 timeout_ms;
	__u32 reserved0;
	struct elc_cycle_info cycle;
};

struct elc_cycle_deactivate {
	__u16 struct_size;
	__u16 api_major;
	__u32 reserved;
	__s32 result;
	__u32 reserved1;
};

struct elc_dc_status {
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

struct elc_io_status {
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

struct elc_input_snapshot {
	__u16 struct_size;
	__u16 api_major;
	__u32 flags;
	__u64 data_ptr;
	__u32 data_capacity;
	__u32 data_size;
	__u64 config_generation;
	__u64 input_sequence;
	__u64 cycle_count;
};

struct elc_input_history_config {
	__u16 struct_size;
	__u16 api_major;
	__u32 flags;
	__u64 config_generation;
	__u32 depth;
	__u32 configured_depth;
};

struct elc_input_history_record {
	__u64 config_generation;
	__u64 cycle_index;
	__u64 input_sequence;
	__u64 scheduled_time_ns;
	__u64 actual_wake_time_ns;
	__s64 wake_lateness_ns;
	__s32 cycle_result;
	__u32 reserved;
};

struct elc_input_history_batch {
	__u16 struct_size;
	__u16 api_major;
	__u32 flags;
	__u64 config_generation;
	__u64 after_cycle_index;
	__u64 records_ptr;
	__u64 data_ptr;
	__u32 max_records;
	__u32 data_capacity;
	__u32 record_count;
	__u32 image_size;
	__u64 first_cycle_index;
	__u64 last_cycle_index;
	__u64 latest_cycle_index;
	__u64 dropped_records;
	__u64 capture_drop_count;
};

struct elc_output_publish {
	__u16 struct_size;
	__u16 api_major;
	__u32 flags;
	__u64 data_ptr;
	__u64 mask_ptr;
	__u32 data_size;
	/*
	 * 0 = all domains (global image size). Non-zero = domain_config_id;
	 * then data_size must equal that domain segment size.
	 */
	__u32 domain_config_id;
	__u64 config_generation;
	__u64 output_sequence;
};

struct elc_output_arm {
	__u16 struct_size;
	__u16 api_major;
	/*
	 * 0 = arm every healthy domain with matching sequence.
	 * Non-zero = domain_config_id for that domain only.
	 */
	__u32 flags;
	__u64 config_generation;
	__u64 output_sequence;
};

struct elc_output_disarm {
	__u16 struct_size;
	__u16 api_major;
	/*
	 * 0 = disarm all domains.
	 * Non-zero = domain_config_id for that domain only.
	 */
	__u32 flags;
	__u64 config_generation;
};

struct elc_output_lease_config {
	__u16 struct_size;
	__u16 api_major;
	__u32 flags;
	__u64 config_generation;
	__u32 cycle_budget;
	__u32 reserved0;
	__u64 reserved1;
};

struct elc_output_lease_renew {
	__u16 struct_size;
	__u16 api_major;
	__u32 flags;
	__u64 config_generation;
	__u32 remaining_cycles;
	__u32 reserved0;
	__u64 renewal_count;
};

struct elc_output_lease_status {
	__u16 struct_size;
	__u16 api_major;
	__u32 flags;
	__u64 config_generation;
	__u32 configured_cycles;
	__u32 remaining_cycles;
	__u8 enabled;
	__u8 valid;
	__u8 reserved0[6];
	__u64 renewal_count;
	__u64 expiry_count;
};

struct elc_config_slave_status {
	__u16 struct_size;
	__u16 api_major;
	__u32 config_id;
	__u64 config_generation;
	__u8 active;
	__u8 online;
	__u8 operational;
	__u8 data_valid;
	__u8 al_state;
	__u8 reserved0[3];
	__s32 state_result;
	__u32 reserved1;
	__u64 cycle_count;
	__u64 input_sequence;
};

struct elc_domain_status {
	__u16 struct_size;
	__u16 api_major;
	__u32 domain_config_id;
	__u64 config_generation;
	__u32 base_offset;
	__u32 domain_size;
	__u32 working_counter;
	__u32 current_faults;
	__u8 active;
	__u8 working_counter_state;
	__u8 data_valid;
	__u8 outputs_armed;
	__u8 rearm_required;
	__u8 reserved0[3];
	__u64 cycle_count;
	__u64 input_sequence;
};

#define ELC_IOC_MAGIC 0xec

#define ELC_IOC_GET_API_VERSION \
	_IOR(ELC_IOC_MAGIC, 0x00, struct elc_api_version)
#define ELC_IOC_GET_MASTER_INFO \
	_IOR(ELC_IOC_MAGIC, 0x01, struct elc_master_info)
#define ELC_IOC_GET_SLAVE_INFO \
	_IOWR(ELC_IOC_MAGIC, 0x02, struct elc_slave_info)
#define ELC_IOC_GET_CAPABILITIES \
	_IOWR(ELC_IOC_MAGIC, 0x03, struct elc_capabilities)
#define ELC_IOC_SETUP_BEGIN \
	_IOW(ELC_IOC_MAGIC, 0x10, struct elc_setup_begin)
#define ELC_IOC_SETUP_ADD_SDO \
	_IOW(ELC_IOC_MAGIC, 0x11, struct elc_setup_sdo)
#define ELC_IOC_SETUP_APPLY \
	_IOWR(ELC_IOC_MAGIC, 0x12, struct elc_setup_apply)
#define ELC_IOC_SETUP_RESET \
	_IOW(ELC_IOC_MAGIC, 0x13, struct elc_setup_begin)
#define ELC_IOC_SDO_UPLOAD \
	_IOWR(ELC_IOC_MAGIC, 0x14, struct elc_sdo_upload)
#define ELC_IOC_CONFIG_BEGIN \
	_IOW(ELC_IOC_MAGIC, 0x20, struct elc_config_begin)
#define ELC_IOC_CONFIG_ADD_SLAVE \
	_IOW(ELC_IOC_MAGIC, 0x21, struct elc_config_slave)
#define ELC_IOC_CONFIG_ADD_SYNC \
	_IOW(ELC_IOC_MAGIC, 0x22, struct elc_config_sync)
#define ELC_IOC_CONFIG_ADD_PDO \
	_IOW(ELC_IOC_MAGIC, 0x23, struct elc_config_pdo)
#define ELC_IOC_CONFIG_ADD_ENTRY \
	_IOW(ELC_IOC_MAGIC, 0x24, struct elc_config_entry)
#define ELC_IOC_CONFIG_VALIDATE \
	_IOWR(ELC_IOC_MAGIC, 0x25, struct elc_config_validate)
#define ELC_IOC_CONFIG_APPLY \
	_IOWR(ELC_IOC_MAGIC, 0x26, struct elc_config_apply)
#define ELC_IOC_DOMAIN_CREATE \
	_IOWR(ELC_IOC_MAGIC, 0x27, struct elc_domain_create)
#define ELC_IOC_GET_ENTRY_OFFSET \
	_IOWR(ELC_IOC_MAGIC, 0x28, struct elc_entry_offset)
#define ELC_IOC_CONFIG_ADD_DC \
	_IOW(ELC_IOC_MAGIC, 0x29, struct elc_config_dc)
#define ELC_IOC_CONFIG_SET_DC_POLICY \
	_IOW(ELC_IOC_MAGIC, 0x2a, struct elc_config_dc_policy)
#define ELC_IOC_CONFIG_ADD_DOMAIN \
	_IOW(ELC_IOC_MAGIC, 0x2b, struct elc_config_domain)
#define ELC_IOC_CONFIG_ASSIGN_DOMAIN \
	_IOW(ELC_IOC_MAGIC, 0x2c, struct elc_config_domain_assignment)
#define ELC_IOC_CYCLE_ACTIVATE \
	_IOWR(ELC_IOC_MAGIC, 0x30, struct elc_cycle_activate)
#define ELC_IOC_CYCLE_GET_STATUS \
	_IOWR(ELC_IOC_MAGIC, 0x31, struct elc_cycle_status)
#define ELC_IOC_CYCLE_DEACTIVATE \
	_IOWR(ELC_IOC_MAGIC, 0x32, struct elc_cycle_deactivate)
#define ELC_IOC_CYCLE_GET_DC_STATUS \
	_IOWR(ELC_IOC_MAGIC, 0x33, struct elc_dc_status)
#define ELC_IOC_CYCLE_GET_INFO \
	_IOWR(ELC_IOC_MAGIC, 0x34, struct elc_cycle_info)
#define ELC_IOC_CYCLE_WAIT \
	_IOWR(ELC_IOC_MAGIC, 0x35, struct elc_cycle_wait)
#define ELC_IOC_CYCLE_SET_PERIOD \
	_IOWR(ELC_IOC_MAGIC, 0x36, struct elc_cycle_period_update)
#define ELC_IOC_GET_IO_STATUS \
	_IOWR(ELC_IOC_MAGIC, 0x40, struct elc_io_status)
#define ELC_IOC_GET_INPUT_SNAPSHOT \
	_IOWR(ELC_IOC_MAGIC, 0x41, struct elc_input_snapshot)
#define ELC_IOC_PUBLISH_OUTPUT \
	_IOWR(ELC_IOC_MAGIC, 0x42, struct elc_output_publish)
#define ELC_IOC_ARM_OUTPUTS \
	_IOW(ELC_IOC_MAGIC, 0x43, struct elc_output_arm)
#define ELC_IOC_DISARM_OUTPUTS \
	_IOW(ELC_IOC_MAGIC, 0x44, struct elc_output_disarm)
#define ELC_IOC_GET_CONFIG_SLAVE_STATUS \
	_IOWR(ELC_IOC_MAGIC, 0x45, struct elc_config_slave_status)
#define ELC_IOC_GET_DOMAIN_STATUS \
	_IOWR(ELC_IOC_MAGIC, 0x46, struct elc_domain_status)
#define ELC_IOC_CONFIGURE_OUTPUT_LEASE \
	_IOW(ELC_IOC_MAGIC, 0x47, struct elc_output_lease_config)
#define ELC_IOC_RENEW_OUTPUT_LEASE \
	_IOWR(ELC_IOC_MAGIC, 0x48, struct elc_output_lease_renew)
#define ELC_IOC_GET_OUTPUT_LEASE_STATUS \
	_IOWR(ELC_IOC_MAGIC, 0x49, struct elc_output_lease_status)
#define ELC_IOC_CONFIGURE_INPUT_HISTORY \
	_IOWR(ELC_IOC_MAGIC, 0x4a, struct elc_input_history_config)
#define ELC_IOC_GET_INPUT_HISTORY_BATCH \
	_IOWR(ELC_IOC_MAGIC, 0x4b, struct elc_input_history_batch)
#define ELC_IOC_CYCLE_GET_DC_INFO \
	_IOWR(ELC_IOC_MAGIC, 0x37, struct elc_cycle_dc_info)

#endif /* ELC_ETHERCAT_UAPI_H */
