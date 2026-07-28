// SPDX-License-Identifier: GPL-2.0-only

#include <linux/atomic.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/init.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <ecrt.h>

#include "elc_ethercat_uapi.h"
#include "elc_kcompat.h"

#define ELC_NAME "elc_ethercat"
#define ELC_DEACTIVATE_SETTLE_MS 5000U
#define ELC_DEACTIVATE_POLL_MS 10U
#define ELC_HISTORY_SLOT_FREE 0U
#define ELC_HISTORY_SLOT_WRITER 1U
#define ELC_HISTORY_SLOT_READER 2U

/*
 * Output ownership is separate from EtherCAT master ownership. API 0.13 has
 * one compatibility authority covering every domain; later delegated domain
 * fds can use the same state with an immutable domain-set authorization.
 */
struct elc_output_authority {
	atomic_t armed;
	atomic_t rearm_required;
	atomic_t healthy;
	atomic_t current_faults;
	atomic_t last_latched_faults;
	atomic64_t fault_output_sequence;
	atomic64_t gate_request;
	atomic64_t gate_applied;
	wait_queue_head_t gate_wait;
	u64 last_sequence_consumed;
	u64 stale_cycles;
	spinlock_t lock;
	u8 *buffers[2];
	u8 *mask;
	u8 *update_mask;
	u8 active;
	int reader;
	atomic64_t sequence;
	u32 image_size;
	u32 lease_configured_cycles;
	u32 lease_timeout_ms;
	atomic_t lease_remaining_cycles;
	atomic64_t lease_renewal_count;
	atomic64_t lease_expiry_count;
	bool ever_healthy;
	bool initialised;
};

struct elc_input_history {
	spinlock_t lock;
	u8 *data;
	u8 *slot_state;
	struct elc_input_history_record *records;
	u32 configured_depth;
	u32 depth;
	u64 latest_cycle_index;
	atomic64_t capture_drop_count;
};

struct elc_file {
	ec_master_t *master;
	ec_domain_t *domain;
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
	struct list_head config_dcs;
	struct list_head config_domains;
	struct list_head config_domain_assignments;
	u32 config_slave_count;
	u32 config_sync_count;
	u32 config_pdo_count;
	u32 config_entry_count;
	u32 config_dc_count;
	u32 config_domain_count;
	u32 config_domain_assignment_count;
	u64 config_generation;
	struct elc_config_dc_policy dc_policy;
	bool dc_policy_set;
	bool config_started;
	bool config_validated;
	bool config_applied;
	bool config_poisoned;
	bool domain_registered;
	struct task_struct *cycle_thread;
	u8 *domain_data;
	u32 domain_size;
	u32 cycle_period_ns;
	atomic_t pending_cycle_period_ns;
	atomic64_t period_request_sequence;
	atomic64_t period_applied_sequence;
	atomic64_t period_applied_cycle;
	wait_queue_head_t period_wait;
	u64 application_time_ns;
	atomic64_t cycle_count;
	atomic64_t cycle_error_count;
	atomic64_t cycle_overrun_count;
	atomic64_t maximum_lateness_ns;
	atomic_t working_counter;
	atomic_t working_counter_state;
	atomic_t last_cycle_result;
	s64 dc_cycle_adjustment_ns;
	s64 dc_difference_total_ns;
	s64 dc_delta_total_ns;
	s32 dc_last_difference_ns;
	u32 dc_last_reference_sample;
	s32 dc_last_applied_adjustment_ns;
	u32 dc_filter_count;
	u32 dc_monitor_countdown;
	u32 dc_monitor_wait_cycles;
	bool dc_reference_valid;
	bool dc_monitor_pending;
	int dc_last_reference_result;
	atomic_t dc_status_reference_valid;
	atomic_t dc_status_monitor_pending;
	atomic_t dc_status_last_reference_result;
	atomic_t dc_status_last_difference_ns;
	atomic_t dc_status_cycle_adjustment_ns;
	atomic_t dc_status_last_maximum_deviation_ns;
	atomic64_t dc_status_maximum_deviation_ns;
	atomic64_t dc_reference_read_error_count;
	atomic64_t dc_reference_resume_count;
	atomic64_t dc_monitor_success_count;
	atomic64_t dc_monitor_timeout_count;
	atomic_t io_bus_healthy;
	atomic_t io_link_up;
	atomic_t io_current_faults;
	atomic_t io_slaves_responding;
	atomic_t io_configured_slaves_online;
	atomic_t io_configured_slaves_operational;
	atomic64_t io_fault_count;
	/* Per-domain output authorities live on elc_domain_node.authority.
	 * Published records are copied under cycle_info_lock; the atomic sequence
	 * changes only after the complete record is visible to waiters.
	 */
	wait_queue_head_t cycle_wait;
	spinlock_t cycle_info_lock;
	struct elc_cycle_info cycle_info;
	atomic64_t cycle_info_sequence;
	/* DC motion-clock contract fields: published under cycle_info_lock
	 * together with cycle_info so waiters see one coherent snapshot.
	 * Zero when DC is not configured.
	 */
	u64 dc_published_app_time_ns;
	u8 dc_published_reference_valid;
	u32 dc_published_reference_sample;
	s32 dc_published_phase_difference_ns;
	s32 dc_published_applied_adjustment_ns;
	spinlock_t input_lock;
	u8 *input_buffers[2];
	u8 input_active;
	int input_reader;
	/* Cycle identity belongs to the buffer, not to the later snapshot call. */
	u64 input_cycle_index[2];
	atomic64_t input_sequence;
	struct elc_input_history input_history;
	bool active;
};

struct elc_setup_entry {
	struct list_head node;
	u32 sequence;
	u16 position;
	u16 index;
	u8 subindex;
	u8 type;
	u16 data_len;
	u8 data[];
};

struct elc_config_node {
	struct list_head node;
	u32 config_id;
};

struct elc_domain_node;

struct elc_slave_node {
	struct elc_config_node common;
	struct elc_config_slave cfg;
	ec_slave_config_t *ec_config;
	struct elc_domain_node *domain;
	atomic_t state_result;
	atomic_t state_online;
	atomic_t state_operational;
	atomic_t state_al_state;
};

struct elc_sync_node {
	struct elc_config_node common;
	struct elc_config_sync cfg;
};

struct elc_pdo_node {
	struct elc_config_node common;
	struct elc_config_pdo cfg;
};

struct elc_entry_node {
	struct elc_config_node common;
	struct elc_config_entry cfg;
	u32 domain_offset;
	u8 bit_position;
	bool registered;
};

struct elc_dc_node {
	struct elc_config_node common;
	struct elc_config_dc cfg;
};

struct elc_domain_node {
	struct elc_config_node common;
	struct elc_config_domain cfg;
	struct elc_output_authority authority;
	ec_domain_t *ec_domain;
	u8 *data;
	u32 base_offset;
	u32 size;
	atomic_t working_counter;
	atomic_t working_counter_state;
};

struct elc_domain_assignment_node {
	struct elc_config_node common;
	struct elc_config_domain_assignment cfg;
};

static atomic_t elc_control_open = ATOMIC_INIT(0);
static atomic64_t elc_next_config_generation = ATOMIC64_INIT(0);
static atomic_t elc_test_allocation_count = ATOMIC_INIT(0);
static int elc_test_fail_allocation;
static bool elc_test_fail_cycle_thread;
static int elc_cycle_cpu = -1;
static uint elc_cycle_fifo_priority;
module_param_named(test_fail_allocation, elc_test_fail_allocation, int, 0444);
MODULE_PARM_DESC(test_fail_allocation,
		 "fail the Nth module-owned allocation (test only; 0 disables)");
module_param_named(test_fail_cycle_thread, elc_test_fail_cycle_thread,
		   bool, 0444);
MODULE_PARM_DESC(test_fail_cycle_thread,
		 "fail cyclic task construction after activation (test only)");
module_param_named(cycle_cpu, elc_cycle_cpu, int, 0444);
MODULE_PARM_DESC(cycle_cpu,
		 "cyclic task CPU affinity (-1 leaves scheduler affinity unchanged)");
module_param_named(cycle_fifo_priority, elc_cycle_fifo_priority, uint, 0444);
MODULE_PARM_DESC(cycle_fifo_priority,
		 "cyclic task SCHED_FIFO priority (0 leaves normal scheduling)");

static int elc_check_header(u16 struct_size, u16 api_major,
			      size_t expected_size);

static bool elc_should_fail_allocation(void)
{
	int allocation;

	if (elc_test_fail_allocation <= 0)
		return false;
	allocation = atomic_inc_return(&elc_test_allocation_count);
	return allocation == elc_test_fail_allocation;
}

static void *elc_kzalloc(size_t size)
{
	if (elc_should_fail_allocation())
		return NULL;
	return kzalloc(size, GFP_KERNEL);
}

static void *elc_kvzalloc(size_t size)
{
	if (elc_should_fail_allocation())
		return NULL;
	return kvzalloc(size, GFP_KERNEL);
}


static struct elc_slave_node *elc_find_slave(struct elc_file *ctx,
					      u32 config_id);

static void elc_authority_init(struct elc_output_authority *authority)
{
	memset(authority, 0, sizeof(*authority));
	spin_lock_init(&authority->lock);
	init_waitqueue_head(&authority->gate_wait);
	authority->reader = -1;
	authority->initialised = true;
	atomic_set(&authority->armed, 0);
	atomic_set(&authority->rearm_required, 0);
	atomic_set(&authority->healthy, 0);
	atomic_set(&authority->current_faults, 0);
	atomic_set(&authority->last_latched_faults, 0);
	atomic_set(&authority->lease_remaining_cycles, 0);
	atomic64_set(&authority->sequence, 0);
	atomic64_set(&authority->fault_output_sequence, 0);
	atomic64_set(&authority->gate_request, 0);
	atomic64_set(&authority->gate_applied, 0);
	atomic64_set(&authority->lease_renewal_count, 0);
	atomic64_set(&authority->lease_expiry_count, 0);
}

static void elc_authority_free_buffers(struct elc_output_authority *authority)
{
	if (!authority)
		return;
	kvfree(authority->buffers[0]);
	kvfree(authority->buffers[1]);
	kvfree(authority->mask);
	kvfree(authority->update_mask);
	authority->buffers[0] = NULL;
	authority->buffers[1] = NULL;
	authority->mask = NULL;
	authority->update_mask = NULL;
	authority->active = 0;
	authority->reader = -1;
	authority->image_size = 0;
	atomic64_set(&authority->sequence, 0);
}

static int elc_authority_alloc_buffers(struct elc_output_authority *authority,
					 u32 size)
{
	authority->buffers[0] = elc_kvzalloc(size);
	authority->buffers[1] = elc_kvzalloc(size);
	authority->mask = elc_kvzalloc(size);
	authority->update_mask = elc_kvzalloc(size);
	if (!authority->buffers[0] || !authority->buffers[1] ||
	    !authority->mask || !authority->update_mask) {
		elc_authority_free_buffers(authority);
		return -ENOMEM;
	}
	authority->image_size = size;
	return 0;
}

static void elc_free_all_output_buffers(struct elc_file *ctx)
{
	struct elc_domain_node *domain;

	list_for_each_entry(domain, &ctx->config_domains, common.node)
		elc_authority_free_buffers(&domain->authority);
}

static int elc_wait_authority_gate(struct elc_output_authority *authority,
				     u64 request, u32 cycle_period_ns)
{
	unsigned long timeout;
	long wait_result;

	timeout = msecs_to_jiffies(
		2U * DIV_ROUND_UP(cycle_period_ns, 1000000U) + 100U);
	wait_result = wait_event_killable_timeout(
		authority->gate_wait,
		atomic64_read(&authority->gate_applied) >= request,
		timeout);
	if (wait_result < 0)
		return wait_result;
	if (!wait_result)
		return -ETIMEDOUT;
	return 0;
}

static void elc_disarm_authority(struct elc_output_authority *authority,
				   u32 faults)
{
	atomic_set(&authority->armed, 0);
	atomic64_inc(&authority->gate_request);
	if (atomic_xchg(&authority->rearm_required, 1))
		atomic_or(faults, &authority->last_latched_faults);
	else
		atomic_set(&authority->last_latched_faults, faults);
	atomic64_set(&authority->fault_output_sequence,
		     atomic64_read(&authority->sequence));
}


static void elc_invalidate_applied_config(struct elc_file *ctx)
{
	struct elc_domain_node *domain;
	struct elc_entry_node *entry;
	struct elc_slave_node *slave;

	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		slave->ec_config = NULL;
		slave->domain = NULL;
	}
	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		domain->ec_domain = NULL;
		domain->data = NULL;
		domain->base_offset = 0;
		domain->size = 0;
	}
	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		entry->domain_offset = 0;
		entry->bit_position = 0;
		entry->registered = false;
	}
	ctx->config_applied = false;
	ctx->config_poisoned = false;
	ctx->domain = NULL;
	ctx->domain_registered = false;
	ctx->domain_data = NULL;
	ctx->domain_size = 0;
}

static void elc_free_input_history(struct elc_file *ctx);

static void elc_free_input_buffers(struct elc_file *ctx)
{
	kvfree(ctx->input_buffers[0]);
	kvfree(ctx->input_buffers[1]);
	ctx->input_buffers[0] = NULL;
	ctx->input_buffers[1] = NULL;
	ctx->input_active = 0;
	ctx->input_reader = -1;
	ctx->input_cycle_index[0] = 0;
	ctx->input_cycle_index[1] = 0;
	atomic64_set(&ctx->input_sequence, 0);
	elc_free_input_history(ctx);
}

static void elc_free_input_history(struct elc_file *ctx)
{
	struct elc_input_history *history = &ctx->input_history;

	kvfree(history->data);
	kvfree(history->slot_state);
	kvfree(history->records);
	history->data = NULL;
	history->slot_state = NULL;
	history->records = NULL;
	history->depth = 0;
	history->latest_cycle_index = 0;
	atomic64_set(&history->capture_drop_count, 0);
}

static int elc_allocate_input_history(struct elc_file *ctx,
					size_t image_size)
{
	struct elc_input_history *history = &ctx->input_history;
	size_t data_size;

	if (!history->configured_depth)
		return 0;
	if (check_mul_overflow((size_t)history->configured_depth,
			       image_size, &data_size) ||
	    data_size > ELC_INPUT_HISTORY_BYTES_MAX)
		return -E2BIG;
	history->data = elc_kvzalloc(data_size);
	history->slot_state =
		elc_kvzalloc(history->configured_depth);
	history->records = elc_kvzalloc(
		array_size(history->configured_depth,
			   sizeof(*history->records)));
	if (!history->data || !history->slot_state || !history->records) {
		elc_free_input_history(ctx);
		return -ENOMEM;
	}
	history->depth = history->configured_depth;
	return 0;
}

static void elc_free_output_buffers(struct elc_file *ctx)
{
	elc_free_all_output_buffers(ctx);
}

