/*
 * Minimal RK3588 CRU (Clock-and-Reset-Unit) stub.
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Local-only board model. The CRU is a CLK_OF_DECLARE provider that
 * registers BOTH the rockchip clock framework (clk-rk3588) and the
 * rockchip reset controller (rst-rk3588). The five in-scope drivers
 * (UART, eMMC, GMAC, PCIe, GPIO) only need fire-and-forget
 * clk_prepare_enable / reset_control_assert|deassert calls to land
 * without aborting; no clock rates or PLL state are modelled.
 *
 * Special-case PLL lock status:
 *
 *   - 0x600 (RK3588_GRF_SOC_STATUS0, in CRU space despite the "GRF"
 *     name) returns 0xffffffff so the early PLL-lock-status poll in
 *     rockchip_clk_register_plls sees every PLL locked and skips its
 *     wait. Returning 0 here hangs clk-rk3588 init very early (before
 *     the console is up), so this read is load-bearing.
 *   - per-PLL status offsets at +0x18 return bit 15 set so SPL PLL
 *     rate changes do not wait forever.
 *
 * Other offsets are RAM-backed and support Rockchip HIWORD-mask writes.
 */

#ifndef HW_MISC_RK3588_CRU_H
#define HW_MISC_RK3588_CRU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_RK3588_CRU "rk3588-cru"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588CRUState, RK3588_CRU)

#define RK3588_CRU_SIZE       0x5c000
#define RK3588_CRU_PLL_STATUS 0x600   /* RK3588_GRF_SOC_STATUS0; returns 0xffffffff */

struct RK3588CRUState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[RK3588_CRU_SIZE / 4];
};

#endif /* HW_MISC_RK3588_CRU_H */
