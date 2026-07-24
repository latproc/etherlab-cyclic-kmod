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
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/wait.h>

#include <ecrt.h>

#include "cw_ethercat_uapi.h"

#define CW_EC_NAME "cw_ethercat"
#define CW_EC_DEACTIVATE_SETTLE_MS 5000U
#define CW_EC_DEACTIVATE_POLL_MS 10U

struct cw_ec_file {
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
	u32 config_slave_count;
	u32 config_sync_count;
	u32 config_pdo_count;
	u32 config_entry_count;
	u32 config_dc_count;
	u64 config_generation;
	struct cw_ec_config_dc_policy dc_policy;
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
	atomic_t io_outputs_armed;
	atomic_t io_rearm_required;
	atomic_t io_link_up;
	atomic_t io_current_faults;
	atomic_t io_last_latched_faults;
	atomic_t io_slaves_responding;
	atomic_t io_configured_slaves_online;
	atomic_t io_configured_slaves_operational;
	atomic64_t io_fault_count;
	atomic64_t io_fault_output_sequence;
	atomic64_t output_gate_request;
	atomic64_t output_gate_applied;
	wait_queue_head_t output_gate_wait;
	bool io_ever_healthy;
	spinlock_t input_lock;
	u8 *input_buffers[2];
	u8 input_active;
	int input_reader;
	atomic64_t input_sequence;
	spinlock_t output_lock;
	u8 *output_buffers[2];
	u8 *output_mask;
	u8 *output_update_mask;
	u8 output_active;
	int output_reader;
	atomic64_t output_sequence;
	bool active;
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
	ec_slave_config_t *ec_config;
	atomic_t state_result;
	atomic_t state_online;
	atomic_t state_operational;
	atomic_t state_al_state;
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
	u32 domain_offset;
	u8 bit_position;
	bool registered;
};

struct cw_ec_dc_node {
	struct cw_ec_config_node common;
	struct cw_ec_config_dc cfg;
};

static atomic_t cw_ec_control_open = ATOMIC_INIT(0);
static atomic64_t cw_ec_next_config_generation = ATOMIC64_INIT(0);
static atomic_t cw_ec_test_allocation_count = ATOMIC_INIT(0);
static int cw_ec_test_fail_allocation;
static bool cw_ec_test_fail_cycle_thread;
static int cw_ec_cycle_cpu = -1;
static uint cw_ec_cycle_fifo_priority;
module_param_named(test_fail_allocation, cw_ec_test_fail_allocation, int, 0444);
MODULE_PARM_DESC(test_fail_allocation,
		 "fail the Nth module-owned allocation (test only; 0 disables)");
module_param_named(test_fail_cycle_thread, cw_ec_test_fail_cycle_thread,
		   bool, 0444);
MODULE_PARM_DESC(test_fail_cycle_thread,
		 "fail cyclic task construction after activation (test only)");
module_param_named(cycle_cpu, cw_ec_cycle_cpu, int, 0444);
MODULE_PARM_DESC(cycle_cpu,
		 "cyclic task CPU affinity (-1 leaves scheduler affinity unchanged)");
module_param_named(cycle_fifo_priority, cw_ec_cycle_fifo_priority, uint, 0444);
MODULE_PARM_DESC(cycle_fifo_priority,
		 "cyclic task SCHED_FIFO priority (0 leaves normal scheduling)");

static int cw_ec_check_header(u16 struct_size, u16 api_major,
			      size_t expected_size);

static bool cw_ec_should_fail_allocation(void)
{
	int allocation;

	if (cw_ec_test_fail_allocation <= 0)
		return false;
	allocation = atomic_inc_return(&cw_ec_test_allocation_count);
	return allocation == cw_ec_test_fail_allocation;
}

static void *cw_ec_kzalloc(size_t size)
{
	if (cw_ec_should_fail_allocation())
		return NULL;
	return kzalloc(size, GFP_KERNEL);
}

static void *cw_ec_kvzalloc(size_t size)
{
	if (cw_ec_should_fail_allocation())
		return NULL;
	return kvzalloc(size, GFP_KERNEL);
}

static void cw_ec_invalidate_applied_config(struct cw_ec_file *ctx)
{
	struct cw_ec_entry_node *entry;
	struct cw_ec_slave_node *slave;

	list_for_each_entry(slave, &ctx->config_slaves, common.node)
		slave->ec_config = NULL;
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

static void cw_ec_free_input_buffers(struct cw_ec_file *ctx)
{
	kvfree(ctx->input_buffers[0]);
	kvfree(ctx->input_buffers[1]);
	ctx->input_buffers[0] = NULL;
	ctx->input_buffers[1] = NULL;
	ctx->input_active = 0;
	ctx->input_reader = -1;
	atomic64_set(&ctx->input_sequence, 0);
}

static void cw_ec_free_output_buffers(struct cw_ec_file *ctx)
{
	kvfree(ctx->output_buffers[0]);
	kvfree(ctx->output_buffers[1]);
	kvfree(ctx->output_mask);
	kvfree(ctx->output_update_mask);
	ctx->output_buffers[0] = NULL;
	ctx->output_buffers[1] = NULL;
	ctx->output_mask = NULL;
	ctx->output_update_mask = NULL;
	ctx->output_active = 0;
	ctx->output_reader = -1;
	atomic64_set(&ctx->output_sequence, 0);
}

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
	CW_EC_CLEAR_CONFIG_LIST(config_dcs);
	CW_EC_CLEAR_CONFIG_LIST(config_pdos);
	CW_EC_CLEAR_CONFIG_LIST(config_syncs);
	CW_EC_CLEAR_CONFIG_LIST(config_slaves);
#undef CW_EC_CLEAR_CONFIG_LIST

	ctx->config_slave_count = 0;
	ctx->config_sync_count = 0;
	ctx->config_pdo_count = 0;
	ctx->config_entry_count = 0;
	ctx->config_dc_count = 0;
	ctx->config_generation = 0;
	memset(&ctx->dc_policy, 0, sizeof(ctx->dc_policy));
	ctx->dc_policy_set = false;
	ctx->config_started = false;
	ctx->config_validated = false;
	ctx->config_applied = false;
	ctx->config_poisoned = false;
	ctx->domain = NULL;
	ctx->domain_registered = false;
}

