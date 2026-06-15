/*
 * Minimal RK3588 SCMI clock responder.
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Local-only board model. Implements the subset of the ARM SCMI
 * Clock protocol (0x14) needed by the dw_mmc-rockchip SD-card
 * driver's sdmmc@fe2c0000 clock requests, transported via SMC
 * (arm,scmi-smc, smc-id 0x82000010) with a single shared-memory
 * slot at 0x10f000 (size 0x100).
 *
 * The SMC trap path in target/arm/tcg/psci.c routes any SMC whose
 * function-id is 0x82000010 (registered by the board via
 * arm_register_psci_smc_handler) into rk3588_scmi_handle_smc(),
 * which parses the shmem message header + payload, writes a canned
 * response into the same shmem, sets CHANNEL_FREE, and returns
 * x0=0 to the guest.
 *
 * Implemented messages (status=SUCCESS unless noted):
 *   BASE (0x10):
 *     PROTOCOL_VERSION (0x0)        ->  {minor=0, major=2}
 *     PROTOCOL_ATTRIBUTES (0x1)     ->  num_agents=1, num_protocols=2
 *     PROTOCOL_MESSAGE_ATTRIBUTES  ->  attributes=0
 *   CLOCK (0x14):
 *     PROTOCOL_VERSION (0x0)        ->  {minor=0, major=3}
 *     PROTOCOL_ATTRIBUTES (0x1)     ->  num_clocks=24
 *     PROTOCOL_MESSAGE_ATTRIBUTES  ->  attributes=0
 *     CLOCK_ATTRIBUTES (0x3)        ->  {attributes=0; name; latency=0}
 *     CLOCK_RATE_GET (0x6)          ->  nominal per-clock rate
 *     CLOCK_RATE_SET (0x5)          ->  SUCCESS (tracks last-set rate)
 *     CLOCK_CONFIG_SET (0x7)        ->  SUCCESS (tracks enable)
 *     NEGOTIATE_PROTOCOL_VERSION (0x10) -> SUCCESS
 *
 * Out-of-range clk_id returns SCMI_ERR_NOT_FOUND (-2); unsupported
 * messages return SCMI_ERR_SUPPORT (-1). All values are
 * little-endian. No IRQ, no notifications, no fastchannels.
 */

#ifndef HW_MISC_RK3588_SCMI_H
#define HW_MISC_RK3588_SCMI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_SCMI "rk3588-scmi"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588SCMIState, RK3588_SCMI)

#define RK3588_SCMI_SHMEM_SIZE 0x100
#define RK3588_SCMI_NUM_CLOCKS 24

/*
 * SMC function-id of the SCMI transport. Match the DTS `arm,smc-id`.
 */
#define RK3588_SCMI_SMC_ID 0x82000010ULL

struct RK3588SCMIState {
    SysBusDevice parent_obj;

    MemoryRegion shmem;

    /* Backing store for the 0x100 shmem MMIO region. */
    uint8_t shmem_buf[RK3588_SCMI_SHMEM_SIZE];

    /* Per-clock rate (Hz) and enable state. */
    uint64_t rate[RK3588_SCMI_NUM_CLOCKS];
    bool enabled[RK3588_SCMI_NUM_CLOCKS];
};

/*
 * Run the SCMI responder against the shmem buffer.
 *
 * Reads the request header + payload from the shmem buffer, writes the
 * canned response into the same buffer (header echoed, status=SUCCESS,
 * CHANNEL_FREE set), and returns true. Returns false if the buffer
 * does not contain a well-formed request.
 *
 * Exposed so the SMC hook (target/arm/tcg/psci.c) can call it via the
 * board-registered callback without referencing the device instance
 * directly; the board keeps a pointer to the realized device.
 */
bool rk3588_scmi_handle_smc(RK3588SCMIState *s);

#endif /* HW_MISC_RK3588_SCMI_H */