static void elc_setup_clear(struct elc_file *ctx)
{
	struct elc_setup_entry *entry;
	struct elc_setup_entry *next;

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

static void elc_config_clear(struct elc_file *ctx)
{
	struct elc_config_node *entry;
	struct elc_config_node *next;

#define ELC_CLEAR_CONFIG_LIST(name) \
	list_for_each_entry_safe(entry, next, &ctx->name, node) { \
		list_del(&entry->node); \
		kfree(entry); \
	}

	ELC_CLEAR_CONFIG_LIST(config_entries);
	ELC_CLEAR_CONFIG_LIST(config_dcs);
	ELC_CLEAR_CONFIG_LIST(config_domain_assignments);
	ELC_CLEAR_CONFIG_LIST(config_domains);
	ELC_CLEAR_CONFIG_LIST(config_pdos);
	ELC_CLEAR_CONFIG_LIST(config_syncs);
	ELC_CLEAR_CONFIG_LIST(config_slaves);
#undef ELC_CLEAR_CONFIG_LIST

	ctx->config_slave_count = 0;
	ctx->config_sync_count = 0;
	ctx->config_pdo_count = 0;
	ctx->config_entry_count = 0;
	ctx->config_dc_count = 0;
	ctx->config_domain_count = 0;
	ctx->config_domain_assignment_count = 0;
	ctx->config_generation = 0;
	ctx->input_history.configured_depth = 0;
	memset(&ctx->dc_policy, 0, sizeof(ctx->dc_policy));
	ctx->dc_policy_set = false;
	ctx->config_started = false;
	ctx->config_validated = false;
	ctx->config_applied = false;
	ctx->config_poisoned = false;
	ctx->domain = NULL;
	ctx->domain_registered = false;
}

static void elc_update_maximum(atomic64_t *maximum, u64 value)
{
	s64 observed = atomic64_read(maximum);

	while (value > observed) {
		s64 previous = atomic64_cmpxchg(maximum, observed, value);

		if (previous == observed)
			break;
		observed = previous;
	}
}

static int elc_dc_process_receive(struct elc_file *ctx)
{
	u32 reference = 0;
	int ret;

	ret = ecrt_master_reference_clock_time(ctx->master, &reference);
	if (!ret) {
		s32 difference = (s32)((u32)ctx->application_time_ns -
				       reference);
		s64 normalized = (s64)difference % ctx->cycle_period_ns;

		if (normalized > ctx->cycle_period_ns / 2)
			normalized -= ctx->cycle_period_ns;
		if (normalized < -(s64)ctx->cycle_period_ns / 2)
			normalized += ctx->cycle_period_ns;
		difference = normalized;

		if (ctx->dc_reference_valid) {
			ctx->dc_difference_total_ns += difference;
			ctx->dc_delta_total_ns +=
				(s64)difference - ctx->dc_last_difference_ns;
			if (++ctx->dc_filter_count >= 1024) {
				ctx->dc_cycle_adjustment_ns +=
					div_s64(ctx->dc_delta_total_ns, 1024);
				ctx->dc_cycle_adjustment_ns +=
					(ctx->dc_difference_total_ns > 0) -
					(ctx->dc_difference_total_ns < 0);
				ctx->dc_cycle_adjustment_ns =
					clamp_t(s64,
						ctx->dc_cycle_adjustment_ns,
						-1000, 1000);
				ctx->dc_difference_total_ns = 0;
				ctx->dc_delta_total_ns = 0;
				ctx->dc_filter_count = 0;
			}
		}
		ctx->dc_last_difference_ns = difference;
		ctx->dc_last_reference_sample = reference;
		ctx->dc_reference_valid = true;
		if (ctx->dc_last_reference_result)
			atomic64_inc(&ctx->dc_reference_resume_count);
	} else {
		ctx->dc_reference_valid = false;
		atomic64_inc(&ctx->dc_reference_read_error_count);
		ctx->dc_last_reference_sample = 0;
		ctx->dc_last_difference_ns = 0;
	}
	ctx->dc_last_reference_result = ret;
	atomic_set(&ctx->dc_status_reference_valid,
		   ctx->dc_reference_valid);
	atomic_set(&ctx->dc_status_last_reference_result, ret);
	atomic_set(&ctx->dc_status_last_difference_ns,
		   ctx->dc_last_difference_ns);
	atomic_set(&ctx->dc_status_cycle_adjustment_ns,
		   ctx->dc_cycle_adjustment_ns);

	if (ctx->dc_monitor_pending) {
		u32 deviation =
			ecrt_master_sync_monitor_process(ctx->master);

		if (deviation != U32_MAX) {
			atomic_set(&ctx->dc_status_last_maximum_deviation_ns,
				   deviation);
			elc_update_maximum(
				&ctx->dc_status_maximum_deviation_ns,
				deviation);
			atomic64_inc(&ctx->dc_monitor_success_count);
			ctx->dc_monitor_pending = false;
			ctx->dc_monitor_wait_cycles = 0;
		} else if (++ctx->dc_monitor_wait_cycles >= 10) {
			atomic64_inc(&ctx->dc_monitor_timeout_count);
			ctx->dc_monitor_pending = false;
			ctx->dc_monitor_wait_cycles = 0;
		}
		atomic_set(&ctx->dc_status_monitor_pending,
			   ctx->dc_monitor_pending);
	}
	return 0;
}

static int elc_dc_prepare_send(struct elc_file *ctx)
{
	s64 phase_step = (ctx->dc_last_difference_ns > 0) -
			 (ctx->dc_last_difference_ns < 0);
	int ret;
	int operation_result;

	ctx->application_time_ns +=
		ctx->cycle_period_ns - ctx->dc_cycle_adjustment_ns - phase_step;
	ctx->dc_last_applied_adjustment_ns =
		ctx->dc_cycle_adjustment_ns + phase_step;
	ret = ecrt_master_application_time(ctx->master,
					  ctx->application_time_ns);
	operation_result = ecrt_master_sync_slave_clocks(ctx->master);
	if (operation_result && !ret)
		ret = operation_result;

	if (!ctx->dc_monitor_pending &&
	    ctx->dc_monitor_countdown-- == 0) {
		operation_result =
			ecrt_master_sync_monitor_queue(ctx->master);
		if (!operation_result) {
			ctx->dc_monitor_pending = true;
			ctx->dc_monitor_wait_cycles = 0;
			atomic_set(&ctx->dc_status_monitor_pending, 1);
		} else if (!ret) {
			ret = operation_result;
		}
		ctx->dc_monitor_countdown =
			DIV_ROUND_UP(1000000000U, ctx->cycle_period_ns);
	}
	return ret;
}

/*
 * Per-domain bus firewall.
 *
 * Master/link faults still apply to every domain. Beyond that, a domain is
 * isolated by its own working counter:
 *
 * - complete WC  => domain exchange is live; do not fail the domain on
 *   transient ecrt_slave_config_state offline during topology re-scan when
 *   another domain (e.g. drives) drops off the bus;
 * - incomplete WC => domain is failed; then also OR that domain's slave
 *   online/OP bits for diagnosis.
 *
 * Domain 2 offline must not clear domain 1 health when domain 1 WC stays
 * complete.
 */
static u32 elc_domain_bus_faults(struct elc_file *ctx,
				   struct elc_domain_node *domain,
				   u32 master_faults)
{
	struct elc_slave_node *slave;
	u32 faults = master_faults;
	bool wc_complete =
		atomic_read(&domain->working_counter_state) == EC_WC_COMPLETE;

	if (!wc_complete)
		faults |= ELC_IO_FAULT_DOMAIN_INCOMPLETE;

	if (wc_complete)
		return faults;

	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		if (slave->domain != domain)
			continue;
		if (atomic_read(&slave->state_result))
			faults |= ELC_IO_FAULT_SLAVE_STATE;
		if (!atomic_read(&slave->state_online))
			faults |= ELC_IO_FAULT_SLAVE_OFFLINE;
		if (!atomic_read(&slave->state_operational))
			faults |= ELC_IO_FAULT_SLAVE_NOT_OPERATIONAL;
	}
	return faults;
}

static void elc_update_io_health(struct elc_file *ctx)
{
	ec_master_state_t master_state = {};
	struct elc_domain_node *domain;
	struct elc_slave_node *slave;
	u32 online = 0;
	u32 operational = 0;
	u32 aggregate_faults = 0;
	bool all_healthy = true;
	bool any_armed = false;
	u32 master_faults = 0;
	int ret;

	ret = ecrt_master_state(ctx->master, &master_state);
	if (ret)
		master_faults |= ELC_IO_FAULT_MASTER_STATE;
	else if (!master_state.link_up)
		master_faults |= ELC_IO_FAULT_LINK_DOWN;

	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		ec_slave_config_state_t state = {};

		ret = ecrt_slave_config_state(slave->ec_config, &state);
		atomic_set(&slave->state_result, ret);
		if (ret) {
			atomic_set(&slave->state_online, 0);
			atomic_set(&slave->state_operational, 0);
			atomic_set(&slave->state_al_state, 0);
			continue;
		}
		atomic_set(&slave->state_online, state.online);
		atomic_set(&slave->state_operational, state.operational);
		atomic_set(&slave->state_al_state, state.al_state);
		if (state.online)
			online++;
		if (state.operational)
			operational++;
	}

	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		struct elc_output_authority *authority = &domain->authority;
		u32 faults = elc_domain_bus_faults(ctx, domain, master_faults);
		bool was_healthy;

		atomic_set(&authority->current_faults, faults);
		was_healthy = atomic_read(&authority->healthy);
		if (!faults) {
			authority->ever_healthy = true;
			atomic_set(&authority->healthy, 1);
		} else {
			atomic_set(&authority->healthy, 0);
			if (was_healthy || authority->ever_healthy) {
				if (atomic_read(&authority->armed) ||
				    !atomic_read(&authority->rearm_required))
					atomic64_inc(&ctx->io_fault_count);
				elc_disarm_authority(authority, faults);
			} else {
				atomic_set(&authority->armed, 0);
			}
			all_healthy = false;
		}
		if (atomic_read(&authority->armed))
			any_armed = true;
		aggregate_faults |= faults;
	}

	atomic_set(&ctx->io_link_up, master_state.link_up);
	atomic_set(&ctx->io_slaves_responding,
		   master_state.slaves_responding);
	atomic_set(&ctx->io_configured_slaves_online, online);
	atomic_set(&ctx->io_configured_slaves_operational, operational);
	atomic_set(&ctx->io_current_faults, aggregate_faults);
	atomic_set(&ctx->io_bus_healthy, all_healthy ? 1 : 0);
	(void)any_armed;
}

static bool elc_publish_input_snapshot(struct elc_file *ctx,
					 u64 cycle_index)
{
	struct elc_domain_node *domain;
	unsigned long irq_flags;
	u8 target;

	spin_lock_irqsave(&ctx->input_lock, irq_flags);
	target = ctx->input_active ^ 1U;
	if (ctx->input_reader == target) {
		spin_unlock_irqrestore(&ctx->input_lock, irq_flags);
		return false;
	}
	spin_unlock_irqrestore(&ctx->input_lock, irq_flags);

	list_for_each_entry(domain, &ctx->config_domains, common.node)
		memcpy(ctx->input_buffers[target] + domain->base_offset,
		       domain->data, domain->size);

	spin_lock_irqsave(&ctx->input_lock, irq_flags);
	ctx->input_active = target;
	ctx->input_cycle_index[target] = cycle_index;
	atomic64_inc(&ctx->input_sequence);
	spin_unlock_irqrestore(&ctx->input_lock, irq_flags);
	return true;
}

/*
 * Refill hang-failsafe budget. Successful publish and arm call this so
 * userspace need not issue a high-rate renew ioctl. No-op renews (already
 * full) do not bump renewal_count.
 */
static void elc_authority_refill_lease(struct elc_output_authority *authority)
{
	u32 budget;
	int remaining;

	if (!authority || !authority->lease_configured_cycles)
		return;
	budget = authority->lease_configured_cycles;
	remaining = atomic_read(&authority->lease_remaining_cycles);
	if (remaining < (int)budget) {
		atomic_set(&authority->lease_remaining_cycles, budget);
		atomic64_inc(&authority->lease_renewal_count);
	} else {
		atomic_set(&authority->lease_remaining_cycles, budget);
	}
	atomic_and(~ELC_IO_FAULT_CONTROLLER_STALE,
		   &authority->current_faults);
}

static u32 elc_lease_budget_from_request(struct elc_file *ctx,
					   u32 cycle_budget, u32 timeout_ms)
{
	u32 period_ns;
	u64 derived;

	if (timeout_ms) {
		period_ns = READ_ONCE(ctx->cycle_period_ns);
		if (!period_ns) {
			/* Pre-activate: require an explicit cycle_budget. */
			if (!cycle_budget)
				return U32_MAX; /* signal EINVAL to caller */
			return cycle_budget;
		}
		derived = div_u64((u64)timeout_ms * 1000000ULL, period_ns);
		if (!derived)
			derived = 1;
		if (derived > ELC_OUTPUT_LEASE_CYCLES_MAX)
			derived = ELC_OUTPUT_LEASE_CYCLES_MAX;
		if (cycle_budget && cycle_budget < derived)
			return cycle_budget;
		return (u32)derived;
	}
	return cycle_budget;
}

static void
elc_expire_output_lease(struct elc_file *ctx,
			  struct elc_output_authority *authority)
{
	if (atomic_cmpxchg(&authority->armed, 1, 0) != 1)
		return;

	atomic_set(&authority->lease_remaining_cycles, 0);
	atomic_or(ELC_IO_FAULT_CONTROLLER_STALE,
		  &authority->current_faults);
	if (atomic_xchg(&authority->rearm_required, 1))
		atomic_or(ELC_IO_FAULT_CONTROLLER_STALE,
			  &authority->last_latched_faults);
	else
		atomic_set(&authority->last_latched_faults,
			   ELC_IO_FAULT_CONTROLLER_STALE);
	atomic64_set(&authority->fault_output_sequence,
		     atomic64_read(&authority->sequence));
	atomic64_inc(&authority->lease_expiry_count);
	atomic64_inc(&authority->gate_request);
	atomic64_inc(&ctx->io_fault_count);
}

static u64 elc_apply_outputs(struct elc_file *ctx)
{
	struct elc_domain_node *domain;
	unsigned long irq_flags;
	u64 output_sequence_consumed = 0;
	u32 i;

	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		struct elc_output_authority *authority = &domain->authority;
		u8 *source = NULL;
		u8 reader = 0;

		if (atomic_read(&authority->armed) &&
		    authority->lease_configured_cycles) {
			if (atomic_read(&authority->lease_remaining_cycles) <=
			    0)
				elc_expire_output_lease(ctx, authority);
			else
				atomic_dec(&authority->lease_remaining_cycles);
		}
		if (atomic_read(&authority->armed) &&
		    atomic_read(&authority->healthy) &&
		    authority->buffers[0]) {
			spin_lock_irqsave(&authority->lock, irq_flags);
			reader = authority->active;
			authority->reader = reader;
			source = authority->buffers[reader];
			if (atomic64_read(&authority->sequence))
				output_sequence_consumed =
					atomic64_read(&authority->sequence);
			spin_unlock_irqrestore(&authority->lock, irq_flags);
		}
		if (!domain->size || !domain->data)
			continue;
		if (source) {
			for (i = 0; i < domain->size; i++) {
				u8 output = source[i];

				domain->data[i] =
					(domain->data[i] &
					 ~authority->mask[i]) |
					(output & authority->mask[i]);
			}
			spin_lock_irqsave(&authority->lock, irq_flags);
			authority->reader = -1;
			spin_unlock_irqrestore(&authority->lock, irq_flags);
			if (output_sequence_consumed &&
			    output_sequence_consumed ==
				    authority->last_sequence_consumed)
				authority->stale_cycles++;
			authority->last_sequence_consumed =
				output_sequence_consumed;
		} else {
			/* Disarmed or unhealthy: zero domain process data. */
			memset(domain->data, 0, domain->size);
		}
	}
	return output_sequence_consumed;
}

static void elc_publish_cycle_info(struct elc_file *ctx, u64 cycle_index,
				     u32 cycle_period_ns,
				     u64 scheduled_time_ns,
				     u64 actual_wake_time_ns,
				     s64 wake_lateness_ns,
				     u64 output_sequence_consumed,
				     int cycle_result)
{
	struct elc_domain_node *domain;
	u64 stale_sum = 0;
	u8 any_armed = 0;
	struct elc_cycle_info info;
	unsigned long irq_flags;

	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		stale_sum += domain->authority.stale_cycles;
		if (atomic_read(&domain->authority.armed))
			any_armed = 1;
	}
	info = (struct elc_cycle_info){
		.struct_size = sizeof(info),
		.api_major = ELC_API_VERSION_MAJOR,
		.config_generation = ctx->config_generation,
		.cycle_index = cycle_index,
		.cycle_period_ns = cycle_period_ns,
		.scheduled_time_ns = scheduled_time_ns,
		.actual_wake_time_ns = actual_wake_time_ns,
		.wake_lateness_ns = wake_lateness_ns,
		.input_sequence = atomic64_read(&ctx->input_sequence),
		.output_sequence_consumed = output_sequence_consumed,
		.missed_deadlines =
			atomic64_read(&ctx->cycle_overrun_count),
		.stale_output_cycles = stale_sum,
		.working_counter = atomic_read(&ctx->working_counter),
		.working_counter_state =
			atomic_read(&ctx->working_counter_state),
		.outputs_armed = any_armed,
		.bus_healthy = atomic_read(&ctx->io_bus_healthy),
		.cycle_result = cycle_result,
	};

	spin_lock_irqsave(&ctx->cycle_info_lock, irq_flags);
	ctx->cycle_info = info;
	/*
	 * Snapshot DC motion-clock fields under the same lock as cycle_info so
	 * GET_DC_INFO cannot observe a torn half-cycle record.
	 */
	if (ctx->config_dc_count) {
		ctx->dc_published_app_time_ns = ctx->application_time_ns;
		ctx->dc_published_reference_valid =
			ctx->dc_reference_valid ? 1 : 0;
		ctx->dc_published_reference_sample =
			ctx->dc_last_reference_sample;
		ctx->dc_published_phase_difference_ns =
			ctx->dc_last_difference_ns;
		ctx->dc_published_applied_adjustment_ns =
			ctx->dc_last_applied_adjustment_ns;
	} else {
		ctx->dc_published_app_time_ns = 0;
		ctx->dc_published_reference_valid = 0;
		ctx->dc_published_reference_sample = 0;
		ctx->dc_published_phase_difference_ns = 0;
		ctx->dc_published_applied_adjustment_ns = 0;
	}
	spin_unlock_irqrestore(&ctx->cycle_info_lock, irq_flags);
	/* Publish the wait predicate only after the coherent record above. */
	atomic64_set(&ctx->cycle_info_sequence, cycle_index);
	wake_up_interruptible(&ctx->cycle_wait);
}

