// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compile-time gate: fail the module build if EtherLab private struct
 * layout no longer matches ELC_EC_*_OFFSET constants used by setup-hold.
 *
 * Built only when ETHERLAB_SOURCE points at a tree that contains
 * master/slave_config.h (normal DKMS source layout).
 */

#include <linux/module.h>
#include <linux/stddef.h>
#include <linux/types.h>

/*
 * Pull EtherLab private types. Paths and stub config.h are supplied via
 * CFLAGS_elc_etherlab_layout_check.o in Kbuild.
 */
#include "globals.h"
#include "master/slave_config.h"
#include "master/slave.h"

#include "elc_etherlab_layout.h"

/*
 * File-scope negative-size arrays fail the compile if offsetof drifts.
 *
 * If any of these fail after an EtherLab upgrade: re-measure offsets, update
 * elc_etherlab_layout.h, then re-run:
 *   make test-etherlab-layout
 *   ELC_MOTION_INHIBITED=YES ./tools/elc_test_setup_hold.sh
 */
static char elc_layout_assert_sc_slave
	[(offsetof(ec_slave_config_t, slave) == ELC_EC_SC_SLAVE_OFFSET) ? 1 :
									  -1]
	__used;
static char elc_layout_assert_requested
	[(offsetof(ec_slave_t, requested_state) ==
	  ELC_EC_SLAVE_REQUESTED_STATE_OFFSET) ?
		 1 :
		 -1]
	__used;
static char elc_layout_assert_error_flag
	[(offsetof(ec_slave_t, error_flag) == ELC_EC_SLAVE_ERROR_FLAG_OFFSET) ?
		 1 :
		 -1]
	__used;
static char elc_layout_assert_preop
	[((int)EC_SLAVE_STATE_PREOP == ELC_EC_SLAVE_STATE_PREOP) ? 1 : -1]
	__used;
static char elc_layout_assert_safeop
	[((int)EC_SLAVE_STATE_SAFEOP == ELC_EC_SLAVE_STATE_SAFEOP) ? 1 : -1]
	__used;
static char elc_layout_assert_op
	[((int)EC_SLAVE_STATE_OP == ELC_EC_SLAVE_STATE_OP) ? 1 : -1] __used;

void *elc_etherlab_layout_check_anchor(void)
{
	return elc_layout_assert_sc_slave;
}