static void cw_ec_update_maximum(atomic64_t *maximum, u64 value)
{
	s64 observed = atomic64_read(maximum);

	while (value > observed) {
		s64 previous = atomic64_cmpxchg(maximum, observed, value);

		if (previous == observed)
			break;
		observed = previous;
	}
}

static int cw_ec_dc_process_receive(struct cw_ec_file *ctx)
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
		ctx->dc_reference_valid = true;
		if (ctx->dc_last_reference_result)
			atomic64_inc(&ctx->dc_reference_resume_count);
	} else {
		ctx->dc_reference_valid = false;
		atomic64_inc(&ctx->dc_reference_read_error_count);
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
			cw_ec_update_maximum(
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

static int cw_ec_dc_prepare_send(struct cw_ec_file *ctx)
{
	s64 phase_step = (ctx->dc_last_difference_ns > 0) -
			 (ctx->dc_last_difference_ns < 0);
	int ret;
	int operation_result;

	ctx->application_time_ns +=
		ctx->cycle_period_ns - ctx->dc_cycle_adjustment_ns - phase_step;
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

static void cw_ec_update_io_health(struct cw_ec_file *ctx,
				   const ec_domain_state_t *domain_state)
{
	ec_master_state_t master_state = {};
	struct cw_ec_slave_node *slave;
	u32 faults = 0;
	u32 online = 0;
	u32 operational = 0;
	int ret;

	ret = ecrt_master_state(ctx->master, &master_state);
	if (ret)
		faults |= CW_EC_IO_FAULT_MASTER_STATE;
	else if (!master_state.link_up)
		faults |= CW_EC_IO_FAULT_LINK_DOWN;

	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		ec_slave_config_state_t state = {};

		ret = ecrt_slave_config_state(slave->ec_config, &state);
		atomic_set(&slave->state_result, ret);
		if (ret) {
			atomic_set(&slave->state_online, 0);
			atomic_set(&slave->state_operational, 0);
			atomic_set(&slave->state_al_state, 0);
			faults |= CW_EC_IO_FAULT_SLAVE_STATE;
			continue;
		}
		atomic_set(&slave->state_online, state.online);
		atomic_set(&slave->state_operational, state.operational);
		atomic_set(&slave->state_al_state, state.al_state);
		if (state.online)
			online++;
		else
			faults |= CW_EC_IO_FAULT_SLAVE_OFFLINE;
		if (state.operational)
			operational++;
		else
			faults |= CW_EC_IO_FAULT_SLAVE_NOT_OPERATIONAL;
	}
	if (domain_state->wc_state != EC_WC_COMPLETE)
		faults |= CW_EC_IO_FAULT_DOMAIN_INCOMPLETE;

	atomic_set(&ctx->io_link_up, master_state.link_up);
	atomic_set(&ctx->io_slaves_responding,
		   master_state.slaves_responding);
	atomic_set(&ctx->io_configured_slaves_online, online);
	atomic_set(&ctx->io_configured_slaves_operational, operational);
	atomic_set(&ctx->io_current_faults, faults);
	if (!faults) {
		ctx->io_ever_healthy = true;
		atomic_set(&ctx->io_bus_healthy, 1);
	} else if (ctx->io_ever_healthy &&
		   atomic_xchg(&ctx->io_bus_healthy, 0)) {
		atomic_set(&ctx->io_outputs_armed, 0);
		atomic64_inc(&ctx->output_gate_request);
		if (atomic_xchg(&ctx->io_rearm_required, 1))
			atomic_or(faults, &ctx->io_last_latched_faults);
		else
			atomic_set(&ctx->io_last_latched_faults, faults);
		atomic64_set(&ctx->io_fault_output_sequence,
			     atomic64_read(&ctx->output_sequence));
		atomic64_inc(&ctx->io_fault_count);
	} else {
		atomic_set(&ctx->io_outputs_armed, 0);
		atomic_set(&ctx->io_bus_healthy, 0);
		if (atomic_read(&ctx->io_rearm_required))
			atomic_or(faults, &ctx->io_last_latched_faults);
	}
}

static void cw_ec_publish_input_snapshot(struct cw_ec_file *ctx)
{
	unsigned long irq_flags;
	u8 target;

	spin_lock_irqsave(&ctx->input_lock, irq_flags);
	target = ctx->input_active ^ 1U;
	if (ctx->input_reader == target) {
		spin_unlock_irqrestore(&ctx->input_lock, irq_flags);
		return;
	}
	spin_unlock_irqrestore(&ctx->input_lock, irq_flags);

	memcpy(ctx->input_buffers[target], ctx->domain_data, ctx->domain_size);

	spin_lock_irqsave(&ctx->input_lock, irq_flags);
	ctx->input_active = target;
	atomic64_inc(&ctx->input_sequence);
	spin_unlock_irqrestore(&ctx->input_lock, irq_flags);
}

static void cw_ec_apply_outputs(struct cw_ec_file *ctx)
{
	unsigned long irq_flags;
	u8 *source = NULL;
	u8 reader = 0;
	u32 i;

	if (atomic_read(&ctx->io_outputs_armed) &&
	    atomic_read(&ctx->io_bus_healthy)) {
		spin_lock_irqsave(&ctx->output_lock, irq_flags);
		reader = ctx->output_active;
		ctx->output_reader = reader;
		source = ctx->output_buffers[reader];
		spin_unlock_irqrestore(&ctx->output_lock, irq_flags);
	}
	for (i = 0; i < ctx->domain_size; i++) {
		u8 output = source ? source[i] : 0;

		ctx->domain_data[i] =
			(ctx->domain_data[i] & ~ctx->output_mask[i]) |
			(output & ctx->output_mask[i]);
	}
	if (source) {
		spin_lock_irqsave(&ctx->output_lock, irq_flags);
		ctx->output_reader = -1;
		spin_unlock_irqrestore(&ctx->output_lock, irq_flags);
	}
}

static int cw_ec_cycle_thread(void *data)
{
	struct cw_ec_file *ctx = data;
	u64 deadline = ktime_get_ns();

	while (!kthread_should_stop()) {
		ec_domain_state_t domain_state = {};
		ktime_t expires;
		u64 now;
		int cycle_result = 0;
		int operation_result;

		deadline += ctx->cycle_period_ns;
		expires = ns_to_ktime(deadline);
		set_current_state(TASK_INTERRUPTIBLE);
		schedule_hrtimeout(&expires, HRTIMER_MODE_ABS);
		__set_current_state(TASK_RUNNING);
		if (kthread_should_stop())
			break;

		now = ktime_get_ns();
		if (now > deadline) {
			u64 lateness = now - deadline;

			cw_ec_update_maximum(&ctx->maximum_lateness_ns,
					     lateness);
			if (lateness >= ctx->cycle_period_ns) {
				atomic64_inc(&ctx->cycle_overrun_count);
				deadline = now;
			}
		}

		operation_result = ecrt_master_receive(ctx->master);
		if (operation_result && !cycle_result)
			cycle_result = operation_result;
		operation_result = ecrt_domain_process(ctx->domain);
		if (operation_result && !cycle_result)
			cycle_result = operation_result;
		if (!operation_result)
			cw_ec_publish_input_snapshot(ctx);
		if (ctx->config_dc_count)
			operation_result = cw_ec_dc_process_receive(ctx);
		else
			operation_result = 0;
		if (operation_result && !cycle_result)
			cycle_result = operation_result;
		operation_result = ecrt_domain_state(ctx->domain, &domain_state);
		if (operation_result && !cycle_result) {
			cycle_result = operation_result;
		} else if (!operation_result) {
			atomic_set(&ctx->working_counter,
				   domain_state.working_counter);
			atomic_set(&ctx->working_counter_state,
				   domain_state.wc_state);
		}
		cw_ec_update_io_health(ctx, &domain_state);
		cw_ec_apply_outputs(ctx);
		if (ctx->config_dc_count) {
			operation_result = cw_ec_dc_prepare_send(ctx);
		} else {
			ctx->application_time_ns += ctx->cycle_period_ns;
			operation_result = ecrt_master_application_time(
				ctx->master, ctx->application_time_ns);
		}
		if (operation_result && !cycle_result)
			cycle_result = operation_result;
		operation_result = ecrt_domain_queue(ctx->domain);
		if (operation_result && !cycle_result)
			cycle_result = operation_result;
		operation_result = ecrt_master_send(ctx->master);
		if (operation_result && !cycle_result)
			cycle_result = operation_result;
		if (!operation_result &&
		    atomic64_read(&ctx->output_gate_applied) !=
		    atomic64_read(&ctx->output_gate_request)) {
			atomic64_set(&ctx->output_gate_applied,
				     atomic64_read(
					     &ctx->output_gate_request));
			wake_up_interruptible(&ctx->output_gate_wait);
		}
		if (cycle_result)
			atomic64_inc(&ctx->cycle_error_count);
		atomic_set(&ctx->last_cycle_result, cycle_result);
		atomic64_inc(&ctx->cycle_count);
	}

	return 0;
}

static int cw_ec_wait_configured_slaves_settled(struct cw_ec_file *ctx)
{
	unsigned long timeout =
		jiffies + msecs_to_jiffies(CW_EC_DEACTIVATE_SETTLE_MS);

	for (;;) {
		struct cw_ec_slave_node *slave;
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
		msleep(CW_EC_DEACTIVATE_POLL_MS);
	}
}

static int cw_ec_wait_output_gate(struct cw_ec_file *ctx, u64 request)
{
	unsigned long timeout;
	long wait_result;

	timeout = msecs_to_jiffies(
		2U * DIV_ROUND_UP(ctx->cycle_period_ns, 1000000U) + 100U);
	wait_result = wait_event_interruptible_timeout(
		ctx->output_gate_wait,
		atomic64_read(&ctx->output_gate_applied) >= request,
		timeout);
	if (wait_result < 0)
		return wait_result;
	if (!wait_result)
		return -ETIMEDOUT;
	return 0;
}

static int cw_ec_deactivate_locked(struct cw_ec_file *ctx)
{
	u64 gate_request;
	int gate_ret;
	int process_ret;
	int receive_ret;
	int settle_ret;
	int ret;

	if (!ctx->active)
		return -EINVAL;

	atomic_set(&ctx->io_outputs_armed, 0);
	gate_request = atomic64_inc_return(&ctx->output_gate_request);
	gate_ret = cw_ec_wait_output_gate(ctx, gate_request);
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
	process_ret = ecrt_domain_process(ctx->domain);
	ret = ecrt_master_deactivate(ctx->master);
	ctx->active = false;
	settle_ret = ret ? 0 : cw_ec_wait_configured_slaves_settled(ctx);
	cw_ec_free_input_buffers(ctx);
	cw_ec_free_output_buffers(ctx);
	cw_ec_invalidate_applied_config(ctx);
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

static int cw_ec_open(struct inode *inode, struct file *file)
{
	struct cw_ec_file *ctx;
	ec_master_t *master;

	if (atomic_cmpxchg(&cw_ec_control_open, 0, 1) != 0)
		return -EBUSY;

	ctx = cw_ec_kzalloc(sizeof(*ctx));
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
	spin_lock_init(&ctx->input_lock);
	spin_lock_init(&ctx->output_lock);
	init_waitqueue_head(&ctx->output_gate_wait);
	ctx->input_reader = -1;
	ctx->output_reader = -1;
	INIT_LIST_HEAD(&ctx->setup_sdos);
	INIT_LIST_HEAD(&ctx->config_slaves);
	INIT_LIST_HEAD(&ctx->config_syncs);
	INIT_LIST_HEAD(&ctx->config_pdos);
	INIT_LIST_HEAD(&ctx->config_entries);
	INIT_LIST_HEAD(&ctx->config_dcs);
	atomic64_set(&ctx->cycle_count, 0);
	atomic64_set(&ctx->cycle_error_count, 0);
	atomic64_set(&ctx->cycle_overrun_count, 0);
	atomic64_set(&ctx->maximum_lateness_ns, 0);
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
	atomic_set(&ctx->io_bus_healthy, 0);
	atomic_set(&ctx->io_outputs_armed, 0);
	atomic_set(&ctx->io_rearm_required, 0);
	atomic_set(&ctx->io_link_up, 0);
	atomic_set(&ctx->io_current_faults, 0);
	atomic_set(&ctx->io_last_latched_faults, 0);
	atomic_set(&ctx->io_slaves_responding, 0);
	atomic_set(&ctx->io_configured_slaves_online, 0);
	atomic_set(&ctx->io_configured_slaves_operational, 0);
	atomic64_set(&ctx->io_fault_count, 0);
	atomic64_set(&ctx->io_fault_output_sequence, 0);
	atomic64_set(&ctx->output_gate_request, 0);
	atomic64_set(&ctx->output_gate_applied, 0);
	atomic64_set(&ctx->input_sequence, 0);
	atomic64_set(&ctx->output_sequence, 0);
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
		if (ctx->active)
			cw_ec_deactivate_locked(ctx);
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
	if (ctx->config_applied || ctx->config_poisoned ||
	    ctx->domain_registered) {
		mutex_unlock(&ctx->lock);
		return -EBUSY;
	}
	cw_ec_config_clear(ctx);
	ctx->config_started = true;
	mutex_unlock(&ctx->lock);
	return 0;
}

static long cw_ec_config_add_slave(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_slave_node *node;
	int ret;

	node = cw_ec_kzalloc(sizeof(*node));
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
	    !node->cfg.product_code || node->cfg.revision_number ||
	    node->cfg.flags) {
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

static struct cw_ec_slave_node *
cw_ec_find_slave(struct cw_ec_file *ctx, u32 config_id)
{
	struct cw_ec_slave_node *slave;

	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		if (slave->cfg.config_id == config_id)
			return slave;
	}
	return NULL;
}

static struct cw_ec_sync_node *
cw_ec_find_sync(struct cw_ec_file *ctx, u32 config_id)
{
	struct cw_ec_sync_node *sync;

	list_for_each_entry(sync, &ctx->config_syncs, common.node) {
		if (sync->cfg.config_id == config_id)
			return sync;
	}
	return NULL;
}

static struct cw_ec_pdo_node *
cw_ec_find_pdo(struct cw_ec_file *ctx, u32 config_id)
{
	struct cw_ec_pdo_node *pdo;

	list_for_each_entry(pdo, &ctx->config_pdos, common.node) {
		if (pdo->cfg.config_id == config_id)
			return pdo;
	}
	return NULL;
}

static struct cw_ec_dc_node *
cw_ec_find_dc_for_slave(struct cw_ec_file *ctx, u32 slave_config_id)
{
	struct cw_ec_dc_node *dc;

	list_for_each_entry(dc, &ctx->config_dcs, common.node) {
		if (dc->cfg.slave_config_id == slave_config_id)
			return dc;
	}
	return NULL;
}

static long cw_ec_config_apply(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_dc_node *dc;
	struct cw_ec_config_apply result;
	struct cw_ec_entry_node *entry;
	struct cw_ec_sync_node *sync;
	struct cw_ec_pdo_node *pdo;
	struct cw_ec_slave_node *slave;
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
			result.failed_object_kind = CW_EC_CONFIG_OBJECT_SLAVE;
			goto out;
		}
	}

	list_for_each_entry(sync, &ctx->config_syncs, common.node) {
		slave = cw_ec_find_slave(ctx, sync->cfg.slave_config_id);
		if (!slave) {
			ret = -ENOENT;
			result.failed_config_id = sync->cfg.config_id;
			result.failed_object_kind = CW_EC_CONFIG_OBJECT_SYNC;
			goto out;
		}
		ret = ecrt_slave_config_sync_manager(
			slave->ec_config, sync->cfg.sync_index,
			sync->cfg.direction == CW_EC_DIR_OUTPUT ?
				EC_DIR_OUTPUT : EC_DIR_INPUT,
			sync->cfg.watchdog_mode == CW_EC_WD_ENABLE ?
				EC_WD_ENABLE :
			sync->cfg.watchdog_mode == CW_EC_WD_DISABLE ?
				EC_WD_DISABLE : EC_WD_DEFAULT);
		if (ret) {
			result.failed_config_id = sync->cfg.config_id;
			result.failed_object_kind = CW_EC_CONFIG_OBJECT_SYNC;
			goto out;
		}
		ecrt_slave_config_pdo_assign_clear(slave->ec_config,
						  sync->cfg.sync_index);
	}

	list_for_each_entry(pdo, &ctx->config_pdos, common.node) {
		sync = cw_ec_find_sync(ctx, pdo->cfg.sync_config_id);
		if (!sync) {
			ret = -ENOENT;
			result.failed_config_id = pdo->cfg.config_id;
			result.failed_object_kind = CW_EC_CONFIG_OBJECT_PDO;
			goto out;
		}
		slave = cw_ec_find_slave(ctx, sync->cfg.slave_config_id);
		if (!slave) {
			ret = -ENOENT;
			result.failed_config_id = pdo->cfg.config_id;
			result.failed_object_kind = CW_EC_CONFIG_OBJECT_PDO;
			goto out;
		}
		ret = ecrt_slave_config_pdo_assign_add(
			slave->ec_config, sync->cfg.sync_index,
			pdo->cfg.pdo_index);
		if (ret) {
			result.failed_config_id = pdo->cfg.config_id;
			result.failed_object_kind = CW_EC_CONFIG_OBJECT_PDO;
			goto out;
		}
		ecrt_slave_config_pdo_mapping_clear(slave->ec_config,
						   pdo->cfg.pdo_index);
	}

	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		pdo = cw_ec_find_pdo(ctx, entry->cfg.pdo_config_id);
		if (!pdo) {
			ret = -ENOENT;
			result.failed_config_id = entry->cfg.config_id;
			result.failed_object_kind = CW_EC_CONFIG_OBJECT_ENTRY;
			goto out;
		}
		sync = cw_ec_find_sync(ctx, pdo->cfg.sync_config_id);
		slave = sync ?
			cw_ec_find_slave(ctx, sync->cfg.slave_config_id) : NULL;
		if (!slave) {
			ret = -ENOENT;
			result.failed_config_id = entry->cfg.config_id;
			result.failed_object_kind = CW_EC_CONFIG_OBJECT_ENTRY;
			goto out;
		}
		ret = ecrt_slave_config_pdo_mapping_add(
			slave->ec_config, pdo->cfg.pdo_index,
			entry->cfg.index, entry->cfg.subindex,
			entry->cfg.bit_length);
		if (ret) {
			result.failed_config_id = entry->cfg.config_id;
			result.failed_object_kind = CW_EC_CONFIG_OBJECT_ENTRY;
			goto out;
		}
	}

	list_for_each_entry(dc, &ctx->config_dcs, common.node) {
		slave = cw_ec_find_slave(ctx, dc->cfg.slave_config_id);
		if (!slave) {
			ret = -ENOENT;
			result.failed_config_id = dc->cfg.config_id;
			result.failed_object_kind = CW_EC_CONFIG_OBJECT_DC;
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
			result.failed_object_kind = CW_EC_CONFIG_OBJECT_DC;
			goto out;
		}
	}

	if (ctx->dc_policy.reference_mode != CW_EC_DC_REFERENCE_DISABLED) {
		ec_slave_config_t *reference = NULL;

		if (ctx->dc_policy.reference_mode ==
		    CW_EC_DC_REFERENCE_EXPLICIT) {
			slave = cw_ec_find_slave(
				ctx, ctx->dc_policy.reference_slave_config_id);
			if (!slave) {
				ret = -ENOENT;
				result.failed_config_id =
					ctx->dc_policy.reference_slave_config_id;
				result.failed_object_kind =
					CW_EC_CONFIG_OBJECT_DC_POLICY;
				goto out;
			}
			reference = slave->ec_config;
		}
		ret = ecrt_master_select_reference_clock(ctx->master, reference);
		if (ret) {
			result.failed_config_id =
				ctx->dc_policy.reference_slave_config_id;
			result.failed_object_kind =
				CW_EC_CONFIG_OBJECT_DC_POLICY;
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

static unsigned int cw_ec_pdo_position(struct cw_ec_file *ctx,
				       const struct cw_ec_pdo_node *target)
{
	struct cw_ec_pdo_node *pdo;
	unsigned int position = 0;

	list_for_each_entry(pdo, &ctx->config_pdos, common.node) {
		if (pdo == target)
			break;
		if (pdo->cfg.sync_config_id == target->cfg.sync_config_id)
			position++;
	}
	return position;
}

static unsigned int cw_ec_entry_position(struct cw_ec_file *ctx,
					 const struct cw_ec_entry_node *target)
{
	struct cw_ec_entry_node *entry;
	unsigned int position = 0;

	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		if (entry == target)
			break;
		if (entry->cfg.pdo_config_id == target->cfg.pdo_config_id)
			position++;
	}
	return position;
}

static long cw_ec_domain_create(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_domain_create result;
	struct cw_ec_entry_node *entry;
	struct cw_ec_slave_node *slave;
	struct cw_ec_sync_node *sync;
	struct cw_ec_pdo_node *pdo;
	unsigned int bit_position;
	bool has_registered_entry = false;
	int offset;
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
	ctx->domain = ecrt_master_create_domain(ctx->master);
	if (!ctx->domain) {
		ret = -ENOMEM;
		goto out;
	}

	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		if (!entry->cfg.entry_id)
			continue;
		pdo = cw_ec_find_pdo(ctx, entry->cfg.pdo_config_id);
		sync = pdo ? cw_ec_find_sync(ctx, pdo->cfg.sync_config_id) : NULL;
		slave = sync ?
			cw_ec_find_slave(ctx, sync->cfg.slave_config_id) : NULL;
		if (!pdo || !sync || !slave) {
			ret = -ENOENT;
			result.failed_config_id = entry->cfg.config_id;
			goto out;
		}

		bit_position = 0;
		offset = ecrt_slave_config_reg_pdo_entry_pos(
			slave->ec_config, sync->cfg.sync_index,
			cw_ec_pdo_position(ctx, pdo),
			cw_ec_entry_position(ctx, entry),
			ctx->domain, &bit_position);
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

static long cw_ec_get_entry_offset(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_entry_offset result;
	struct cw_ec_entry_node *entry;
	u32 entry_id;
	int ret = -ENOENT;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = cw_ec_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
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
			result.api_major = CW_EC_API_VERSION_MAJOR;
			result.entry_id = entry_id;
			result.domain_offset = entry->domain_offset;
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

static long cw_ec_cycle_activate(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_dc_node *dc;
	struct cw_ec_entry_node *entry;
	struct cw_ec_pdo_node *pdo;
	struct cw_ec_slave_node *slave;
	struct cw_ec_sync_node *sync;
	struct cw_ec_cycle_activate result;
	u64 first_bit;
	u64 end_bit;
	u64 bit;
	size_t domain_size;
	u32 period_ns;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = cw_ec_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.flags ||
	    result.cycle_period_ns < CW_EC_CYCLE_PERIOD_MIN_NS ||
	    result.cycle_period_ns > CW_EC_CYCLE_PERIOD_MAX_NS)
		return -EINVAL;
	if (cw_ec_cycle_cpu < -1 ||
	    (cw_ec_cycle_cpu >= 0 &&
	     (cw_ec_cycle_cpu >= nr_cpu_ids ||
	      !cpu_online(cw_ec_cycle_cpu))) ||
	    cw_ec_cycle_fifo_priority >= MAX_RT_PRIO)
		return -EINVAL;
	period_ns = result.cycle_period_ns;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = CW_EC_API_VERSION_MAJOR;
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
	domain_size = ecrt_domain_size(ctx->domain);
	if (domain_size > U32_MAX) {
		ret = -EOVERFLOW;
		goto out;
	}
	if (domain_size > CW_EC_PROCESS_IMAGE_MAX) {
		ret = -E2BIG;
		goto out;
	}
	ctx->input_buffers[0] = cw_ec_kvzalloc(domain_size);
	if (!ctx->input_buffers[0]) {
		ret = -ENOMEM;
		goto out;
	}
	ctx->input_buffers[1] = cw_ec_kvzalloc(domain_size);
	if (!ctx->input_buffers[1]) {
		ret = -ENOMEM;
		cw_ec_free_input_buffers(ctx);
		goto out;
	}
	ctx->output_buffers[0] = cw_ec_kvzalloc(domain_size);
	ctx->output_buffers[1] = cw_ec_kvzalloc(domain_size);
	ctx->output_mask = cw_ec_kvzalloc(domain_size);
	ctx->output_update_mask = cw_ec_kvzalloc(domain_size);
	if (!ctx->output_buffers[0] || !ctx->output_buffers[1] ||
	    !ctx->output_mask || !ctx->output_update_mask) {
		ret = -ENOMEM;
		cw_ec_free_input_buffers(ctx);
		cw_ec_free_output_buffers(ctx);
		goto out;
	}
	list_for_each_entry(entry, &ctx->config_entries, common.node) {
		pdo = cw_ec_find_pdo(ctx, entry->cfg.pdo_config_id);
		sync = pdo ? cw_ec_find_sync(ctx,
					     pdo->cfg.sync_config_id) : NULL;
		if (!sync || sync->cfg.direction != CW_EC_DIR_OUTPUT)
			continue;
		first_bit = (u64)entry->domain_offset * 8U +
			    entry->bit_position;
		end_bit = first_bit + entry->cfg.bit_length;
		if (end_bit > (u64)domain_size * 8U) {
			ret = -EOVERFLOW;
			cw_ec_free_input_buffers(ctx);
			cw_ec_free_output_buffers(ctx);
			goto out;
		}
		for (bit = first_bit; bit < end_bit; bit++)
			ctx->output_mask[bit / 8U] |= BIT(bit % 8U);
	}

	ctx->cycle_period_ns = period_ns;
	ctx->application_time_ns =
		div64_u64(ktime_get_ns(), period_ns) * period_ns;
	ret = ecrt_master_application_time(ctx->master,
					  ctx->application_time_ns);
	if (ret) {
		cw_ec_free_input_buffers(ctx);
		cw_ec_free_output_buffers(ctx);
		goto out;
	}
	ret = ecrt_master_activate(ctx->master);
	if (ret) {
		cw_ec_free_input_buffers(ctx);
		cw_ec_free_output_buffers(ctx);
		ctx->config_poisoned = true;
		goto out;
	}

	ctx->domain_size = domain_size;
	ctx->domain_data = ecrt_domain_data(ctx->domain);
	if (ctx->domain_size && !ctx->domain_data) {
		ret = -EFAULT;
		ecrt_master_deactivate(ctx->master);
		cw_ec_free_input_buffers(ctx);
		cw_ec_free_output_buffers(ctx);
		cw_ec_invalidate_applied_config(ctx);
		goto out;
	}

	/*
	 * No user-space process-image writer exists in API 0.4. Start every
	 * mapped output at zero before the first application datagram is sent.
	 */
	if (ctx->domain_size)
		memset(ctx->domain_data, 0, ctx->domain_size);
	atomic64_set(&ctx->cycle_count, 0);
	atomic64_set(&ctx->cycle_error_count, 0);
	atomic64_set(&ctx->cycle_overrun_count, 0);
	atomic64_set(&ctx->maximum_lateness_ns, 0);
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
	atomic_set(&ctx->io_bus_healthy, 0);
	atomic_set(&ctx->io_outputs_armed, 0);
	atomic_set(&ctx->io_rearm_required, 0);
	atomic_set(&ctx->io_link_up, 0);
	atomic_set(&ctx->io_current_faults, 0);
	atomic_set(&ctx->io_last_latched_faults, 0);
	atomic_set(&ctx->io_slaves_responding, 0);
	atomic_set(&ctx->io_configured_slaves_online, 0);
	atomic_set(&ctx->io_configured_slaves_operational, 0);
	atomic64_set(&ctx->io_fault_count, 0);
	atomic64_set(&ctx->io_fault_output_sequence, 0);
	atomic64_set(&ctx->output_gate_request, 0);
	atomic64_set(&ctx->output_gate_applied, 0);
	ctx->io_ever_healthy = false;
	list_for_each_entry(slave, &ctx->config_slaves, common.node) {
		atomic_set(&slave->state_result, -ENODATA);
		atomic_set(&slave->state_online, 0);
		atomic_set(&slave->state_operational, 0);
		atomic_set(&slave->state_al_state, 0);
	}
	ctx->input_active = 0;
	ctx->input_reader = -1;
	atomic64_set(&ctx->input_sequence, 0);
	ctx->output_active = 0;
	ctx->output_reader = -1;
	atomic64_set(&ctx->output_sequence, 0);
	if (cw_ec_test_fail_cycle_thread)
		ctx->cycle_thread = ERR_PTR(-ENOMEM);
	else
		ctx->cycle_thread = kthread_create(cw_ec_cycle_thread, ctx,
						   "cw_ec_cycle");
	if (IS_ERR(ctx->cycle_thread)) {
		ret = PTR_ERR(ctx->cycle_thread);
		ctx->cycle_thread = NULL;
		ecrt_master_deactivate(ctx->master);
		cw_ec_free_input_buffers(ctx);
		cw_ec_free_output_buffers(ctx);
		cw_ec_invalidate_applied_config(ctx);
		goto out;
	}
	if (cw_ec_cycle_cpu >= 0) {
		ret = set_cpus_allowed_ptr(ctx->cycle_thread,
					   cpumask_of(cw_ec_cycle_cpu));
		if (ret)
			goto thread_config_failed;
	}
	if (cw_ec_cycle_fifo_priority) {
		struct sched_attr attr = {
			.size = sizeof(attr),
			.sched_policy = SCHED_FIFO,
			.sched_priority = cw_ec_cycle_fifo_priority,
		};

		ret = sched_setattr_nocheck(ctx->cycle_thread, &attr);
		if (ret)
			goto thread_config_failed;
	}
	wake_up_process(ctx->cycle_thread);
	ctx->active = true;
	result.domain_size = ctx->domain_size;
	ret = 0;
	goto out;

thread_config_failed:
	kthread_stop(ctx->cycle_thread);
	ctx->cycle_thread = NULL;
	ecrt_master_deactivate(ctx->master);
	cw_ec_free_input_buffers(ctx);
	cw_ec_free_output_buffers(ctx);
	cw_ec_invalidate_applied_config(ctx);
out:
	result.result = ret;
	if (copy_to_user(argp, &result, sizeof(result))) {
		if (ctx->active)
			cw_ec_deactivate_locked(ctx);
		ret = -EFAULT;
	}
	mutex_unlock(&ctx->lock);
	return ret;
}

static long cw_ec_cycle_get_status(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_cycle_status result;
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
	result.active = ctx->active;
	result.cycle_period_ns = ctx->cycle_period_ns;
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

static long cw_ec_cycle_deactivate(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_cycle_deactivate result;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = cw_ec_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.reserved || result.reserved1)
		return -EINVAL;

	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = CW_EC_API_VERSION_MAJOR;
	mutex_lock(&ctx->lock);
	ret = cw_ec_deactivate_locked(ctx);
	result.result = ret;
	if (copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static long cw_ec_cycle_get_dc_status(struct cw_ec_file *ctx,
				      void __user *argp)
{
	struct cw_ec_dc_status result;
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

static long cw_ec_get_io_status(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_io_status result;
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
	result.bus_healthy = atomic_read(&ctx->io_bus_healthy);
	result.outputs_armed = atomic_read(&ctx->io_outputs_armed);
	result.rearm_required = atomic_read(&ctx->io_rearm_required);
	result.link_up = atomic_read(&ctx->io_link_up);
	result.current_faults = atomic_read(&ctx->io_current_faults);
	result.last_latched_faults =
		atomic_read(&ctx->io_last_latched_faults);
	result.slaves_responding =
		atomic_read(&ctx->io_slaves_responding);
	result.configured_slave_count = ctx->config_slave_count;
	result.configured_slaves_online =
		atomic_read(&ctx->io_configured_slaves_online);
	result.configured_slaves_operational =
		atomic_read(&ctx->io_configured_slaves_operational);
	result.domain_size = ctx->domain_size;
	result.config_generation = ctx->config_generation;
	result.fault_count = atomic64_read(&ctx->io_fault_count);
	result.input_sequence = atomic64_read(&ctx->input_sequence);
	result.output_sequence = atomic64_read(&ctx->output_sequence);
	mutex_unlock(&ctx->lock);

	if (copy_to_user(argp, &result, sizeof(result)))
		return -EFAULT;
	return 0;
}

static long cw_ec_get_input_snapshot(struct cw_ec_file *ctx,
				     void __user *argp)
{
	struct cw_ec_input_snapshot result;
	unsigned long irq_flags;
	void __user *data_ptr;
	u64 requested_ptr;
	u32 capacity;
	u8 reader;
	int ret = 0;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = cw_ec_check_header(result.struct_size, result.api_major,
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
	result.api_major = CW_EC_API_VERSION_MAJOR;
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
	result.cycle_count = atomic64_read(&ctx->cycle_count);
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

static long cw_ec_publish_output(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_output_publish result;
	unsigned long irq_flags;
	void __user *data_ptr;
	void __user *mask_ptr;
	u64 requested_ptr;
	u64 requested_mask_ptr;
	u64 requested_generation;
	u32 data_size;
	u32 i;
	u8 active;
	u8 target;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = cw_ec_check_header(result.struct_size, result.api_major,
				 sizeof(result));
	if (ret)
		return ret;
	if (result.flags || result.reserved || !result.data_ptr ||
	    !result.mask_ptr)
		return -EINVAL;
	requested_ptr = result.data_ptr;
	requested_mask_ptr = result.mask_ptr;
	requested_generation = result.config_generation;
	data_size = result.data_size;
	data_ptr = u64_to_user_ptr(requested_ptr);
	mask_ptr = u64_to_user_ptr(requested_mask_ptr);

	mutex_lock(&ctx->lock);
	if (!ctx->active || !ctx->output_buffers[0] ||
	    !ctx->output_buffers[1] || !ctx->output_mask) {
		ret = -EINVAL;
		goto out;
	}
	if (requested_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}
	if (data_size != ctx->domain_size) {
		ret = -EMSGSIZE;
		goto out;
	}

	spin_lock_irqsave(&ctx->output_lock, irq_flags);
	active = ctx->output_active;
	target = active ^ 1U;
	if (ctx->output_reader == target) {
		spin_unlock_irqrestore(&ctx->output_lock, irq_flags);
		ret = -EBUSY;
		goto out;
	}
	spin_unlock_irqrestore(&ctx->output_lock, irq_flags);
	if (copy_from_user(ctx->output_buffers[target], data_ptr, data_size)) {
		ret = -EFAULT;
		goto out;
	}
	if (copy_from_user(ctx->output_update_mask, mask_ptr, data_size)) {
		ret = -EFAULT;
		goto out;
	}
	for (i = 0; i < data_size; i++) {
		u8 mask = ctx->output_update_mask[i] & ctx->output_mask[i];
		u8 old = ctx->output_buffers[active][i];

		ctx->output_buffers[target][i] =
			(ctx->output_buffers[target][i] & mask) |
			(old & ~mask);
	}

	spin_lock_irqsave(&ctx->output_lock, irq_flags);
	ctx->output_active = target;
	result.output_sequence = atomic64_inc_return(
		&ctx->output_sequence);
	spin_unlock_irqrestore(&ctx->output_lock, irq_flags);
	result.config_generation = ctx->config_generation;
	ret = 0;
out:
	if (!ret && copy_to_user(argp, &result, sizeof(result)))
		ret = -EFAULT;
	mutex_unlock(&ctx->lock);
	return ret;
}

static long cw_ec_arm_outputs(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_output_arm request;
	u64 current_sequence;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = cw_ec_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (request.flags)
		return -EINVAL;

	mutex_lock(&ctx->lock);
	if (!ctx->active) {
		ret = -EINVAL;
		goto out;
	}
	if (request.config_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}
	current_sequence = atomic64_read(&ctx->output_sequence);
	if (!current_sequence || request.output_sequence != current_sequence) {
		ret = -ESTALE;
		goto out;
	}
	if (!atomic_read(&ctx->io_bus_healthy)) {
		ret = -EAGAIN;
		goto out;
	}
	if (atomic_read(&ctx->io_rearm_required) &&
	    current_sequence <=
		    atomic64_read(&ctx->io_fault_output_sequence)) {
		ret = -EAGAIN;
		goto out;
	}
	atomic_set(&ctx->io_rearm_required, 0);
	atomic_set(&ctx->io_outputs_armed, 1);
	ret = 0;
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static long cw_ec_disarm_outputs(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_output_disarm request;
	u64 gate_request;
	int ret;

	if (copy_from_user(&request, argp, sizeof(request)))
		return -EFAULT;
	ret = cw_ec_check_header(request.struct_size, request.api_major,
				 sizeof(request));
	if (ret)
		return ret;
	if (request.flags)
		return -EINVAL;

	mutex_lock(&ctx->lock);
	if (!ctx->active) {
		ret = -EINVAL;
		goto out;
	}
	if (request.config_generation != ctx->config_generation) {
		ret = -ESTALE;
		goto out;
	}
	atomic_set(&ctx->io_outputs_armed, 0);
	atomic_set(&ctx->io_rearm_required, 1);
	atomic64_set(&ctx->io_fault_output_sequence,
		     atomic64_read(&ctx->output_sequence));
	gate_request = atomic64_inc_return(&ctx->output_gate_request);
	ret = cw_ec_wait_output_gate(ctx, gate_request);
	if (ret)
		goto out;
out:
	mutex_unlock(&ctx->lock);
	return ret;
}

static long cw_ec_get_config_slave_status(struct cw_ec_file *ctx,
					   void __user *argp)
{
	struct cw_ec_config_slave_status result;
	struct cw_ec_slave_node *slave;
	u64 requested_generation;
	u32 config_id;
	int ret;

	if (copy_from_user(&result, argp, sizeof(result)))
		return -EFAULT;
	ret = cw_ec_check_header(result.struct_size, result.api_major,
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
	slave = cw_ec_find_slave(ctx, config_id);
	if (!slave) {
		ret = -ENOENT;
		goto out;
	}
	memset(&result, 0, sizeof(result));
	result.struct_size = sizeof(result);
	result.api_major = CW_EC_API_VERSION_MAJOR;
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
		result.data_valid =
			!result.state_result && result.online &&
			result.operational && result.input_sequence &&
			atomic_read(&ctx->working_counter_state) ==
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

#define CW_EC_CONFIG_ADD_CHILD(function_name, node_type, cfg_member, list_name, \
			       count_name, max_count, validate_expr) \
static long function_name(struct cw_ec_file *ctx, void __user *argp) \
{ \
	struct node_type *node; \
	int ret; \
	node = cw_ec_kzalloc(sizeof(*node)); \
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
		       !node->cfg.pdo_config_id || !node->cfg.bit_length ||
		       ((!node->cfg.entry_id || !node->cfg.index) &&
			(node->cfg.entry_id || node->cfg.index ||
			 node->cfg.subindex)))

CW_EC_CONFIG_ADD_CHILD(cw_ec_config_add_dc, cw_ec_dc_node, cfg,
		       config_dcs, config_dc_count, CW_EC_CONFIG_DC_MAX,
		       !node->cfg.slave_config_id ||
		       !node->cfg.assign_activate ||
		       !node->cfg.sync0_cycle_ns ||
		       node->cfg.reserved0 || node->cfg.flags)

#undef CW_EC_CONFIG_ADD_CHILD

static long cw_ec_config_set_dc_policy(struct cw_ec_file *ctx,
				       void __user *argp)
{
	struct cw_ec_config_dc_policy policy;
	int ret;

	if (copy_from_user(&policy, argp, sizeof(policy)))
		return -EFAULT;
	ret = cw_ec_check_header(policy.struct_size, policy.api_major,
				 sizeof(policy));
	if (ret)
		return ret;
	if (policy.reference_mode > CW_EC_DC_REFERENCE_EXPLICIT ||
	    memchr_inv(policy.reserved0, 0, sizeof(policy.reserved0)) ||
	    policy.flags ||
	    (policy.reference_mode == CW_EC_DC_REFERENCE_EXPLICIT) !=
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

static long cw_ec_config_validate(struct cw_ec_file *ctx, void __user *argp)
{
	struct cw_ec_dc_node *dc;
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
		struct cw_ec_slave_node *other;

		list_for_each_entry(other, &ctx->config_slaves, common.node) {
			if (other != slave && other->cfg.alias == slave->cfg.alias &&
			    other->cfg.position == slave->cfg.position) {
				ret = -EEXIST;
				goto out;
			}
		}
	}
	list_for_each_entry(dc, &ctx->config_dcs, common.node) {
		struct cw_ec_dc_node *other;

		if (!cw_ec_config_id_exists(&ctx->config_slaves,
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
	if (ctx->dc_policy.reference_mode == CW_EC_DC_REFERENCE_EXPLICIT &&
	    (!cw_ec_config_id_exists(
		     &ctx->config_slaves,
		     ctx->dc_policy.reference_slave_config_id) ||
	     !cw_ec_find_dc_for_slave(
		     ctx, ctx->dc_policy.reference_slave_config_id))) {
		ret = -ENOENT;
		goto out;
	}
	if (ctx->dc_policy.reference_mode != CW_EC_DC_REFERENCE_DISABLED &&
	    !ctx->config_dc_count) {
		ret = -EINVAL;
		goto out;
	}
	if (ctx->config_dc_count &&
	    ctx->dc_policy.reference_mode == CW_EC_DC_REFERENCE_DISABLED) {
		ret = -EINVAL;
		goto out;
	}
	ctx->config_validated = true;
	ctx->config_generation =
		atomic64_inc_return(&cw_ec_next_config_generation);
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
	entry = cw_ec_kzalloc(allocation_size);
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
	case CW_EC_IOC_CYCLE_GET_STATUS:
		return cw_ec_cycle_get_status(ctx, argp);
	case CW_EC_IOC_CYCLE_DEACTIVATE:
		return cw_ec_cycle_deactivate(ctx, argp);
	case CW_EC_IOC_CYCLE_GET_DC_STATUS:
		return cw_ec_cycle_get_dc_status(ctx, argp);
	case CW_EC_IOC_GET_IO_STATUS:
		return cw_ec_get_io_status(ctx, argp);
	case CW_EC_IOC_GET_INPUT_SNAPSHOT:
		return cw_ec_get_input_snapshot(ctx, argp);
	case CW_EC_IOC_PUBLISH_OUTPUT:
		return cw_ec_publish_output(ctx, argp);
	case CW_EC_IOC_ARM_OUTPUTS:
		return cw_ec_arm_outputs(ctx, argp);
	case CW_EC_IOC_DISARM_OUTPUTS:
		return cw_ec_disarm_outputs(ctx, argp);
	case CW_EC_IOC_GET_CONFIG_SLAVE_STATUS:
		return cw_ec_get_config_slave_status(ctx, argp);
	default:
		break;
	}
	if (READ_ONCE(ctx->active))
		return -EBUSY;

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
	case CW_EC_IOC_CONFIG_ADD_DC:
		return cw_ec_config_add_dc(ctx, argp);
	case CW_EC_IOC_CONFIG_SET_DC_POLICY:
		return cw_ec_config_set_dc_policy(ctx, argp);
	case CW_EC_IOC_CONFIG_VALIDATE:
		return cw_ec_config_validate(ctx, argp);
	case CW_EC_IOC_CONFIG_APPLY:
		return cw_ec_config_apply(ctx, argp);
	case CW_EC_IOC_DOMAIN_CREATE:
		return cw_ec_domain_create(ctx, argp);
	case CW_EC_IOC_GET_ENTRY_OFFSET:
		return cw_ec_get_entry_offset(ctx, argp);
	case CW_EC_IOC_CYCLE_ACTIVATE:
		return cw_ec_cycle_activate(ctx, argp);
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