static void elc_publish_input_history(struct elc_file *ctx,
					u64 cycle_index,
					u64 scheduled_time_ns,
					u64 actual_wake_time_ns,
					s64 wake_lateness_ns,
					int cycle_result)
{
	struct elc_input_history *history = &ctx->input_history;
	struct elc_input_history_record record;
	unsigned long irq_flags;
	u32 slot;
	u8 source;

	if (!history->depth)
		return;
	slot = cycle_index % history->depth;
	spin_lock_irqsave(&history->lock, irq_flags);
	history->latest_cycle_index = cycle_index;
	if (history->slot_state[slot] != ELC_HISTORY_SLOT_FREE) {
		atomic64_inc(&history->capture_drop_count);
		spin_unlock_irqrestore(&history->lock, irq_flags);
		return;
	}
	history->slot_state[slot] = ELC_HISTORY_SLOT_WRITER;
	spin_unlock_irqrestore(&history->lock, irq_flags);

	spin_lock_irqsave(&ctx->input_lock, irq_flags);
	source = ctx->input_active;
	spin_unlock_irqrestore(&ctx->input_lock, irq_flags);
	memcpy(history->data + (size_t)slot * ctx->domain_size,
	       ctx->input_buffers[source], ctx->domain_size);
	memset(&record, 0, sizeof(record));
	record.config_generation = ctx->config_generation;
	record.cycle_index = cycle_index;
	record.input_sequence = atomic64_read(&ctx->input_sequence);
	record.scheduled_time_ns = scheduled_time_ns;
	record.actual_wake_time_ns = actual_wake_time_ns;
	record.wake_lateness_ns = wake_lateness_ns;
	record.cycle_result = cycle_result;

	spin_lock_irqsave(&history->lock, irq_flags);
	history->records[slot] = record;
	history->slot_state[slot] = ELC_HISTORY_SLOT_FREE;
	spin_unlock_irqrestore(&history->lock, irq_flags);
}

static int elc_cycle_thread(void *data)
{
	struct elc_file *ctx = data;
	u64 deadline = ktime_get_ns();

	while (!kthread_should_stop()) {
		struct elc_domain_node *domain;
		ec_domain_state_t aggregate_state = {};
		bool domains_complete = true;
		bool input_published = false;
		ktime_t expires;
		u64 now;
		u64 scheduled_time_ns;
		u64 output_sequence_consumed;
		u64 cycle_index;
		u32 cycle_period_ns;
		s64 wake_lateness;
		int cycle_result = 0;
		int operation_result;

		cycle_period_ns = READ_ONCE(ctx->cycle_period_ns);
		deadline += cycle_period_ns;
		scheduled_time_ns = deadline;
		expires = ns_to_ktime(deadline);
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_hrtimeout(&expires, HRTIMER_MODE_ABS);
		__set_current_state(TASK_RUNNING);
		if (kthread_should_stop())
			break;

		now = ktime_get_ns();
		cycle_index = atomic64_read(&ctx->cycle_count) + 1;
		wake_lateness = now >= scheduled_time_ns ?
			(s64)(now - scheduled_time_ns) :
			-(s64)(scheduled_time_ns - now);
		if (now > deadline) {
			u64 lateness = now - deadline;

			elc_update_maximum(&ctx->maximum_lateness_ns,
					     lateness);
			if (lateness >= cycle_period_ns) {
				atomic64_inc(&ctx->cycle_overrun_count);
				deadline = now;
			}
		}

		operation_result = ecrt_master_receive(ctx->master);
		if (operation_result && !cycle_result)
			cycle_result = operation_result;
		list_for_each_entry(domain, &ctx->config_domains, common.node) {
			ec_domain_state_t state = {};

			operation_result =
				ecrt_domain_process(domain->ec_domain);
			if (operation_result && !cycle_result)
				cycle_result = operation_result;
			operation_result =
				ecrt_domain_state(domain->ec_domain, &state);
			if (operation_result && !cycle_result) {
				cycle_result = operation_result;
				domains_complete = false;
			} else if (!operation_result) {
				atomic_set(&domain->working_counter,
					   state.working_counter);
				atomic_set(&domain->working_counter_state,
					   state.wc_state);
				aggregate_state.working_counter +=
					state.working_counter;
				if (state.wc_state != EC_WC_COMPLETE)
					domains_complete = false;
			}
		}
		aggregate_state.wc_state = domains_complete ?
			EC_WC_COMPLETE : EC_WC_INCOMPLETE;
		if (!cycle_result)
			input_published =
				elc_publish_input_snapshot(ctx, cycle_index);
		if (ctx->config_dc_count)
			operation_result = elc_dc_process_receive(ctx);
		else
			operation_result = 0;
		if (operation_result && !cycle_result)
			cycle_result = operation_result;
		atomic_set(&ctx->working_counter,
			   aggregate_state.working_counter);
		atomic_set(&ctx->working_counter_state,
			   aggregate_state.wc_state);
		elc_update_io_health(ctx);
		output_sequence_consumed = elc_apply_outputs(ctx);
		if (ctx->config_dc_count) {
			operation_result = elc_dc_prepare_send(ctx);
		} else {
			ctx->application_time_ns += cycle_period_ns;
			operation_result = ecrt_master_application_time(
				ctx->master, ctx->application_time_ns);
		}
		if (operation_result && !cycle_result)
			cycle_result = operation_result;
		list_for_each_entry(domain, &ctx->config_domains, common.node) {
			operation_result =
				ecrt_domain_queue(domain->ec_domain);
			if (operation_result && !cycle_result)
				cycle_result = operation_result;
		}
		operation_result = ecrt_master_send(ctx->master);
		if (operation_result && !cycle_result)
			cycle_result = operation_result;
		if (!operation_result) {
			list_for_each_entry(domain, &ctx->config_domains,
					    common.node) {
				struct elc_output_authority *auth =
					&domain->authority;

				if (atomic64_read(&auth->gate_applied) !=
				    atomic64_read(&auth->gate_request)) {
					atomic64_set(
						&auth->gate_applied,
						atomic64_read(
							&auth->gate_request));
					wake_up_interruptible(&auth->gate_wait);
				}
			}
		}
		if (cycle_result)
			atomic64_inc(&ctx->cycle_error_count);
		atomic_set(&ctx->last_cycle_result, cycle_result);
		atomic64_set(&ctx->cycle_count, cycle_index);
		elc_publish_cycle_info(ctx, cycle_index, cycle_period_ns,
					 scheduled_time_ns, now,
					 wake_lateness,
					 output_sequence_consumed,
					 cycle_result);
		if (input_published)
			elc_publish_input_history(ctx, cycle_index,
						    scheduled_time_ns, now,
						    wake_lateness,
						    cycle_result);
		if (atomic_read(&ctx->pending_cycle_period_ns)) {
			u32 pending = atomic_xchg(
				&ctx->pending_cycle_period_ns, 0);

			if (pending) {
				u32 previous = cycle_period_ns;
				struct elc_dc_node *dc;

				WRITE_ONCE(ctx->cycle_period_ns, pending);
				if (ctx->config_dc_count) {
					/*
					 * Keep SYNC0 identical to the host
					 * period (activation invariant).
					 * ecrt_slave_config_dc only updates
					 * the stored slave_config; EtherLab
					 * reprograms the ESC on the next
					 * slave configuration pass. Host
					 * application-time stepping and the
					 * DC filter use the new period
					 * immediately.
					 */
					list_for_each_entry(
						dc, &ctx->config_dcs,
						common.node) {
						struct elc_slave_node *slave;

						if (dc->cfg.sync0_cycle_ns ==
						    previous)
							dc->cfg.sync0_cycle_ns =
								pending;
						slave = elc_find_slave(
							ctx,
							dc->cfg.slave_config_id);
						if (!slave ||
						    !slave->ec_config)
							continue;
						ecrt_slave_config_dc(
							slave->ec_config,
							dc->cfg.assign_activate,
							dc->cfg.sync0_cycle_ns,
							dc->cfg.sync0_shift_ns,
							dc->cfg.sync1_cycle_ns,
							dc->cfg.sync1_shift_ns);
					}
					ctx->dc_filter_count = 0;
					ctx->dc_difference_total_ns = 0;
					ctx->dc_delta_total_ns = 0;
					ctx->dc_cycle_adjustment_ns = 0;
					ctx->dc_last_applied_adjustment_ns = 0;
					ctx->dc_monitor_countdown =
						DIV_ROUND_UP(1000000000U,
							     pending);
				}
				atomic64_set(&ctx->period_applied_cycle,
					     cycle_index);
				atomic64_set(
					&ctx->period_applied_sequence,
					atomic64_read(
						&ctx->period_request_sequence));
				wake_up_interruptible(&ctx->period_wait);
			}
		}
	}

	return 0;
}

static int elc_wait_configured_slaves_settled(struct elc_file *ctx)
{
	unsigned long timeout =
		jiffies + msecs_to_jiffies(ELC_DEACTIVATE_SETTLE_MS);

	for (;;) {
		struct elc_slave_node *slave;
		bool settled = true;

		list_for_each_entry(slave, &ctx->config_slaves, common.node) {
			ec_slave_info_t info = {};
			int ret;

			ret = ecrt_master_get_slave(ctx->master,
						    slave->cfg.position, &info);
			if (ret == -ENOENT)
				continue;
			if (ret)
				return ret;
			if (info.al_state & (EC_AL_STATE_SAFEOP |
					     EC_AL_STATE_OP)) {
				settled = false;
				break;
			}
		}
		if (settled)
			return 0;
		if (time_after_eq(jiffies, timeout))
			return -ETIMEDOUT;
		msleep(ELC_DEACTIVATE_POLL_MS);
	}
}

static int elc_wait_output_gate(struct elc_file *ctx, u64 request)
{
	struct elc_domain_node *domain;
	int ret = 0;

	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		int one = elc_wait_authority_gate(&domain->authority, request,
						    ctx->cycle_period_ns);

		if (one && !ret)
			ret = one;
	}
	return ret;
}

static int elc_deactivate_locked(struct elc_file *ctx)
{
	struct elc_domain_node *domain;
	u64 gate_request = 0;
	int gate_ret;
	int process_ret;
	int receive_ret;
	int settle_ret;
	int ret;

	if (!ctx->active)
		return -EINVAL;

	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		struct elc_output_authority *authority = &domain->authority;

		atomic_set(&authority->armed, 0);
		atomic_set(&authority->lease_remaining_cycles, 0);
		gate_request = atomic64_inc_return(&authority->gate_request);
	}
	gate_ret = elc_wait_output_gate(ctx, gate_request);
	kthread_stop(ctx->cycle_thread);
	ctx->cycle_thread = NULL;
	/*
	 * The last cyclic send can still have a response waiting in the NIC.
	 * Consume it while the application callbacks/domain are valid, before
	 * EtherLab restarts its idle thread during deactivation.
	 */
	usleep_range(DIV_ROUND_UP(ctx->cycle_period_ns, 1000U),
		     DIV_ROUND_UP(ctx->cycle_period_ns, 1000U) + 100U);
	receive_ret = ecrt_master_receive(ctx->master);
	process_ret = 0;
	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		int domain_ret = ecrt_domain_process(domain->ec_domain);

		if (domain_ret && !process_ret)
			process_ret = domain_ret;
	}
	ret = ecrt_master_deactivate(ctx->master);
	WRITE_ONCE(ctx->active, false);
	wake_up_interruptible_all(&ctx->cycle_wait);
	settle_ret = ret ? 0 : elc_wait_configured_slaves_settled(ctx);
	elc_free_input_buffers(ctx);
	elc_free_output_buffers(ctx);
	elc_invalidate_applied_config(ctx);
	if (settle_ret)
		ctx->config_poisoned = true;
	if (ret)
		return ret;
	if (settle_ret)
		return settle_ret;
	if (receive_ret)
		return receive_ret;
	if (gate_ret)
		return gate_ret;
	return process_ret;
}

static int elc_open(struct inode *inode, struct file *file)
{
	struct elc_file *ctx;
	ec_master_t *master;

	if (atomic_cmpxchg(&elc_control_open, 0, 1) != 0)
		return -EBUSY;

	ctx = elc_kzalloc(sizeof(*ctx));
	if (!ctx) {
		atomic_set(&elc_control_open, 0);
		return -ENOMEM;
	}

	master = ecrt_request_master(0);
	if (!master) {
		kfree(ctx);
		atomic_set(&elc_control_open, 0);
		return -EBUSY;
	}

	ctx->master = master;
	if (elc_test_fail_allocation)
		atomic_set(&elc_test_allocation_count, 0);
	mutex_init(&ctx->lock);
	spin_lock_init(&ctx->input_lock);
	spin_lock_init(&ctx->cycle_info_lock);
	spin_lock_init(&ctx->input_history.lock);
	init_waitqueue_head(&ctx->cycle_wait);
	init_waitqueue_head(&ctx->period_wait);
	ctx->input_reader = -1;
	INIT_LIST_HEAD(&ctx->setup_sdos);
	INIT_LIST_HEAD(&ctx->config_slaves);
	INIT_LIST_HEAD(&ctx->config_syncs);
	INIT_LIST_HEAD(&ctx->config_pdos);
	INIT_LIST_HEAD(&ctx->config_entries);
	INIT_LIST_HEAD(&ctx->config_dcs);
	INIT_LIST_HEAD(&ctx->config_domains);
	INIT_LIST_HEAD(&ctx->config_domain_assignments);
	atomic64_set(&ctx->cycle_count, 0);
	atomic64_set(&ctx->cycle_error_count, 0);
	atomic64_set(&ctx->cycle_overrun_count, 0);
	atomic64_set(&ctx->maximum_lateness_ns, 0);
	atomic64_set(&ctx->cycle_info_sequence, 0);
	atomic64_set(&ctx->input_history.capture_drop_count, 0);
	atomic_set(&ctx->pending_cycle_period_ns, 0);
	atomic64_set(&ctx->period_request_sequence, 0);
	atomic64_set(&ctx->period_applied_sequence, 0);
	atomic64_set(&ctx->period_applied_cycle, 0);
	memset(&ctx->cycle_info, 0, sizeof(ctx->cycle_info));
	ctx->cycle_info.struct_size = sizeof(ctx->cycle_info);
	ctx->cycle_info.api_major = ELC_API_VERSION_MAJOR;
	ctx->cycle_info.config_generation = ctx->config_generation;
	ctx->cycle_info.cycle_period_ns = ctx->cycle_period_ns;
	atomic_set(&ctx->working_counter, 0);
	atomic_set(&ctx->working_counter_state, EC_WC_ZERO);
	atomic_set(&ctx->last_cycle_result, 0);
	atomic_set(&ctx->dc_status_reference_valid, 0);
	atomic_set(&ctx->dc_status_monitor_pending, 0);
	atomic_set(&ctx->dc_status_last_reference_result, 0);
	atomic_set(&ctx->dc_status_last_difference_ns, 0);
	atomic_set(&ctx->dc_status_cycle_adjustment_ns, 0);
	atomic_set(&ctx->dc_status_last_maximum_deviation_ns, 0);
	atomic64_set(&ctx->dc_status_maximum_deviation_ns, 0);
	atomic64_set(&ctx->dc_reference_read_error_count, 0);
	atomic64_set(&ctx->dc_reference_resume_count, 0);
	atomic64_set(&ctx->dc_monitor_success_count, 0);
	atomic64_set(&ctx->dc_monitor_timeout_count, 0);
	ctx->dc_last_reference_sample = 0;
	ctx->dc_last_applied_adjustment_ns = 0;
	ctx->dc_published_app_time_ns = 0;
	ctx->dc_published_reference_valid = 0;
	ctx->dc_published_reference_sample = 0;
	ctx->dc_published_phase_difference_ns = 0;
	ctx->dc_published_applied_adjustment_ns = 0;
	atomic_set(&ctx->io_bus_healthy, 0);
	atomic_set(&ctx->io_link_up, 0);
	atomic_set(&ctx->io_current_faults, 0);
	atomic_set(&ctx->io_slaves_responding, 0);
	atomic_set(&ctx->io_configured_slaves_online, 0);
	atomic_set(&ctx->io_configured_slaves_operational, 0);
	atomic64_set(&ctx->io_fault_count, 0);
	atomic64_set(&ctx->input_sequence, 0);
	file->private_data = ctx;
	nonseekable_open(inode, file);

	pr_info(ELC_NAME ": control owner acquired master 0\n");
	return 0;
}

static int elc_release(struct inode *inode, struct file *file)
{
	struct elc_file *ctx = file->private_data;

	if (ctx) {
		mutex_lock(&ctx->lock);
		if (ctx->active)
			elc_deactivate_locked(ctx);
		elc_setup_clear(ctx);
		elc_config_clear(ctx);
		if (ctx->master) {
			ecrt_release_master(ctx->master);
			ctx->master = NULL;
		}
		mutex_unlock(&ctx->lock);
		kfree(ctx);
		file->private_data = NULL;
	}

	atomic_set(&elc_control_open, 0);
	pr_info(ELC_NAME ": control owner released master 0\n");
	return 0;
}

