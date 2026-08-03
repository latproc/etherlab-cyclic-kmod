/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * EtherLab private layout constants used by setup-hold.
 *
 * Public ecrt does not export a request-AL API. Setup-hold writes
 * ec_slave_config->slave->requested_state through known offsets for the
 * linked EtherLab revision (see docs/building/etherlab-dkms-environment.md).
 *
 * Offsets are verified at module build time by elc_etherlab_layout_check.c
 * against master/slave_config.h and master/slave.h when those sources are
 * available. Do not change a constant without re-running:
 *   make test-etherlab-layout
 */
#ifndef ELC_ETHERLAB_LAYOUT_H
#define ELC_ETHERLAB_LAYOUT_H

/* EtherLab 1.6.9 (ethercat-dkms) x86_64 layout, offsetof-verified. */
#define ELC_EC_SC_SLAVE_OFFSET			40
#define ELC_EC_SLAVE_REQUESTED_STATE_OFFSET	128
#define ELC_EC_SLAVE_ERROR_FLAG_OFFSET		136

/* AL state values (match ec_slave_state_t / EC_AL_STATE_*). */
#define ELC_EC_SLAVE_STATE_PREOP		0x02
#define ELC_EC_SLAVE_STATE_SAFEOP		0x04
#define ELC_EC_SLAVE_STATE_OP			0x08

#endif /* ELC_ETHERLAB_LAYOUT_H */