static bool elc_config_id_exists(struct list_head *head, u32 config_id)
{
	struct elc_config_node *entry;

	list_for_each_entry(entry, head, node) {
		if (entry->config_id == config_id)
			return true;
	}
	return false;
}

static long elc_config_begin(struct elc_file *ctx, void __user *argp)
{
	struct elc_config_begin request;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = elc_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (request.reserved)
		return -EINVAL;

	mutex_lock(&ctx->lock);
	if (ctx->config_applied || ctx->config_poisoned ||
	    ctx->domain_registered) {
		mutex_unlock(&ctx->lock);
		return -EBUSY;
	}
	elc_config_clear(ctx);
	ctx->config_started = true;
	mutex_unlock(&ctx->lock);
	return 0;
}

static long elc_config_add_slave(struct elc_file *ctx, void __user *argp)
{
	struct elc_slave_node *node;
	int ret;

	node = elc_kzalloc(sizeof(*node));
	if (!node)
		return -ENOMEM;
	if (copy_from_user(&node->cfg, argp, sizeof(node->cfg))) {
		ret = -EFAULT;
		goto out;
	}
	ret = elc_check_header(node->cfg.struct_size, node->cfg.api_major,
				 sizeof(node->cfg));
	if (ret)
		goto out;
	if (!node->cfg.config_id || !node->cfg.vendor_id ||
	    !node->cfg.product_code || node->cfg.revision_number ||
	    node->cfg.flags) {
		ret = -EINVAL;
		goto out;
	}
	node->common.config_id = node->cfg.config_id;

	mutex_lock(&ctx->lock);
	if (!ctx->config_started || ctx->config_validated) {
		ret = -EINVAL;
	} else if (ctx->config_slave_count >= ELC_CONFIG_SLAVE_MAX) {
		ret = -E2BIG;
	} else if (elc_config_id_exists(&ctx->config_slaves,
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

static struct elc_slave_node *
elc_find_slave(struct elc_file *ctx, u32 config_id)
{
	struct elc_slave_node *slave;

	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		if (slave->cfg.config_id == config_id)
			return slave;
	}
	return NULL;
}

static struct elc_domain_node *
elc_find_domain(struct elc_file *ctx, u32 config_id)
{
	struct elc_domain_node *domain;

	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		if (domain->cfg.config_id == config_id)
			return domain;
	}
	return NULL;
}

static struct elc_domain_assignment_node *
elc_find_domain_assignment(struct elc_file *ctx, u32 slave_config_id)
{
	struct elc_domain_assignment_node *assignment;

	list_for_each_entry(assignment, &ctx->config_domain_assignments,
			    common.node) {
		if (assignment->cfg.slave_config_id == slave_config_id)
			return assignment;
	}
	return NULL;
}

static struct elc_sync_node *
elc_find_sync(struct elc_file *ctx, u32 config_id)
{
	struct elc_sync_node *sync;

	list_for_each_entry(sync, &ctx->config_syncs, common.node) {
		if (sync->cfg.config_id == config_id)
			return sync;
	}
	return NULL;
}

static struct elc_pdo_node *
elc_find_pdo(struct elc_file *ctx, u32 config_id)
{
	struct elc_pdo_node *pdo;

	list_for_each_entry(pdo, &ctx->config_pdos, common.node) {
		if (pdo->cfg.config_id == config_id)
			return pdo;
	}
	return NULL;
}

static struct elc_dc_node *
elc_find_dc_for_slave(struct elc_file *ctx, u32 slave_config_id)
{
	struct elc_dc_node *dc;

	list_for_each_entry(dc, &ctx->config_dcs, common.node) {
		if (dc->cfg.slave_config_id == slave_config_id)
			return dc;
	}
	return NULL;
}

static long elc_config_apply(struct elc_file *ctx, void __user *argp)
{
	struct elc_dc_node *dc;
	struct elc_config_apply result;
	struct elc_entry_node *entry;
	struct elc_sync_node *sync;
	struct elc_pdo_node *pdo;
	struct elc_slave_node *slave;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.reserved1 ||
	    memchr_inv(result.reserved0, 0, sizeof(result.reserved0)))
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;

	mutex_lock(&ctx->lock);
	if (!ctx->config_validated || ctx->config_applied ||
	    ctx->config_poisoned) {
		ret = -EINVAL;
		goto out;
	}

	/*
	 * EtherLab has no public operation to remove one partially constructed
	 * slave configuration. Any failure below poisons this master
	 * application; close/reopen releases the master and provides rollback.
	 */
	ctx->config_poisoned = true;

	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		slave->ec_config =
			ecrt_master_slave_config(ctx->master, slave->cfg.alias,
						slave->cfg.position,
						slave->cfg.vendor_id,
						slave->cfg.product_code);
		if (!slave->ec_config) {
			ret = -ENOMEM;
			result.failed_config_id = slave->cfg.config_id;
			result.failed_object_kind = ELC_CONFIG_OBJECT_SLAVE;
			goto out;
		}
	}

	list_for_each_entry(sync, &ctx->config_syncs, common.node) {
		slave = elc_find_slave(ctx, sync->cfg.slave_config_id);
		if (!slave) {
			ret = -ENOENT;
			result.failed_config_id = sync->cfg.config_id;
			result.failed_object_kind = ELC_CONFIG_OBJECT_SYNC;
			goto out;
		}
		ret = ecrt_slave_config_sync_manager(
			slave->ec_config, sync->cfg.sync_index,
			sync->cfg.direction == ELC_DIR_OUTPUT ?
				EC_DIR_OUTPUT : EC_DIR_INPUT,
			sync->cfg.watchdog_mode == ELC_WD_ENABLE ?
				EC_WD_ENABLE :
			sync->cfg.watchdog_mode == ELC_WD_DISABLE ?
				EC_WD_DISABLE : EC_WD_DEFAULT);
		if (ret) {
			result.failed_config_id = sync->cfg.config_id;
			result.failed_object_kind = ELC_CONFIG_OBJECT_SYNC;
			goto out;
		}
		ecrt_slave_config_pdo_assign_clear(slave->ec_config,
						  sync->cfg.sync_index);
	}

	list_for_each_entry(pdo, &ctx->config_pdos, common.node) {
		sync = elc_find_sync(ctx, pdo->cfg.sync_config_id);
		if (!sync) {
			ret = -ENOENT;
			result.failed_config_id = pdo->cfg.config_id;
			result.failed_object_kind = ELC_CONFIG_OBJECT_PDO;
			goto out;
		}
		slave = elc_find_slave(ctx, sync->cfg.slave_config_id);
		if (!slave) {
			ret = -ENOENT;
			result.failed_config_id = pdo->cfg.config_id;
			result.failed_object_kind = ELC_CONFIG_OBJECT_PDO;
			goto out;
		}
		ret = ecrt_slave_config_pdo_assign_add(
			slave->ec_config, sync->cfg.sync_index,
			pdo->cfg.pdo_index);
		if (ret) {
			result.failed_config_id = pdo->cfg.config_id;
			result.failed_object_kind = ELC_CONFIG_OBJECT_PDO;
			goto out;
		}
		ecrt_slave_config_pdo_mapping_clear(slave->ec_config,
						   pdo->cfg.pdo_index);
	}

	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		pdo = elc_find_pdo(ctx, entry->cfg.pdo_config_id);
		if (!pdo) {
			ret = -ENOENT;
			result.failed_config_id = entry->cfg.config_id;
			result.failed_object_kind = ELC_CONFIG_OBJECT_ENTRY;
			goto out;
		}
		sync = elc_find_sync(ctx, pdo->cfg.sync_config_id);
		slave = sync ?
			elc_find_slave(ctx, sync->cfg.slave_config_id) : NULL;
		if (!slave) {
			ret = -ENOENT;
			result.failed_config_id = entry->cfg.config_id;
			result.failed_object_kind = ELC_CONFIG_OBJECT_ENTRY;
			goto out;
		}
		ret = ecrt_slave_config_pdo_mapping_add(
			slave->ec_config, pdo->cfg.pdo_index,
			entry->cfg.index, entry->cfg.subindex,
			entry->cfg.bit_length);
		if (ret) {
			result.failed_config_id = entry->cfg.config_id;
			result.failed_object_kind = ELC_CONFIG_OBJECT_ENTRY;
			goto out;
		}
	}

	list_for_each_entry(dc, &ctx->config_dcs, common.node) {
		slave = elc_find_slave(ctx, dc->cfg.slave_config_id);
		if (!slave) {
			ret = -ENOENT;
			result.failed_config_id = dc->cfg.config_id;
			result.failed_object_kind = ELC_CONFIG_OBJECT_DC;
			goto out;
		}
		ret = ecrt_slave_config_dc(slave->ec_config,
					   dc->cfg.assign_activate,
					   dc->cfg.sync0_cycle_ns,
					   dc->cfg.sync0_shift_ns,
					   dc->cfg.sync1_cycle_ns,
					   dc->cfg.sync1_shift_ns);
		if (ret) {
			result.failed_config_id = dc->cfg.config_id;
			result.failed_object_kind = ELC_CONFIG_OBJECT_DC;
			goto out;
		}
	}

	if (ctx->dc_policy.reference_mode != ELC_DC_REFERENCE_DISABLED) {
		ec_slave_config_t *reference = NULL;

		if (ctx->dc_policy.reference_mode ==
		    ELC_DC_REFERENCE_EXPLICIT) {
			slave = elc_find_slave(
				ctx, ctx->dc_policy.reference_slave_config_id);
			if (!slave) {
				ret = -ENOENT;
				result.failed_config_id =
					ctx->dc_policy.reference_slave_config_id;
				result.failed_object_kind =
					ELC_CONFIG_OBJECT_DC_POLICY;
				goto out;
			}
			reference = slave->ec_config;
		}
		ret = ecrt_master_select_reference_clock(ctx->master, reference);
		if (ret) {
			result.failed_config_id =
				ctx->dc_policy.reference_slave_config_id;
			result.failed_object_kind =
				ELC_CONFIG_OBJECT_DC_POLICY;
			goto out;
		}
	}

	ctx->config_applied = true;
	ctx->config_poisoned = false;
	ret = 0;
out:
	result.result = ret;
	if (copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static unsigned int elc_pdo_position(struct elc_file *ctx,
				       const struct elc_pdo_node *target)
{
	struct elc_pdo_node *pdo;
	unsigned int position = 0;

	list_for_each_entry(pdo, &ctx->config_pdos, common.node) {
		if (pdo == target)
			break;
		if (pdo->cfg.sync_config_id == target->cfg.sync_config_id)
			position++;
	}
	return position;
}

static unsigned int elc_entry_position(struct elc_file *ctx,
					 const struct elc_entry_node *target)
{
	struct elc_entry_node *entry;
	unsigned int position = 0;

	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		if (entry == target)
			break;
		if (entry->cfg.pdo_config_id == target->cfg.pdo_config_id)
			position++;
	}
	return position;
}

static long elc_domain_create(struct elc_file *ctx, void __user *argp)
{
	struct elc_domain_create result;
	struct elc_domain_assignment_node *assignment;
	struct elc_domain_node *domain;
	struct elc_entry_node *entry;
	struct elc_slave_node *slave;
	struct elc_sync_node *sync;
	struct elc_pdo_node *pdo;
	unsigned int bit_position;
	bool has_registered_entry = false;
	u64 total_size = 0;
	int offset;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.reserved)
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;

	mutex_lock(&ctx->lock);
	if (!ctx->config_applied || ctx->config_poisoned ||
	    ctx->domain_registered || !ctx->config_entry_count) {
		ret = -EINVAL;
		goto out;
	}
	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		if (entry->cfg.entry_id) {
			has_registered_entry = true;
			break;
		}
	}
	if (!has_registered_entry) {
		ret = -EINVAL;
		goto out;
	}

	ctx->config_poisoned = true;
	if (!ctx->config_domain_count) {
		domain = elc_kzalloc(sizeof(*domain));
		if (!domain) {
			ret = -ENOMEM;
			goto out;
		}
		domain->cfg.struct_size = sizeof(domain->cfg);
		domain->cfg.api_major = ELC_API_VERSION_MAJOR;
		domain->cfg.config_id = U32_MAX;
		domain->common.config_id = U32_MAX;
		list_add_tail(&domain->common.node, &ctx->config_domains);
		ctx->config_domain_count = 1;
	}
	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		elc_authority_init(&domain->authority);
		domain->ec_domain = ecrt_master_create_domain(ctx->master);
		if (!domain->ec_domain) {
			ret = -ENOMEM;
			result.failed_config_id = domain->cfg.config_id;
			goto out;
		}
		if (!ctx->domain)
			ctx->domain = domain->ec_domain;
	}
	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		if (ctx->config_domain_assignment_count) {
			assignment = elc_find_domain_assignment(
				ctx, slave->cfg.config_id);
			domain = assignment ?
				elc_find_domain(
					ctx,
					assignment->cfg.domain_config_id) : NULL;
		} else {
			domain = list_first_entry(&ctx->config_domains,
						 struct elc_domain_node,
						 common.node);
		}
		if (!domain) {
			ret = -ENOENT;
			result.failed_config_id = slave->cfg.config_id;
			goto out;
		}
		slave->domain = domain;
	}

	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		if (!entry->cfg.entry_id)
			continue;
		pdo = elc_find_pdo(ctx, entry->cfg.pdo_config_id);
		sync = pdo ? elc_find_sync(ctx, pdo->cfg.sync_config_id) : NULL;
		slave = sync ?
			elc_find_slave(ctx, sync->cfg.slave_config_id) : NULL;
		if (!pdo || !sync || !slave) {
			ret = -ENOENT;
			result.failed_config_id = entry->cfg.config_id;
			goto out;
		}

		bit_position = 0;
		offset = ecrt_slave_config_reg_pdo_entry_pos(
			slave->ec_config, sync->cfg.sync_index,
			elc_pdo_position(ctx, pdo),
			elc_entry_position(ctx, entry),
			slave->domain->ec_domain, &bit_position);
		if (offset < 0) {
			ret = offset;
			result.failed_config_id = entry->cfg.config_id;
			goto out;
		}
		entry->domain_offset = offset;
		entry->bit_position = bit_position;
		entry->registered = true;
		result.entry_count++;
	}
	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		size_t size = ecrt_domain_size(domain->ec_domain);

		if (!size || size > U32_MAX || total_size + size > U32_MAX) {
			ret = !size ? -EINVAL : -EOVERFLOW;
			result.failed_config_id = domain->cfg.config_id;
			goto out;
		}
		domain->base_offset = total_size;
		domain->size = size;
		total_size += size;
	}
	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		if (!entry->registered)
			continue;
		pdo = elc_find_pdo(ctx, entry->cfg.pdo_config_id);
		sync = pdo ? elc_find_sync(ctx,
					     pdo->cfg.sync_config_id) : NULL;
		slave = sync ?
			elc_find_slave(ctx, sync->cfg.slave_config_id) : NULL;
		if (!slave || !slave->domain ||
		    entry->domain_offset >
			    U32_MAX - slave->domain->base_offset) {
			ret = -EOVERFLOW;
			result.failed_config_id = entry->cfg.config_id;
			goto out;
		}
		entry->domain_offset += slave->domain->base_offset;
	}
	ctx->domain_size = total_size;

	ctx->domain_registered = true;
	ctx->config_poisoned = false;
	ret = 0;
out:
	result.result = ret;
	if (copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_get_entry_offset(struct elc_file *ctx, void __user *argp)
{
	struct elc_entry_offset result;
	struct elc_entry_node *entry;
	u32 entry_id;
	int ret = -ENOENT;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (memchr_inv(result.reserved, 0, sizeof(result.reserved)))
		return -EINVAL;
	if (!result.entry_id)
		return -EINVAL;
	entry_id = result.entry_id;

	mutex_lock(&ctx->lock);
	ret = -ENOENT;
	if (!ctx->domain_registered) {
		ret = -EINVAL;
		goto out;
	}
	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		if (entry->cfg.entry_id == entry_id && entry->registered) {
			memset(&result, 0, sizeof(result));
			result.struct_size = sizeof(result);
			result.api_major = ELC_API_VERSION_MAJOR;
			result.entry_id = entry_id;
			result.global_offset = entry->domain_offset;
			result.bit_position = entry->bit_position;
			result.bit_length = entry->cfg.bit_length;
			ret = 0;
			break;
		}
	}
out:
	if (!ret && copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_cycle_activate(struct elc_file *ctx, void __user *argp)
{
	struct elc_dc_node *dc;
	struct elc_domain_node *domain;
	struct elc_entry_node *entry;
	struct elc_pdo_node *pdo;
	struct elc_slave_node *slave;
	struct elc_sync_node *sync;
	struct elc_cycle_activate result;
	u64 first_bit;
	u64 end_bit;
	u64 bit;
	size_t domain_size;
	u32 period_ns;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.flags ||
	    result.cycle_period_ns < ELC_CYCLE_PERIOD_MIN_NS ||
	    result.cycle_period_ns > ELC_CYCLE_PERIOD_MAX_NS)
		return -EINVAL;
	if (elc_cycle_cpu < -1 ||
	    (elc_cycle_cpu >= 0 &&
	     (elc_cycle_cpu >= nr_cpu_ids ||
	      !cpu_online(elc_cycle_cpu))) ||
	    elc_cycle_fifo_priority >= MAX_RT_PRIO)
		return -EINVAL;
	period_ns = result.cycle_period_ns;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
	result.cycle_period_ns = period_ns;

	mutex_lock(&ctx->lock);
	if (!ctx->domain_registered || ctx->config_poisoned || ctx->active) {
		ret = -EINVAL;
		goto out;
	}
	list_for_each_entry(dc, &ctx->config_dcs, common.node) {
		if (dc->cfg.sync0_cycle_ns != period_ns) {
			ret = -EINVAL;
			goto out;
		}
	}
	domain_size = ctx->domain_size;
	if (domain_size > U32_MAX) {
		ret = -EOVERFLOW;
		goto out;
	}
	if (domain_size > ELC_PROCESS_IMAGE_MAX) {
		ret = -E2BIG;
		goto out;
	}
	ctx->input_buffers[0] = elc_kvzalloc(domain_size);
	if (!ctx->input_buffers[0]) {
		ret = -ENOMEM;
		goto out;
	}
	ctx->input_buffers[1] = elc_kvzalloc(domain_size);
	if (!ctx->input_buffers[1]) {
		ret = -ENOMEM;
		elc_free_input_buffers(ctx);
		goto out;
	}
	ret = elc_allocate_input_history(ctx, domain_size);
	if (ret) {
		elc_free_input_buffers(ctx);
		goto out;
	}
	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		struct elc_output_authority *authority = &domain->authority;

		if (!domain->authority.initialised)
			elc_authority_init(authority);
		ret = elc_authority_alloc_buffers(authority, domain->size);
		if (ret) {
			elc_free_input_buffers(ctx);
			elc_free_output_buffers(ctx);
			goto out;
		}
		authority->last_sequence_consumed = 0;
		authority->stale_cycles = 0;
		authority->ever_healthy = false;
		atomic_set(&authority->armed, 0);
		atomic_set(&authority->rearm_required, 0);
		atomic_set(&authority->healthy, 0);
		atomic_set(&authority->current_faults, 0);
		atomic_set(&authority->last_latched_faults, 0);
		atomic64_set(&authority->fault_output_sequence, 0);
		atomic64_set(&authority->gate_request, 0);
		atomic64_set(&authority->gate_applied, 0);
		atomic64_set(&authority->sequence, 0);
		atomic_set(&authority->lease_remaining_cycles, 0);
		/* lease_configured_cycles preserved if set pre-activation */
	}
	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		struct elc_output_authority *authority;
		u32 local_offset;

		if (!entry->registered)
			continue;
		pdo = elc_find_pdo(ctx, entry->cfg.pdo_config_id);
		sync = pdo ? elc_find_sync(ctx,
					     pdo->cfg.sync_config_id) : NULL;
		slave = sync ?
			elc_find_slave(ctx, sync->cfg.slave_config_id) : NULL;
		if (!sync || !slave || !slave->domain ||
		    sync->cfg.direction != ELC_DIR_OUTPUT)
			continue;
		if (entry->domain_offset < slave->domain->base_offset)
			continue;
		local_offset = entry->domain_offset - slave->domain->base_offset;
		authority = &slave->domain->authority;
		first_bit = (u64)local_offset * 8U + entry->bit_position;
		end_bit = first_bit + entry->cfg.bit_length;
		if (end_bit > (u64)slave->domain->size * 8U) {
			ret = -EOVERFLOW;
			elc_free_input_buffers(ctx);
			elc_free_output_buffers(ctx);
			goto out;
		}
		for (bit = first_bit; bit < end_bit; bit++)
			authority->mask[bit / 8U] |= BIT(bit % 8U);
	}

	ctx->cycle_period_ns = period_ns;
	ctx->application_time_ns =
		div64_u64(ktime_get_ns(), period_ns) * period_ns;
	ret = ecrt_master_application_time(ctx->master,
					  ctx->application_time_ns);
	if (ret) {
		elc_free_input_buffers(ctx);
		elc_free_output_buffers(ctx);
		goto out;
	}
	ret = ecrt_master_activate(ctx->master);
	if (ret) {
		elc_free_input_buffers(ctx);
		elc_free_output_buffers(ctx);
		ctx->config_poisoned = true;
		goto out;
	}

	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		domain->data = ecrt_domain_data(domain->ec_domain);
		if (domain->size && !domain->data) {
			ret = -EFAULT;
			ecrt_master_deactivate(ctx->master);
			elc_free_input_buffers(ctx);
			elc_free_output_buffers(ctx);
			elc_invalidate_applied_config(ctx);
			goto out;
		}
	}

	/*
	 * No user-space process-image writer exists in API 0.4. Start every
	 * mapped output at zero before the first application datagram is sent.
	 */
	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		if (domain->size)
			memset(domain->data, 0, domain->size);
		atomic_set(&domain->working_counter, 0);
		atomic_set(&domain->working_counter_state, EC_WC_ZERO);
	}
	atomic64_set(&ctx->cycle_count, 0);
	atomic64_set(&ctx->cycle_error_count, 0);
	atomic64_set(&ctx->cycle_overrun_count, 0);
	atomic64_set(&ctx->maximum_lateness_ns, 0);
	atomic64_set(&ctx->cycle_info_sequence, 0);
	atomic_set(&ctx->pending_cycle_period_ns, 0);
	atomic64_set(&ctx->period_request_sequence, 0);
	atomic64_set(&ctx->period_applied_sequence, 0);
	atomic64_set(&ctx->period_applied_cycle, 0);
	memset(&ctx->cycle_info, 0, sizeof(ctx->cycle_info));
	ctx->cycle_info.struct_size = sizeof(ctx->cycle_info);
	ctx->cycle_info.api_major = ELC_API_VERSION_MAJOR;
	ctx->cycle_info.config_generation = ctx->config_generation;
	ctx->cycle_info.cycle_period_ns = ctx->cycle_period_ns;
	atomic_set(&ctx->working_counter, 0);
	atomic_set(&ctx->working_counter_state, EC_WC_ZERO);
	atomic_set(&ctx->last_cycle_result, 0);
	ctx->dc_cycle_adjustment_ns = 0;
	ctx->dc_difference_total_ns = 0;
	ctx->dc_delta_total_ns = 0;
	ctx->dc_last_difference_ns = 0;
	ctx->dc_filter_count = 0;
	ctx->dc_monitor_countdown = 0;
	ctx->dc_monitor_wait_cycles = 0;
	ctx->dc_reference_valid = false;
	ctx->dc_monitor_pending = false;
	ctx->dc_last_reference_result = 0;
	atomic_set(&ctx->dc_status_reference_valid, 0);
	atomic_set(&ctx->dc_status_monitor_pending, 0);
	atomic_set(&ctx->dc_status_last_reference_result, 0);
	atomic_set(&ctx->dc_status_last_difference_ns, 0);
	atomic_set(&ctx->dc_status_cycle_adjustment_ns, 0);
	atomic_set(&ctx->dc_status_last_maximum_deviation_ns, 0);
	atomic64_set(&ctx->dc_status_maximum_deviation_ns, 0);
	atomic64_set(&ctx->dc_reference_read_error_count, 0);
	atomic64_set(&ctx->dc_reference_resume_count, 0);
	atomic64_set(&ctx->dc_monitor_success_count, 0);
	atomic64_set(&ctx->dc_monitor_timeout_count, 0);
	ctx->dc_last_reference_sample = 0;
	ctx->dc_last_applied_adjustment_ns = 0;
	ctx->dc_published_app_time_ns = 0;
	ctx->dc_published_reference_valid = 0;
	ctx->dc_published_reference_sample = 0;
	ctx->dc_published_phase_difference_ns = 0;
	ctx->dc_published_applied_adjustment_ns = 0;
	atomic_set(&ctx->io_bus_healthy, 0);
	atomic_set(&ctx->io_link_up, 0);
	atomic_set(&ctx->io_current_faults, 0);
	atomic_set(&ctx->io_slaves_responding, 0);
	atomic_set(&ctx->io_configured_slaves_online, 0);
	atomic_set(&ctx->io_configured_slaves_operational, 0);
	atomic64_set(&ctx->io_fault_count, 0);

	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		atomic_set(&slave->state_result, -ENODATA);
		atomic_set(&slave->state_online, 0);
		atomic_set(&slave->state_operational, 0);
		atomic_set(&slave->state_al_state, 0);
	}
	ctx->input_active = 0;
	ctx->input_reader = -1;
	atomic64_set(&ctx->input_sequence, 0);
	if (elc_test_fail_cycle_thread)
		ctx->cycle_thread = ERR_PTR(-ENOMEM);
	else
		ctx->cycle_thread = kthread_create(elc_cycle_thread, ctx,
						   "elc_cycle");
	if (IS_ERR(ctx->cycle_thread)) {
		ret = PTR_ERR(ctx->cycle_thread);
		ctx->cycle_thread = NULL;
		ecrt_master_deactivate(ctx->master);
		elc_free_input_buffers(ctx);
		elc_free_output_buffers(ctx);
		elc_invalidate_applied_config(ctx);
		goto out;
	}
	if (elc_cycle_cpu >= 0) {
		ret = set_cpus_allowed_ptr(ctx->cycle_thread,
					   cpumask_of(elc_cycle_cpu));
		if (ret)
			goto thread_config_failed;
	}
	if (elc_cycle_fifo_priority) {
		ret = elc_set_fifo_priority(ctx->cycle_thread,
					    elc_cycle_fifo_priority);
		if (ret)
			goto thread_config_failed;
	}
	WRITE_ONCE(ctx->active, true);
	wake_up_process(ctx->cycle_thread);
	result.domain_size = ctx->domain_size;
	ret = 0;
	goto out;

thread_config_failed:
	kthread_stop(ctx->cycle_thread);
	ctx->cycle_thread = NULL;
	ecrt_master_deactivate(ctx->master);
	elc_free_input_buffers(ctx);
	elc_free_output_buffers(ctx);
	elc_invalidate_applied_config(ctx);
out:
	result.result = ret;
	if (copy_to_user(argp, &result, sizeof(result))) {
		if (ctx->active)
			elc_deactivate_locked(ctx);
		ret = -EFAULT;
	}
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_cycle_set_period(struct elc_file *ctx, void __user *argp)
{
	struct elc_cycle_period_update result;
	unsigned long timeout;
	u64 request_sequence;
	u32 requested_period;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.flags || result.applied_period_ns ||
	    result.effective_after_cycle ||
	    result.cycle_period_ns < ELC_CYCLE_PERIOD_MIN_NS ||
	    result.cycle_period_ns > ELC_CYCLE_PERIOD_MAX_NS)
		return -EINVAL;
	requested_period = result.cycle_period_ns;

	mutex_lock(&ctx->lock);
	if (!ctx->active) {
		ret = -EINVAL;
		goto out;
	}
	if (result.config_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}
	/*
	 * DC sessions are allowed when disarmed. At the completed-cycle
	 * boundary the cyclic task updates the host period, rewrites each
	 * configured SYNC0 to match, and resets the DC filter so phase
	 * control re-locks to the new period. Outputs must stay disarmed for
	 * the whole transition.
	 */
	{
		bool any_armed = false;
		struct elc_domain_node *d;

		list_for_each_entry(d, &ctx->config_domains, common.node) {
			if (atomic_read(&d->authority.armed)) {
				any_armed = true;
				break;
			}
		}
		if (any_armed) {
			ret = -EBUSY;
			goto out;
		}
	}
	if (requested_period == READ_ONCE(ctx->cycle_period_ns)) {
		result.applied_period_ns = requested_period;
		result.effective_after_cycle =
			atomic64_read(&ctx->cycle_count);
		ret = 0;
		goto copy_result;
	}

	request_sequence =
		atomic64_inc_return(&ctx->period_request_sequence);
	atomic_set(&ctx->pending_cycle_period_ns, requested_period);
	timeout = msecs_to_jiffies(
		2U * DIV_ROUND_UP(
			max(requested_period,
			    READ_ONCE(ctx->cycle_period_ns)),
			1000000U) +
		100U);
	if (!wait_event_timeout(
		    ctx->period_wait,
		    atomic64_read(&ctx->period_applied_sequence) >=
			    request_sequence,
		    timeout)) {
		int pending;

		/*
		 * Remove an unconsumed request. If the task won that race, give
		 * its boundary acknowledgement one more bounded wait so user
		 * space never sees a timeout for a period that was applied.
		 */
		pending = atomic_cmpxchg(&ctx->pending_cycle_period_ns,
					 requested_period, 0);
		if (pending == requested_period) {
			ret = -ETIMEDOUT;
			goto out;
		}
		if (!wait_event_timeout(
			    ctx->period_wait,
			    atomic64_read(&ctx->period_applied_sequence) >=
				    request_sequence,
			    timeout)) {
			ret = -EIO;
			goto out;
		}
	}
	result.applied_period_ns = READ_ONCE(ctx->cycle_period_ns);
	result.effective_after_cycle =
		atomic64_read(&ctx->period_applied_cycle);
	ret = result.applied_period_ns == requested_period ? 0 : -EIO;

copy_result:
	if (!ret && copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_cycle_get_status(struct elc_file *ctx, void __user *argp)
{
	struct elc_cycle_status result;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (memchr_inv(result.reserved0, 0, sizeof(result.reserved0)) ||
	    memchr_inv(result.reserved1, 0, sizeof(result.reserved1)))
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
	mutex_lock(&ctx->lock);
	result.active = ctx->active;
	result.cycle_period_ns = READ_ONCE(ctx->cycle_period_ns);
	result.domain_size = ctx->domain_size;
	result.working_counter = atomic_read(&ctx->working_counter);
	result.working_counter_state =
		atomic_read(&ctx->working_counter_state);
	result.last_cycle_result = atomic_read(&ctx->last_cycle_result);
	result.cycle_count = atomic64_read(&ctx->cycle_count);
	result.cycle_error_count = atomic64_read(&ctx->cycle_error_count);
	result.cycle_overrun_count =
		atomic64_read(&ctx->cycle_overrun_count);
	result.maximum_lateness_ns =
		atomic64_read(&ctx->maximum_lateness_ns);
	mutex_unlock(&ctx->lock);

	if (copy_to_user(argp, &result, sizeof(result)))
		return -EFAULT;
	return 0;
}

static void elc_copy_cycle_info(struct elc_file *ctx,
				  struct elc_cycle_info *result)
{
	unsigned long irq_flags;

	spin_lock_irqsave(&ctx->cycle_info_lock, irq_flags);
	*result = ctx->cycle_info;
	spin_unlock_irqrestore(&ctx->cycle_info_lock, irq_flags);
}

static long elc_cycle_get_info(struct elc_file *ctx, void __user *argp)
{
	struct elc_cycle_info request;
	struct elc_cycle_info result;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = elc_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (request.flags || request.reserved0 || request.reserved1)
		return -EINVAL;

	elc_copy_cycle_info(ctx, &result);
	if (copy_to_user(argp, &result, sizeof(result)))
		return -EFAULT;
	return 0;
}

static long elc_cycle_get_dc_info(struct elc_file *ctx, void __user *argp)
{
	struct elc_cycle_dc_info request;
	struct elc_cycle_dc_info result;
	struct elc_cycle_info cycle;
	unsigned long irq_flags;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = elc_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (request.flags || request.reserved1 ||
	    memchr_inv(request.reserved2, 0, sizeof(request.reserved2)))
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;

	spin_lock_irqsave(&ctx->cycle_info_lock, irq_flags);
	cycle = ctx->cycle_info;
	result.application_time_ns = ctx->dc_published_app_time_ns;
	result.dc_reference_valid = ctx->dc_published_reference_valid;
	result.dc_reference_sample = ctx->dc_published_reference_sample;
	result.dc_phase_difference_ns = ctx->dc_published_phase_difference_ns;
	result.dc_applied_adjustment_ns = ctx->dc_published_applied_adjustment_ns;
	spin_unlock_irqrestore(&ctx->cycle_info_lock, irq_flags);

	result.config_generation = cycle.config_generation;
	result.cycle_index = cycle.cycle_index;
	result.cycle_period_ns = cycle.cycle_period_ns;
	result.scheduled_time_ns = cycle.scheduled_time_ns;
	result.actual_wake_time_ns = cycle.actual_wake_time_ns;
	result.wake_lateness_ns = cycle.wake_lateness_ns;
	result.input_sequence = cycle.input_sequence;
	result.output_sequence_consumed = cycle.output_sequence_consumed;
	result.missed_deadlines = cycle.missed_deadlines;
	result.stale_output_cycles = cycle.stale_output_cycles;
	result.working_counter = cycle.working_counter;
	result.working_counter_state = cycle.working_counter_state;
	result.outputs_armed = cycle.outputs_armed;
	result.bus_healthy = cycle.bus_healthy;
	result.dc_enabled = !!ctx->config_dc_count;
	result.cycle_result = cycle.cycle_result;

	if (copy_to_user(argp, &result, sizeof(result)))
		return -EFAULT;
	return 0;
}

static long elc_cycle_wait(struct elc_file *ctx, void __user *argp)
{
	struct elc_cycle_wait request;
	struct elc_cycle_wait result;
	unsigned long timeout;
	long wait_result;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = elc_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (request.flags || request.reserved0 || !request.timeout_ms ||
	    request.timeout_ms > ELC_CYCLE_WAIT_TIMEOUT_MAX_MS)
		return -EINVAL;

	mutex_lock(&ctx->lock);
	if (!ctx->active) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (request.config_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out_unlock;
	}
	if (request.after_cycle_index >
	    atomic64_read(&ctx->cycle_info_sequence)) {
		ret = -EINVAL;
		goto out_unlock;
	}
	mutex_unlock(&ctx->lock);

	timeout = msecs_to_jiffies(request.timeout_ms);
	/* User tasks sleep here; the cyclic thread only performs a wake-up after
	 * publishing its record and never waits for a consumer.
	 */
	wait_result = wait_event_interruptible_timeout(
		ctx->cycle_wait,
		atomic64_read(&ctx->cycle_info_sequence) !=
				request.after_cycle_index ||
			!READ_ONCE(ctx->active),
		timeout);
	if (wait_result < 0)
		return wait_result;
	if (!wait_result)
		return -ETIMEDOUT;
	if (!READ_ONCE(ctx->active))
		return -ESHUTDOWN;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
	result.config_generation = request.config_generation;
	result.after_cycle_index = request.after_cycle_index;
	result.timeout_ms = request.timeout_ms;
	elc_copy_cycle_info(ctx, &result.cycle);
	if (result.cycle.config_generation != request.config_generation)
		return -ESTALE;
	if (copy_to_user(argp, &result, sizeof(result)))
		return -EFAULT;
	return 0;

out_unlock:
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_cycle_deactivate(struct elc_file *ctx, void __user *argp)
{
	struct elc_cycle_deactivate result;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.reserved || result.reserved1)
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
	mutex_lock(&ctx->lock);
	ret = elc_deactivate_locked(ctx);
	result.result = ret;
	if (copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_cycle_get_dc_status(struct elc_file *ctx,
				      void __user *argp)
{
	struct elc_dc_status result;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.reserved0)
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
	mutex_lock(&ctx->lock);
	result.enabled = !!ctx->config_dc_count;
	result.reference_valid =
		atomic_read(&ctx->dc_status_reference_valid);
	result.monitor_pending =
		atomic_read(&ctx->dc_status_monitor_pending);
	result.last_reference_result =
		atomic_read(&ctx->dc_status_last_reference_result);
	result.last_difference_ns =
		atomic_read(&ctx->dc_status_last_difference_ns);
	result.cycle_adjustment_ns =
		atomic_read(&ctx->dc_status_cycle_adjustment_ns);
	result.last_maximum_deviation_ns =
		atomic_read(&ctx->dc_status_last_maximum_deviation_ns);
	result.maximum_deviation_ns =
		atomic64_read(&ctx->dc_status_maximum_deviation_ns);
	result.reference_read_error_count =
		atomic64_read(&ctx->dc_reference_read_error_count);
	result.reference_resume_count =
		atomic64_read(&ctx->dc_reference_resume_count);
	result.monitor_success_count =
		atomic64_read(&ctx->dc_monitor_success_count);
	result.monitor_timeout_count =
		atomic64_read(&ctx->dc_monitor_timeout_count);
	mutex_unlock(&ctx->lock);

	if (copy_to_user(argp, &result, sizeof(result)))
		return -EFAULT;
	return 0;
}

static long elc_get_io_status(struct elc_file *ctx, void __user *argp)
{
	struct elc_domain_node *domain;
	struct elc_io_status result;
	u8 any_armed = 0;
	u8 any_rearm = 0;
	u32 latched = 0;
	u32 auth_faults = 0;
	u64 max_seq = 0;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.reserved0)
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
	mutex_lock(&ctx->lock);
	result.bus_healthy = atomic_read(&ctx->io_bus_healthy);
	result.link_up = atomic_read(&ctx->io_link_up);
	result.current_faults = atomic_read(&ctx->io_current_faults);
	result.slaves_responding = atomic_read(&ctx->io_slaves_responding);
	result.configured_slave_count = ctx->config_slave_count;
	result.configured_slaves_online =
		atomic_read(&ctx->io_configured_slaves_online);
	result.configured_slaves_operational =
		atomic_read(&ctx->io_configured_slaves_operational);
	result.domain_size = ctx->domain_size;
	result.config_generation = ctx->config_generation;
	result.fault_count = atomic64_read(&ctx->io_fault_count);
	result.input_sequence = atomic64_read(&ctx->input_sequence);
	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		struct elc_output_authority *a = &domain->authority;

		if (atomic_read(&a->armed))
			any_armed = 1;
		if (atomic_read(&a->rearm_required))
			any_rearm = 1;
		latched |= atomic_read(&a->last_latched_faults);
		auth_faults |= atomic_read(&a->current_faults);
		if (atomic64_read(&a->sequence) > max_seq)
			max_seq = atomic64_read(&a->sequence);
	}
	result.outputs_armed = any_armed;
	result.rearm_required = any_rearm;
	result.last_latched_faults = latched;
	result.current_faults |= auth_faults;
	result.output_sequence = max_seq;
	mutex_unlock(&ctx->lock);

	if (copy_to_user(argp, &result, sizeof(result)))
		return -EFAULT;
	return 0;
}

static long elc_get_input_snapshot(struct elc_file *ctx,
				     void __user *argp)
{
	struct elc_input_snapshot result;
	unsigned long irq_flags;
	void __user *data_ptr;
	u64 requested_ptr;
	u32 capacity;
	u8 reader;
	int ret = 0;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.flags || !result.data_ptr)
		return -EINVAL;
	requested_ptr = result.data_ptr;
	capacity = result.data_capacity;
	data_ptr = u64_to_user_ptr(requested_ptr);

	mutex_lock(&ctx->lock);
	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
	result.data_ptr = requested_ptr;
	result.data_capacity = capacity;
	result.data_size = ctx->domain_size;
	result.config_generation = ctx->config_generation;
	if (!ctx->active || !ctx->input_buffers[0] ||
	    !ctx->input_buffers[1]) {
		ret = -EINVAL;
		goto out_copy_result;
	}
	if (capacity < ctx->domain_size) {
		ret = -ENOSPC;
		goto out_copy_result;
	}

	spin_lock_irqsave(&ctx->input_lock, irq_flags);
	if (ctx->input_reader >= 0) {
		spin_unlock_irqrestore(&ctx->input_lock, irq_flags);
		ret = -EBUSY;
		goto out_copy_result;
	}
	reader = ctx->input_active;
	ctx->input_reader = reader;
	result.input_sequence = atomic64_read(&ctx->input_sequence);
	result.cycle_count = ctx->input_cycle_index[reader];
	spin_unlock_irqrestore(&ctx->input_lock, irq_flags);

	if (copy_to_user(data_ptr, ctx->input_buffers[reader],
			 ctx->domain_size))
		ret = -EFAULT;

	spin_lock_irqsave(&ctx->input_lock, irq_flags);
	ctx->input_reader = -1;
	spin_unlock_irqrestore(&ctx->input_lock, irq_flags);

out_copy_result:
	if (copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_configure_input_history(struct elc_file *ctx,
					  void __user *argp)
{
	struct elc_input_history_config result;
	size_t bytes;
	u32 depth;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.flags || result.configured_depth ||
	    result.depth > ELC_INPUT_HISTORY_DEPTH_MAX)
		return -EINVAL;
	depth = result.depth;

	mutex_lock(&ctx->lock);
	if (!ctx->domain_registered || ctx->active) {
		ret = -EINVAL;
		goto out;
	}
	if (result.config_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}
	if (depth &&
	    (check_mul_overflow((size_t)depth,
				(size_t)ctx->domain_size, &bytes) ||
	     bytes > ELC_INPUT_HISTORY_BYTES_MAX)) {
		ret = -E2BIG;
		goto out;
	}
	ctx->input_history.configured_depth = depth;
	result.configured_depth = depth;
	ret = 0;
out:
	if (!ret && copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_get_input_history_batch(struct elc_file *ctx,
					   void __user *argp)
{
	struct elc_input_history *history = &ctx->input_history;
	struct elc_input_history_batch result;
	struct elc_input_history_record *records = NULL;
	unsigned long irq_flags;
	void __user *records_ptr;
	void __user *data_ptr;
	size_t required_data;
	u64 cycle;
	u64 latest;
	u32 *slots = NULL;
	u32 count = 0;
	u32 i;
	u8 *image_data = NULL;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.flags || !result.records_ptr || !result.data_ptr ||
	    !result.max_records ||
	    result.max_records > ELC_INPUT_HISTORY_BATCH_MAX ||
	    result.record_count || result.image_size ||
	    result.first_cycle_index || result.last_cycle_index ||
	    result.latest_cycle_index || result.dropped_records ||
	    result.capture_drop_count ||
	    result.after_cycle_index == U64_MAX)
		return -EINVAL;
	records_ptr = u64_to_user_ptr(result.records_ptr);
	data_ptr = u64_to_user_ptr(result.data_ptr);
	slots = kcalloc(result.max_records, sizeof(*slots), GFP_KERNEL);
	records = kcalloc(result.max_records, sizeof(*records), GFP_KERNEL);
	if (!slots || !records) {
		ret = -ENOMEM;
		goto out_free;
	}

	mutex_lock(&ctx->lock);
	if (!ctx->active || !history->depth) {
		ret = -EINVAL;
		goto out_unlock;
	}
	if (result.config_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out_unlock;
	}
	if (check_mul_overflow((size_t)result.max_records,
			       (size_t)ctx->domain_size, &required_data)) {
		ret = -EOVERFLOW;
		goto out_unlock;
	}
	result.image_size = ctx->domain_size;
	if (result.data_capacity < required_data) {
		ret = -ENOSPC;
		goto out_copy_result;
	}
	image_data = kvmalloc(required_data, GFP_KERNEL);
	if (!image_data) {
		ret = -ENOMEM;
		goto out_copy_result;
	}

	spin_lock_irqsave(&history->lock, irq_flags);
	latest = history->latest_cycle_index;
	result.latest_cycle_index = latest;
	result.capture_drop_count =
		atomic64_read(&history->capture_drop_count);
	if (result.after_cycle_index > latest) {
		spin_unlock_irqrestore(&history->lock, irq_flags);
		ret = -EINVAL;
		goto out_copy_result;
	}
	cycle = result.after_cycle_index + 1U;
	if (latest >= history->depth &&
	    cycle <= latest - history->depth)
		cycle = latest - history->depth + 1U;
	for (; cycle <= latest && count < result.max_records; cycle++) {
		u32 slot = cycle % history->depth;

		if (history->slot_state[slot] !=
			    ELC_HISTORY_SLOT_FREE ||
		    history->records[slot].cycle_index != cycle)
			continue;
		history->slot_state[slot] = ELC_HISTORY_SLOT_READER;
		slots[count] = slot;
		records[count] = history->records[slot];
		count++;
		if (cycle == U64_MAX)
			break;
	}
	spin_unlock_irqrestore(&history->lock, irq_flags);

	result.record_count = count;
	if (count) {
		result.first_cycle_index = records[0].cycle_index;
		result.last_cycle_index = records[count - 1U].cycle_index;
		result.dropped_records =
			result.first_cycle_index -
			result.after_cycle_index - 1U;
		for (i = 1; i < count; i++)
			result.dropped_records +=
				records[i].cycle_index -
				records[i - 1U].cycle_index - 1U;
	} else if (latest > result.after_cycle_index) {
		result.dropped_records =
			latest - result.after_cycle_index;
	}
	if (copy_to_user(records_ptr, records,
			 count * sizeof(*records))) {
		ret = -EFAULT;
		goto out_release;
	}
	for (i = 0; i < count; i++) {
		memcpy(image_data + (size_t)i * ctx->domain_size,
		       history->data +
			       (size_t)slots[i] * ctx->domain_size,
		       ctx->domain_size);
	}
	if (count && copy_to_user(data_ptr, image_data,
				  (size_t)count * ctx->domain_size)) {
		ret = -EFAULT;
		goto out_release;
	}
	ret = 0;

out_release:
	spin_lock_irqsave(&history->lock, irq_flags);
	for (i = 0; i < count; i++)
		if (history->slot_state[slots[i]] ==
		    ELC_HISTORY_SLOT_READER)
			history->slot_state[slots[i]] =
				ELC_HISTORY_SLOT_FREE;
	spin_unlock_irqrestore(&history->lock, irq_flags);
out_copy_result:
	if (copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
out_unlock:
	mutex_unlock(&ctx->lock);
out_free:
	kvfree(image_data);
	kfree(records);
	kfree(slots);
	return ret;
}

static long elc_publish_output(struct elc_file *ctx, void __user *argp)
{
	struct elc_output_publish result;
	struct elc_domain_node *domain;
	unsigned long irq_flags;
	void __user *data_ptr;
	void __user *mask_ptr;
	u64 requested_generation;
	u64 max_sequence = 0;
	u32 data_size;
	u32 domain_id;
	u32 i;
	int ret;
	u8 *tmp_data = NULL;
	u8 *tmp_mask = NULL;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.flags || !result.data_ptr || !result.mask_ptr)
		return -EINVAL;
	domain_id = result.domain_config_id;
	requested_generation = result.config_generation;
	data_size = result.data_size;
	data_ptr = u64_to_user_ptr(result.data_ptr);
	mask_ptr = u64_to_user_ptr(result.mask_ptr);

	mutex_lock(&ctx->lock);
	if (!ctx->active) {
		ret = -EINVAL;
		goto out;
	}
	if (requested_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}

	if (!domain_id) {
		if (data_size != ctx->domain_size) {
			ret = -EMSGSIZE;
			goto out;
		}
		tmp_data = kvzalloc(data_size, GFP_KERNEL);
		tmp_mask = kvzalloc(data_size, GFP_KERNEL);
		if (!tmp_data || !tmp_mask) {
			ret = -ENOMEM;
			goto out;
		}
		if (copy_from_user(tmp_data, data_ptr, data_size) ||
		    copy_from_user(tmp_mask, mask_ptr, data_size)) {
			ret = -EFAULT;
			goto out;
		}
		list_for_each_entry(domain, &ctx->config_domains, common.node) {
			struct elc_output_authority *authority =
				&domain->authority;
			u8 active, target;

			if (!authority->buffers[0] || !domain->size)
				continue;
			spin_lock_irqsave(&authority->lock, irq_flags);
			active = authority->active;
			target = active ^ 1U;
			if (authority->reader == target) {
				spin_unlock_irqrestore(&authority->lock,
						       irq_flags);
				ret = -EBUSY;
				goto out;
			}
			spin_unlock_irqrestore(&authority->lock, irq_flags);
			for (i = 0; i < domain->size; i++) {
				u32 g = domain->base_offset + i;
				u8 mask = tmp_mask[g] & authority->mask[i];
				u8 old = authority->buffers[active][i];
				u8 neu = tmp_data[g];

				authority->buffers[target][i] =
					(neu & mask) | (old & ~mask);
			}
			spin_lock_irqsave(&authority->lock, irq_flags);
			authority->active = target;
			max_sequence =
				atomic64_inc_return(&authority->sequence);
			spin_unlock_irqrestore(&authority->lock, irq_flags);
			elc_authority_refill_lease(authority);
		}
	} else {
		struct elc_output_authority *authority;
		u8 active, target;

		domain = elc_find_domain(ctx, domain_id);
		if (!domain || !domain->authority.buffers[0]) {
			ret = -ENOENT;
			goto out;
		}
		if (data_size != domain->size) {
			ret = -EMSGSIZE;
			goto out;
		}
		authority = &domain->authority;
		spin_lock_irqsave(&authority->lock, irq_flags);
		active = authority->active;
		target = active ^ 1U;
		if (authority->reader == target) {
			spin_unlock_irqrestore(&authority->lock, irq_flags);
			ret = -EBUSY;
			goto out;
		}
		spin_unlock_irqrestore(&authority->lock, irq_flags);
		if (copy_from_user(authority->buffers[target], data_ptr,
				   data_size) ||
		    copy_from_user(authority->update_mask, mask_ptr,
				   data_size)) {
			ret = -EFAULT;
			goto out;
		}
		for (i = 0; i < data_size; i++) {
			u8 mask = authority->update_mask[i] &
				  authority->mask[i];
			u8 old = authority->buffers[active][i];

			authority->buffers[target][i] =
				(authority->buffers[target][i] & mask) |
				(old & ~mask);
		}
		spin_lock_irqsave(&authority->lock, irq_flags);
		authority->active = target;
		max_sequence = atomic64_inc_return(&authority->sequence);
		spin_unlock_irqrestore(&authority->lock, irq_flags);
		elc_authority_refill_lease(authority);
	}
	result.output_sequence = max_sequence;
	result.config_generation = ctx->config_generation;
	ret = 0;
out:
	if (!ret && copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	kvfree(tmp_data);
	kvfree(tmp_mask);
	return ret;
}

static long elc_arm_outputs(struct elc_file *ctx, void __user *argp)
{
	struct elc_output_arm request;
	struct elc_domain_node *domain;
	u32 domain_id;
	int ret;
	int armed_count = 0;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = elc_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	/* flags: 0 = all healthy domains; non-zero = domain_config_id */
	domain_id = request.flags;

	mutex_lock(&ctx->lock);
	if (!ctx->active) {
		ret = -EINVAL;
		goto out;
	}
	if (request.config_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}

	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		struct elc_output_authority *authority = &domain->authority;
		u64 current_sequence;

		if (domain_id && domain->cfg.config_id != domain_id)
			continue;
		current_sequence = atomic64_read(&authority->sequence);
		if (!current_sequence ||
		    request.output_sequence != current_sequence) {
			if (domain_id) {
				ret = -ESTALE;
				goto out;
			}
			continue;
		}
		if (!atomic_read(&authority->healthy)) {
			if (domain_id) {
				ret = -EAGAIN;
				goto out;
			}
			continue;
		}
		if (atomic_read(&authority->rearm_required) &&
		    current_sequence <=
			    atomic64_read(&authority->fault_output_sequence)) {
			if (domain_id) {
				ret = -EAGAIN;
				goto out;
			}
			continue;
		}
		/* Seed/refill lease so arm never needs a prior renew ioctl. */
		elc_authority_refill_lease(authority);
		atomic_set(&authority->rearm_required, 0);
		atomic_set(&authority->armed, 1);
		armed_count++;
	}
	if (!armed_count)
		ret = domain_id ? -ENOENT : -EAGAIN;
	else
		ret = 0;
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_disarm_outputs(struct elc_file *ctx, void __user *argp)
{
	struct elc_output_disarm request;
	struct elc_domain_node *domain;
	u32 domain_id;
	u64 gate_request = 0;
	int ret;
	int count = 0;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = elc_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	domain_id = request.flags;

	mutex_lock(&ctx->lock);
	if (!ctx->active) {
		ret = -EINVAL;
		goto out;
	}
	if (request.config_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}
	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		struct elc_output_authority *authority = &domain->authority;

		if (domain_id && domain->cfg.config_id != domain_id)
			continue;
		count++;
		atomic_set(&authority->armed, 0);
		atomic_set(&authority->rearm_required, 1);
		atomic64_set(&authority->fault_output_sequence,
			     atomic64_read(&authority->sequence));
		gate_request = atomic64_inc_return(&authority->gate_request);
	}
	if (!count) {
		ret = -ENOENT;
		goto out;
	}
	ret = elc_wait_output_gate(ctx, gate_request);
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static long
elc_configure_output_lease(struct elc_file *ctx, void __user *argp)
{
	struct elc_domain_node *domain;
	struct elc_output_lease_config request;
	u32 domain_id;
	u32 budget;
	int matched = 0;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = elc_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	/* flags: 0 = all domains; non-zero = domain_config_id. */
	if (request.reserved1 ||
	    request.cycle_budget > ELC_OUTPUT_LEASE_CYCLES_MAX ||
	    request.timeout_ms > ELC_OUTPUT_LEASE_TIMEOUT_MS_MAX)
		return -EINVAL;
	if (!request.cycle_budget && !request.timeout_ms) {
		/* Disable lease on the selected domain(s). */
		budget = 0;
	} else {
		budget = elc_lease_budget_from_request(ctx, request.cycle_budget,
						       request.timeout_ms);
		if (budget == U32_MAX)
			return -EINVAL;
	}
	domain_id = request.flags;

	mutex_lock(&ctx->lock);
	if (!ctx->domain_registered) {
		ret = -EINVAL;
		goto out;
	}
	if (request.config_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}
	/*
	 * API 0.18: configure is allowed while cycling so hang-failsafe can
	 * be enabled after a clean OP bring-up. Disabling (budget 0) is also
	 * allowed while active.
	 */
	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		struct elc_output_authority *authority = &domain->authority;

		if (domain_id && domain->cfg.config_id != domain_id)
			continue;
		if (!authority->initialised)
			elc_authority_init(authority);
		authority->lease_timeout_ms = request.timeout_ms;
		authority->lease_configured_cycles = budget;
		/* Seed remaining so arm does not require a prior renew. */
		atomic_set(&authority->lease_remaining_cycles, budget);
		atomic64_set(&authority->lease_renewal_count, 0);
		atomic64_set(&authority->lease_expiry_count, 0);
		matched++;
	}
	if (!matched) {
		ret = -ENOENT;
		goto out;
	}
	ret = 0;
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static long
elc_renew_output_lease(struct elc_file *ctx, void __user *argp)
{
	struct elc_domain_node *domain;
	struct elc_output_lease_renew result;
	u64 generation;
	u64 renewals = 0;
	u32 budget = 0;
	u32 domain_id;
	bool any = false;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.reserved0 || result.remaining_cycles ||
	    result.renewal_count)
		return -EINVAL;
	domain_id = result.flags;
	generation = result.config_generation;
	mutex_lock(&ctx->lock);
	if (!ctx->active) {
		ret = -EINVAL;
		goto out;
	}
	if (generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}
	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		struct elc_output_authority *authority = &domain->authority;

		if (domain_id && domain->cfg.config_id != domain_id)
			continue;
		if (!authority->lease_configured_cycles)
			continue;
		any = true;
		elc_authority_refill_lease(authority);
		budget = authority->lease_configured_cycles;
		renewals = atomic64_read(&authority->lease_renewal_count);
	}
	if (!any) {
		ret = domain_id ? -ENOENT : -EINVAL;
		goto out;
	}
	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
	result.config_generation = ctx->config_generation;
	result.remaining_cycles = budget;
	result.renewal_count = renewals;
	ret = 0;
out:
	if (!ret && copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static long
elc_get_output_lease_status(struct elc_file *ctx, void __user *argp)
{
	struct elc_domain_node *domain;
	struct elc_output_lease_status result;
	u64 generation;
	u32 domain_id;
	int remaining = 0;
	u32 configured = 0;
	u32 timeout_ms = 0;
	u64 renewals = 0;
	u64 expiries = 0;
	bool enabled = false;
	int matched = 0;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (memchr_inv(result.reserved0, 0, sizeof(result.reserved0)) ||
	    result.configured_cycles || result.remaining_cycles ||
	    result.enabled || result.valid || result.timeout_ms ||
	    result.renewal_count || result.expiry_count)
		return -EINVAL;
	domain_id = result.flags;
	generation = result.config_generation;
	mutex_lock(&ctx->lock);
	if (!ctx->config_validated || generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}
	list_for_each_entry(domain, &ctx->config_domains, common.node) {
		struct elc_output_authority *authority = &domain->authority;
		int rem;

		if (domain_id && domain->cfg.config_id != domain_id)
			continue;
		matched++;
		if (!authority->lease_configured_cycles)
			continue;
		enabled = true;
		configured = authority->lease_configured_cycles;
		timeout_ms = authority->lease_timeout_ms;
		rem = atomic_read(&authority->lease_remaining_cycles);
		if (domain_id) {
			remaining = rem;
			renewals =
				atomic64_read(&authority->lease_renewal_count);
			expiries =
				atomic64_read(&authority->lease_expiry_count);
		} else {
			if (rem > remaining)
				remaining = rem;
			renewals =
				atomic64_read(&authority->lease_renewal_count);
			expiries =
				atomic64_read(&authority->lease_expiry_count);
		}
	}
	if (domain_id && !matched) {
		ret = -ENOENT;
		goto out;
	}
	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
	result.config_generation = ctx->config_generation;
	result.configured_cycles = configured;
	result.remaining_cycles = max(remaining, 0);
	result.timeout_ms = timeout_ms;
	result.enabled = enabled;
	result.valid = ctx->active && enabled && remaining > 0;
	result.renewal_count = renewals;
	result.expiry_count = expiries;
	ret = 0;
out:
	if (!ret && copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_get_config_slave_status(struct elc_file *ctx,
					   void __user *argp)
{
	struct elc_config_slave_status result;
	struct elc_slave_node *slave;
	u64 requested_generation;
	u32 config_id;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.reserved1 || result.reserved0[0] ||
	    result.reserved0[1] || result.reserved0[2])
		return -EINVAL;
	config_id = result.config_id;
	requested_generation = result.config_generation;

	mutex_lock(&ctx->lock);
	if (!ctx->config_validated ||
	    requested_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}
	slave = elc_find_slave(ctx, config_id);
	if (!slave) {
		ret = -ENOENT;
		goto out;
	}
	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
	result.config_id = config_id;
	result.config_generation = ctx->config_generation;
	result.active = ctx->active;
	if (ctx->active) {
		result.state_result = atomic_read(&slave->state_result);
		result.online = atomic_read(&slave->state_online);
		result.operational =
			atomic_read(&slave->state_operational);
		result.al_state = atomic_read(&slave->state_al_state);
		result.cycle_count = atomic64_read(&ctx->cycle_count);
		result.input_sequence =
			atomic64_read(&ctx->input_sequence);
		/*
		 * Bus data validity follows the assigned domain WC (domain
		 * firewall). online/operational remain separate fields for
		 * application policy; they must not clear validity for a
		 * domain that is still exchanging during topology re-scan.
		 */
		result.data_valid =
			result.input_sequence && slave->domain &&
			atomic_read(&slave->domain->working_counter_state) ==
				EC_WC_COMPLETE;
	} else {
		result.state_result = -ENODATA;
	}
	ret = 0;
out:
	if (!ret && copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_get_domain_status(struct elc_file *ctx, void __user *argp)
{
	struct elc_output_authority *authority;
	struct elc_domain_status result;
	struct elc_domain_node *domain;
	u64 generation;
	u32 domain_id;
	u32 faults = 0;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (memchr_inv(result.reserved0, 0, sizeof(result.reserved0)))
		return -EINVAL;
	domain_id = result.domain_config_id;
	generation = result.config_generation;

	mutex_lock(&ctx->lock);
	if (!ctx->config_validated || generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}
	domain = elc_find_domain(ctx, domain_id);
	if (!domain || !ctx->domain_registered) {
		ret = -ENOENT;
		goto out;
	}
	authority = &domain->authority;
	if (ctx->active) {
		u32 master_faults = 0;

		if (!atomic_read(&ctx->io_link_up))
			master_faults |= ELC_IO_FAULT_LINK_DOWN;
		faults = elc_domain_bus_faults(ctx, domain, master_faults);
	}
	faults |= atomic_read(&authority->current_faults);
	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
	result.domain_config_id = domain_id;
	result.config_generation = ctx->config_generation;
	result.base_offset = domain->base_offset;
	result.domain_size = domain->size;
	result.active = ctx->active;
	result.current_faults = faults;
	result.working_counter =
		atomic_read(&domain->working_counter);
	result.working_counter_state =
		atomic_read(&domain->working_counter_state);
	result.cycle_count = atomic64_read(&ctx->cycle_count);
	result.input_sequence = atomic64_read(&ctx->input_sequence);
	result.data_valid = ctx->active &&
		atomic_read(&authority->healthy) &&
		result.input_sequence;
	result.outputs_armed = atomic_read(&authority->armed);
	result.rearm_required = atomic_read(&authority->rearm_required);
	ret = 0;
out:
	if (!ret && copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static bool elc_entry_config_valid(const struct elc_config_entry *cfg)
{
	/* Padding entry: all zero */
	if (!cfg->entry_id && !cfg->index && !cfg->subindex)
		return true;
	/* Both identity fields set (entry_id + index/subindex) */
	if (cfg->entry_id && cfg->index)
		return true;
	return false;
}

#define ELC_CONFIG_ADD_CHILD(function_name, node_type, cfg_member, list_name, \
			       count_name, max_count, validate_expr) \
static long function_name(struct elc_file *ctx, void __user *argp) \
{ \
	struct node_type *node; \
	int ret; \
	node = elc_kzalloc(sizeof(*node)); \
	if (!node) \
		return -ENOMEM; \
	if (copy_from_user(&node->cfg_member, argp, sizeof(node->cfg_member))) { \
		ret = -EFAULT; \
		goto out; \
	} \
	ret = elc_check_header(node->cfg_member.struct_size, \
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
	else if (elc_config_id_exists(&ctx->list_name, \
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

ELC_CONFIG_ADD_CHILD(elc_config_add_sync, elc_sync_node, cfg,
		       config_syncs, config_sync_count, ELC_CONFIG_SYNC_MAX,
		       !node->cfg.slave_config_id ||
		       node->cfg.sync_index >= EC_MAX_SYNC_MANAGERS ||
		       (node->cfg.direction != ELC_DIR_OUTPUT &&
			node->cfg.direction != ELC_DIR_INPUT) ||
		       node->cfg.watchdog_mode > ELC_WD_DISABLE ||
		       node->cfg.reserved)

ELC_CONFIG_ADD_CHILD(elc_config_add_pdo, elc_pdo_node, cfg,
		       config_pdos, config_pdo_count, ELC_CONFIG_PDO_MAX,
		       !node->cfg.sync_config_id || !node->cfg.pdo_index ||
		       node->cfg.reserved)

ELC_CONFIG_ADD_CHILD(elc_config_add_entry, elc_entry_node, cfg,
		       config_entries, config_entry_count,
		       ELC_CONFIG_ENTRY_MAX,
		       !node->cfg.pdo_config_id || !node->cfg.bit_length ||
		       !elc_entry_config_valid(&node->cfg))

ELC_CONFIG_ADD_CHILD(elc_config_add_dc, elc_dc_node, cfg,
		       config_dcs, config_dc_count, ELC_CONFIG_DC_MAX,
		       !node->cfg.slave_config_id ||
		       !node->cfg.assign_activate ||
		       !node->cfg.sync0_cycle_ns ||
		       node->cfg.reserved0 || node->cfg.flags)

ELC_CONFIG_ADD_CHILD(elc_config_add_domain, elc_domain_node, cfg,
		       config_domains, config_domain_count,
		       ELC_CONFIG_DOMAIN_MAX,
		       node->cfg.flags || node->cfg.reserved)

ELC_CONFIG_ADD_CHILD(elc_config_assign_domain,
		       elc_domain_assignment_node, cfg,
		       config_domain_assignments,
		       config_domain_assignment_count,
		       ELC_CONFIG_SLAVE_MAX,
		       !node->cfg.slave_config_id ||
		       !node->cfg.domain_config_id || node->cfg.flags)

#undef ELC_CONFIG_ADD_CHILD

static long elc_config_set_dc_policy(struct elc_file *ctx,
				       void __user *argp)
{
	struct elc_config_dc_policy policy;
	int ret;

	if (copy_from_user(&policy, argp, sizeof(policy)))
		return -EFAULT;
	ret = elc_check_header(policy.struct_size, policy.api_major,
				 sizeof(policy));
	if (ret)
		return ret;
	if (policy.reference_mode > ELC_DC_REFERENCE_EXPLICIT ||
	    memchr_inv(policy.reserved0, 0, sizeof(policy.reserved0)) ||
	    policy.flags ||
	    (policy.reference_mode == ELC_DC_REFERENCE_EXPLICIT) !=
		    !!policy.reference_slave_config_id)
		return -EINVAL;

	mutex_lock(&ctx->lock);
	if (!ctx->config_started || ctx->config_validated)
		ret = -EINVAL;
	else if (ctx->dc_policy_set)
		ret = -EEXIST;
	else {
		ctx->dc_policy = policy;
		ctx->dc_policy_set = true;
		ret = 0;
	}
	mutex_unlock(&ctx->lock);
	return ret;
}

static long elc_config_validate(struct elc_file *ctx, void __user *argp)
{
	struct elc_domain_assignment_node *assignment;
	struct elc_dc_node *dc;
	struct elc_config_validate result;
	struct elc_entry_node *entry;
	struct elc_sync_node *sync;
	struct elc_pdo_node *pdo;
	struct elc_slave_node *slave;
	int ret = 0;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.reserved)
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;

	mutex_lock(&ctx->lock);
	if (!ctx->config_started || !ctx->config_slave_count) {
		ret = -EINVAL;
		goto out;
	}
	if (!!ctx->config_domain_count !=
	    !!ctx->config_domain_assignment_count) {
		ret = -EINVAL;
		goto out;
	}
	if (ctx->config_domain_count) {
		list_for_each_entry(assignment,
				    &ctx->config_domain_assignments,
				    common.node) {
			struct elc_domain_assignment_node *other;

			if (!elc_config_id_exists(
				    &ctx->config_slaves,
				    assignment->cfg.slave_config_id) ||
			    !elc_config_id_exists(
				    &ctx->config_domains,
				    assignment->cfg.domain_config_id)) {
				ret = -ENOENT;
				goto out;
			}
			list_for_each_entry(
				other, &ctx->config_domain_assignments,
				common.node) {
				if (other != assignment &&
				    other->cfg.slave_config_id ==
					    assignment->cfg.slave_config_id) {
					ret = -EEXIST;
					goto out;
				}
			}
		}
		list_for_each_entry(slave, &ctx->config_slaves, common.node) {
			bool assigned = false;

			list_for_each_entry(
				assignment, &ctx->config_domain_assignments,
				common.node) {
				if (assignment->cfg.slave_config_id ==
				    slave->cfg.config_id) {
					assigned = true;
					break;
				}
			}
			if (!assigned) {
				ret = -ENOENT;
				goto out;
			}
		}
	}
	list_for_each_entry(sync, &ctx->config_syncs, common.node) {
		struct elc_sync_node *other;

		if (!elc_config_id_exists(&ctx->config_slaves,
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
		struct elc_pdo_node *other;

		if (!elc_config_id_exists(&ctx->config_syncs,
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
		struct elc_entry_node *other;

		if (!elc_config_id_exists(&ctx->config_pdos,
					    entry->cfg.pdo_config_id)) {
			ret = -ENOENT;
			goto out;
		}
		list_for_each_entry(other, &ctx->config_entries, common.node) {
			if (entry->cfg.entry_id && other != entry &&
			    other->cfg.entry_id == entry->cfg.entry_id) {
				ret = -EEXIST;
				goto out;
			}
			if (entry->cfg.index && other != entry &&
			    other->cfg.pdo_config_id == entry->cfg.pdo_config_id &&
			    other->cfg.index == entry->cfg.index &&
			    other->cfg.subindex == entry->cfg.subindex) {
				ret = -EEXIST;
				goto out;
			}
		}
	}
	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		struct elc_slave_node *other;

		list_for_each_entry(other, &ctx->config_slaves, common.node) {
			if (other != slave && other->cfg.alias == slave->cfg.alias &&
			    other->cfg.position == slave->cfg.position) {
				ret = -EEXIST;
				goto out;
			}
		}
	}
	list_for_each_entry(dc, &ctx->config_dcs, common.node) {
		struct elc_dc_node *other;

		if (!elc_config_id_exists(&ctx->config_slaves,
					    dc->cfg.slave_config_id)) {
			ret = -ENOENT;
			goto out;
		}
		list_for_each_entry(other, &ctx->config_dcs, common.node) {
			if (other != dc &&
			    other->cfg.slave_config_id ==
				    dc->cfg.slave_config_id) {
				ret = -EEXIST;
				goto out;
			}
		}
	}
	if (ctx->dc_policy.reference_mode == ELC_DC_REFERENCE_EXPLICIT &&
	    (!elc_config_id_exists(
		     &ctx->config_slaves,
		     ctx->dc_policy.reference_slave_config_id) ||
	     !elc_find_dc_for_slave(
		     ctx, ctx->dc_policy.reference_slave_config_id))) {
		ret = -ENOENT;
		goto out;
	}
	if (ctx->dc_policy.reference_mode != ELC_DC_REFERENCE_DISABLED &&
	    !ctx->config_dc_count) {
		ret = -EINVAL;
		goto out;
	}
	if (ctx->config_dc_count &&
	    ctx->dc_policy.reference_mode == ELC_DC_REFERENCE_DISABLED) {
		ret = -EINVAL;
		goto out;
	}
	ctx->config_validated = true;
	ctx->config_generation =
		atomic64_inc_return(&elc_next_config_generation);
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

static int elc_check_header(u16 struct_size, u16 api_major,
			      size_t expected_size)
{
	if (struct_size != expected_size)
		return -EINVAL;
	if (api_major != ELC_API_VERSION_MAJOR)
		return -EPROTONOSUPPORT;
	return 0;
}

static int elc_validate_sdo_type(u8 type, u16 data_len)
{
	switch (type) {
	case ELC_SDO_U8:
	case ELC_SDO_S8:
		return data_len == 1 ? 0 : -EINVAL;
	case ELC_SDO_U16:
	case ELC_SDO_S16:
		return data_len == 2 ? 0 : -EINVAL;
	case ELC_SDO_U32:
	case ELC_SDO_S32:
		return data_len == 4 ? 0 : -EINVAL;
	case ELC_SDO_BYTES:
		return data_len > 0 ? 0 : -EINVAL;
	default:
		return -EINVAL;
	}
}

static long elc_setup_begin(struct elc_file *ctx, void __user *argp)
{
	struct elc_setup_begin request;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = elc_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (request.reserved)
		return -EINVAL;

	mutex_lock(&ctx->lock);
	elc_setup_clear(ctx);
	ctx->setup_started = true;
	mutex_unlock(&ctx->lock);
	return 0;
}

static long elc_setup_reset(struct elc_file *ctx, void __user *argp)
{
	struct elc_setup_begin request;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = elc_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (request.reserved)
		return -EINVAL;

	mutex_lock(&ctx->lock);
	elc_setup_clear(ctx);
	mutex_unlock(&ctx->lock);
	return 0;
}

static long elc_setup_add_sdo(struct elc_file *ctx, void __user *argp)
{
	struct elc_setup_sdo request;
	struct elc_setup_entry *entry;
	size_t allocation_size;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = elc_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (!request.sequence || request.data_len > ELC_SETUP_SDO_DATA_MAX)
		return -EINVAL;
	ret = elc_validate_sdo_type(request.type, request.data_len);
	if (ret)
		return ret;

	allocation_size = struct_size(entry, data, request.data_len);
	entry = elc_kzalloc(allocation_size);
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
	if (ctx->setup_count >= ELC_SETUP_SDO_MAX ||
	    request.data_len > ELC_SETUP_SDO_TOTAL_MAX - ctx->setup_bytes) {
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

static long elc_setup_apply(struct elc_file *ctx, void __user *argp)
{
	struct elc_setup_apply result;
	struct elc_setup_entry *entry;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (memchr_inv(result.reserved0, 0, sizeof(result.reserved0)))
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;

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

static long elc_sdo_upload(struct elc_file *ctx, void __user *argp)
{
	struct elc_sdo_upload result;
	size_t result_size = 0;
	u32 abort_code = 0;
	u16 requested_len;
	u16 position;
	u16 index;
	u8 subindex;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = elc_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.reserved0 || result.reserved1)
		return -EINVAL;
	if (!result.index || !result.requested_len ||
	    result.requested_len > ELC_SETUP_SDO_DATA_MAX)
		return -EINVAL;

	position = result.position;
	index = result.index;
	subindex = result.subindex;
	requested_len = result.requested_len;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = ELC_API_VERSION_MAJOR;
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
	    result_size > ELC_SETUP_SDO_DATA_MAX) {
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

static long elc_get_api_version(void __user *argp)
{
	struct elc_api_version version = {
		.struct_size = sizeof(version),
		.major = ELC_API_VERSION_MAJOR,
		.minor = ELC_API_VERSION_MINOR,
	};

	if (copy_to_user(argp, &version, sizeof(version)))
		return -EFAULT;

	return 0;
}

static long elc_get_capabilities(void __user *argp)
{
	struct elc_capabilities request;
	struct elc_capabilities result = {
		.struct_size = sizeof(result),
		.api_major = ELC_API_VERSION_MAJOR,
		.capabilities =
			ELC_CAP_COHERENT_PROCESS_IMAGE |
			ELC_CAP_CYCLE_TIMING |
			ELC_CAP_CYCLE_WAIT |
			ELC_CAP_DC_DIAGNOSTICS |
			ELC_CAP_OUTPUT_LEASE |
			ELC_CAP_OUTPUT_LEASE_PUBLISH_RENEW |
			ELC_CAP_CYCLE_PERIOD_UPDATE |
			ELC_CAP_INPUT_HISTORY |
			ELC_CAP_CYCLE_DC_INFO |
			ELC_CAP_DOMAIN_OUTPUT_AUTHORITY,
	};
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = elc_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (request.reserved0 ||
	    memchr_inv(request.reserved1, 0, sizeof(request.reserved1)))
		return -EINVAL;
	if (copy_to_user(argp, &result, sizeof(result)))
		return -EFAULT;
	return 0;
}

static long elc_get_master_info(struct elc_file *ctx, void __user *argp)
{
	struct elc_master_info info = {
		.struct_size = sizeof(info),
		.api_major = ELC_API_VERSION_MAJOR,
		.api_minor = ELC_API_VERSION_MINOR,
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

static long elc_get_slave_info(struct elc_file *ctx, void __user *argp)
{
	struct elc_slave_info info;
	ec_slave_info_t ec_info = {};
	int ret;

	if (copy_from_user(&info, argp, sizeof(info)))
		return -EFAULT;

	ret = elc_check_header(info.struct_size, info.api_major,
				 sizeof(info));
	if (ret)
		return ret;

	ret = ecrt_master_get_slave(ctx->master, info.position, &ec_info);
	if (ret)
		return ret;

	memset(&info, 0, sizeof(info));
	info.struct_size = sizeof(info);
	info.api_major = ELC_API_VERSION_MAJOR;
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

static long elc_ioctl(struct file *file, unsigned int cmd,
			unsigned long arg)
{
	struct elc_file *ctx = file->private_data;
	void __user *argp = (void __user *)arg;

	if (!ctx || !ctx->master)
		return -ENODEV;

	if (_IOC_TYPE(cmd) != ELC_IOC_MAGIC)
		return -ENOTTY;

	/*
	 * Allowed while cyclic is active: process-image/status paths, and
	 * blocking mailbox setup/SDO (client-owned commissioning after slave
	 * power return). Setup is NOT RT-safe — callers must run it off the
	 * hard real-time path. Declarative config change remains EBUSY while
	 * active (must deactivate first).
	 */
	switch (cmd) {
	case ELC_IOC_GET_CAPABILITIES:
		return elc_get_capabilities(argp);
	case ELC_IOC_GET_API_VERSION:
		return elc_get_api_version(argp);
	case ELC_IOC_GET_MASTER_INFO:
		return elc_get_master_info(ctx, argp);
	case ELC_IOC_GET_SLAVE_INFO:
		return elc_get_slave_info(ctx, argp);
	case ELC_IOC_SETUP_BEGIN:
		return elc_setup_begin(ctx, argp);
	case ELC_IOC_SETUP_ADD_SDO:
		return elc_setup_add_sdo(ctx, argp);
	case ELC_IOC_SETUP_APPLY:
		return elc_setup_apply(ctx, argp);
	case ELC_IOC_SETUP_RESET:
		return elc_setup_reset(ctx, argp);
	case ELC_IOC_SDO_UPLOAD:
		return elc_sdo_upload(ctx, argp);
	case ELC_IOC_CYCLE_GET_STATUS:
		return elc_cycle_get_status(ctx, argp);
	case ELC_IOC_CYCLE_DEACTIVATE:
		return elc_cycle_deactivate(ctx, argp);
	case ELC_IOC_CYCLE_GET_DC_STATUS:
		return elc_cycle_get_dc_status(ctx, argp);
	case ELC_IOC_CYCLE_GET_INFO:
		return elc_cycle_get_info(ctx, argp);
	case ELC_IOC_CYCLE_GET_DC_INFO:
		return elc_cycle_get_dc_info(ctx, argp);
	case ELC_IOC_CYCLE_WAIT:
		return elc_cycle_wait(ctx, argp);
	case ELC_IOC_CYCLE_SET_PERIOD:
		return elc_cycle_set_period(ctx, argp);
	case ELC_IOC_GET_IO_STATUS:
		return elc_get_io_status(ctx, argp);
	case ELC_IOC_GET_INPUT_SNAPSHOT:
		return elc_get_input_snapshot(ctx, argp);
	case ELC_IOC_CONFIGURE_INPUT_HISTORY:
		return elc_configure_input_history(ctx, argp);
	case ELC_IOC_GET_INPUT_HISTORY_BATCH:
		return elc_get_input_history_batch(ctx, argp);
	case ELC_IOC_PUBLISH_OUTPUT:
		return elc_publish_output(ctx, argp);
	case ELC_IOC_ARM_OUTPUTS:
		return elc_arm_outputs(ctx, argp);
	case ELC_IOC_DISARM_OUTPUTS:
		return elc_disarm_outputs(ctx, argp);
	case ELC_IOC_GET_CONFIG_SLAVE_STATUS:
		return elc_get_config_slave_status(ctx, argp);
	case ELC_IOC_GET_DOMAIN_STATUS:
		return elc_get_domain_status(ctx, argp);
	case ELC_IOC_CONFIGURE_OUTPUT_LEASE:
		return elc_configure_output_lease(ctx, argp);
	case ELC_IOC_RENEW_OUTPUT_LEASE:
		return elc_renew_output_lease(ctx, argp);
	case ELC_IOC_GET_OUTPUT_LEASE_STATUS:
		return elc_get_output_lease_status(ctx, argp);
	default:
		break;
	}
	if (READ_ONCE(ctx->active))
		return -EBUSY;

	switch (cmd) {
	case ELC_IOC_CONFIG_BEGIN:
		return elc_config_begin(ctx, argp);
	case ELC_IOC_CONFIG_ADD_SLAVE:
		return elc_config_add_slave(ctx, argp);
	case ELC_IOC_CONFIG_ADD_SYNC:
		return elc_config_add_sync(ctx, argp);
	case ELC_IOC_CONFIG_ADD_PDO:
		return elc_config_add_pdo(ctx, argp);
	case ELC_IOC_CONFIG_ADD_ENTRY:
		return elc_config_add_entry(ctx, argp);
	case ELC_IOC_CONFIG_ADD_DC:
		return elc_config_add_dc(ctx, argp);
	case ELC_IOC_CONFIG_SET_DC_POLICY:
		return elc_config_set_dc_policy(ctx, argp);
	case ELC_IOC_CONFIG_ADD_DOMAIN:
		return elc_config_add_domain(ctx, argp);
	case ELC_IOC_CONFIG_ASSIGN_DOMAIN:
		return elc_config_assign_domain(ctx, argp);
	case ELC_IOC_CONFIG_VALIDATE:
		return elc_config_validate(ctx, argp);
	case ELC_IOC_CONFIG_APPLY:
		return elc_config_apply(ctx, argp);
	case ELC_IOC_DOMAIN_CREATE:
		return elc_domain_create(ctx, argp);
	case ELC_IOC_GET_ENTRY_OFFSET:
		return elc_get_entry_offset(ctx, argp);
	case ELC_IOC_CYCLE_ACTIVATE:
		return elc_cycle_activate(ctx, argp);
	default:
		return -ENOTTY;
	}
}

ELC_DEFINE_COMPAT_IOCTL(elc_ioctl)

static const struct file_operations elc_fops = {
	.owner = THIS_MODULE,
	.open = elc_open,
	.release = elc_release,
	.unlocked_ioctl = elc_ioctl,
	ELC_FOP_COMPAT_IOCTL
	.llseek = no_llseek,
};

static struct miscdevice elc_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "elc_ethercat0",
	.fops = &elc_fops,
	.mode = 0600,
};

static int __init elc_init(void)
{
	unsigned int runtime_magic;
	int ret;

	runtime_magic = ecrt_version_magic();
	if (runtime_magic != ECRT_VERSION_MAGIC) {
		pr_err(ELC_NAME
		       ": EtherLab API mismatch: header=0x%x runtime=0x%x\n",
		       ECRT_VERSION_MAGIC, runtime_magic);
		return -EPROTO;
	}

	ret = misc_register(&elc_miscdev);
	if (ret)
		return ret;

	pr_info(ELC_NAME ": registered /dev/%s (API %u.%u)\n",
		elc_miscdev.name, ELC_API_VERSION_MAJOR,
		ELC_API_VERSION_MINOR);
	return 0;
}

static void __exit elc_exit(void)
{
	misc_deregister(&elc_miscdev);
	pr_info(ELC_NAME ": unloaded\n");
}

module_init(elc_init);
module_exit(elc_exit);

MODULE_AUTHOR("latproc");
MODULE_DESCRIPTION("Generic EtherLab cyclic transport");
MODULE_LICENSE("GPL");
