/*
 * STM32G474 USB full-speed device controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/irq.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/usb/stm32g474_usbfs.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/visitor.h"
#include "system/qtest.h"

#define STM32G474_USBFS_NUM_REGS \
    (0x5c / sizeof(uint32_t))
#define STM32G474_USBFS_NUM_BDT_REGS 32

struct Stm32g474UsbFsState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[STM32G474_USBFS_NUM_REGS];
    uint32_t regs[STM32G474_USBFS_NUM_REGS];

    MemoryRegion pma_mr;
    uint16_t pma[STM32G474_USBFS_PMA_SIZE / sizeof(uint16_t)];
    RegisterInfo bdt_regs[STM32G474_USBFS_NUM_BDT_REGS];

    Clock *pclk;
    Clock *usb;
    qemu_irq hp_irq;
    qemu_irq lp_irq;

    bool resetting;
    bool peripheral_reset_asserted;
    bool fres_active;
    bool fres_reset_pending;
};

REG32(EP0R, 0x00)
    FIELD(EP0R, EA, 0, 4)
    FIELD(EP0R, STAT_TX, 4, 2)
    FIELD(EP0R, DTOG_TX, 6, 1)
    FIELD(EP0R, CTR_TX, 7, 1)
    FIELD(EP0R, EP_KIND, 8, 1)
    FIELD(EP0R, EP_TYPE, 9, 2)
    FIELD(EP0R, SETUP, 11, 1)
    FIELD(EP0R, STAT_RX, 12, 2)
    FIELD(EP0R, DTOG_RX, 14, 1)
    FIELD(EP0R, CTR_RX, 15, 1)
REG32(EP1R, 0x04)
REG32(EP2R, 0x08)
REG32(EP3R, 0x0c)
REG32(EP4R, 0x10)
REG32(EP5R, 0x14)
REG32(EP6R, 0x18)
REG32(EP7R, 0x1c)

REG32(CNTR, 0x40)
    FIELD(CNTR, FRES, 0, 1)
    FIELD(CNTR, PDWN, 1, 1)
    FIELD(CNTR, LPMODE, 2, 1)
    FIELD(CNTR, FSUSP, 3, 1)
    FIELD(CNTR, RESUME, 4, 1)
    FIELD(CNTR, L1RESUME, 5, 1)
    FIELD(CNTR, L1REQM, 7, 1)
    FIELD(CNTR, ESOFM, 8, 1)
    FIELD(CNTR, SOFM, 9, 1)
    FIELD(CNTR, RESETM, 10, 1)
    FIELD(CNTR, SUSPM, 11, 1)
    FIELD(CNTR, WKUPM, 12, 1)
    FIELD(CNTR, ERRM, 13, 1)
    FIELD(CNTR, PMAOVRM, 14, 1)
    FIELD(CNTR, CTRM, 15, 1)
REG32(ISTR, 0x44)
    FIELD(ISTR, EP_ID, 0, 4)
    FIELD(ISTR, DIR, 4, 1)
    FIELD(ISTR, L1REQ, 7, 1)
    FIELD(ISTR, ESOF, 8, 1)
    FIELD(ISTR, SOF, 9, 1)
    FIELD(ISTR, RESET, 10, 1)
    FIELD(ISTR, SUSP, 11, 1)
    FIELD(ISTR, WKUP, 12, 1)
    FIELD(ISTR, ERR, 13, 1)
    FIELD(ISTR, PMAOVR, 14, 1)
    FIELD(ISTR, CTR, 15, 1)
REG32(FNR, 0x48)
REG32(DADDR, 0x4c)
    FIELD(DADDR, ADD, 0, 7)
    FIELD(DADDR, EF, 7, 1)
REG32(BTABLE, 0x50)
    FIELD(BTABLE, TABLE, 3, 13)
REG32(LPMCSR, 0x54)
    FIELD(LPMCSR, LPMEN, 0, 1)
    FIELD(LPMCSR, LPMACK, 1, 1)
REG32(BCDR, 0x58)
    FIELD(BCDR, BCDEN, 0, 1)
    FIELD(BCDR, DCDEN, 1, 1)
    FIELD(BCDR, PDEN, 2, 1)
    FIELD(BCDR, SDEN, 3, 1)
    FIELD(BCDR, DPPU, 15, 1)

#define USBFS_NUM_ENDPOINTS 8
#define USBFS_BDT_WINDOW_SIZE 64
#define USBFS_FUNCTIONAL_CLOCK_HZ 48000000
#define USBFS_EP_TYPE_BULK 0
#define USBFS_EP_TYPE_ISOCHRONOUS 2

#define USBFS_EPR_CTR_MASK \
    (R_EP0R_CTR_RX_MASK | R_EP0R_CTR_TX_MASK)
#define USBFS_EPR_TOGGLE_MASK \
    (R_EP0R_DTOG_RX_MASK | R_EP0R_STAT_RX_MASK | \
     R_EP0R_DTOG_TX_MASK | R_EP0R_STAT_TX_MASK)
#define USBFS_EPR_RW_MASK \
    (R_EP0R_EP_TYPE_MASK | R_EP0R_EP_KIND_MASK | R_EP0R_EA_MASK)
#define USBFS_ISTR_STATUS_MASK \
    (R_ISTR_PMAOVR_MASK | R_ISTR_ERR_MASK | R_ISTR_WKUP_MASK | \
     R_ISTR_SUSP_MASK | R_ISTR_RESET_MASK | R_ISTR_SOF_MASK | \
     R_ISTR_ESOF_MASK | R_ISTR_L1REQ_MASK)
#define USBFS_ISTR_DERIVED_MASK \
    (R_ISTR_CTR_MASK | R_ISTR_DIR_MASK | R_ISTR_EP_ID_MASK)
#define USBFS_LPMCSR_LINK_MASK 0x00f8
#define USBFS_BCDR_PHY_MASK 0x00f0
#define USBFS_QTEST_CTR_EP_SHIFT 16
#define USBFS_QTEST_CTR_EP_LENGTH 3

static const unsigned int stm32g474_usbfs_ep_regs[USBFS_NUM_ENDPOINTS] = {
    R_EP0R, R_EP1R, R_EP2R, R_EP3R,
    R_EP4R, R_EP5R, R_EP6R, R_EP7R,
};

static bool stm32g474_usbfs_ep_is_high_priority(uint32_t epr)
{
    unsigned int ep_type = FIELD_EX32(epr, EP0R, EP_TYPE);

    return ep_type == USBFS_EP_TYPE_ISOCHRONOUS ||
           (ep_type == USBFS_EP_TYPE_BULK &&
            (epr & R_EP0R_EP_KIND_MASK));
}

static uint32_t stm32g474_usbfs_find_pending_endpoint(
    Stm32g474UsbFsState *s, bool high_priority)
{
    for (unsigned int ep = 0; ep < USBFS_NUM_ENDPOINTS; ep++) {
        uint32_t epr = s->regs[stm32g474_usbfs_ep_regs[ep]];

        if (stm32g474_usbfs_ep_is_high_priority(epr) != high_priority) {
            continue;
        }

        if (epr & R_EP0R_CTR_RX_MASK) {
            return R_ISTR_CTR_MASK | R_ISTR_DIR_MASK | ep;
        }
        if (epr & R_EP0R_CTR_TX_MASK) {
            return R_ISTR_CTR_MASK | ep;
        }
    }

    return 0;
}

static void stm32g474_usbfs_recompute_istr(Stm32g474UsbFsState *s,
                                            uint32_t high_ctr,
                                            uint32_t low_ctr)
{
    uint32_t derived = high_ctr ? high_ctr : low_ctr;

    s->regs[R_ISTR] = (s->regs[R_ISTR] & ~USBFS_ISTR_DERIVED_MASK) |
                      derived;
}

static bool stm32g474_usbfs_clocks_valid(Stm32g474UsbFsState *s)
{
    return clock_get_hz(s->pclk) > 0 &&
           clock_get_hz(s->usb) == USBFS_FUNCTIONAL_CLOCK_HZ;
}

static bool stm32g474_usbfs_fres_asserted(Stm32g474UsbFsState *s)
{
    return (s->regs[R_CNTR] & R_CNTR_FRES_MASK) != 0;
}

static void stm32g474_usbfs_update_irqs(Stm32g474UsbFsState *s)
{
    uint32_t high_ctr;
    uint32_t low_ctr;
    bool hp_level;
    bool lp_level;

    if (s->resetting) {
        return;
    }

    high_ctr = stm32g474_usbfs_find_pending_endpoint(s, true);
    low_ctr = stm32g474_usbfs_find_pending_endpoint(s, false);
    stm32g474_usbfs_recompute_istr(s, high_ctr, low_ctr);
    if (s->peripheral_reset_asserted ||
        !stm32g474_usbfs_clocks_valid(s)) {
        qemu_set_irq(s->hp_irq, 0);
        qemu_set_irq(s->lp_irq, 0);
        return;
    }

    /*
     * RM0440 45.6.1 defines the endpoint dedicated interrupt condition as
     * independent of CNTR.CTRM. The HP output models that condition.
     */
    hp_level = high_ctr != 0;
    lp_level = (s->regs[R_ISTR] & s->regs[R_CNTR] & 0x7f80) ||
               (low_ctr && (s->regs[R_CNTR] & R_CNTR_CTRM_MASK));
    qemu_set_irq(s->hp_irq, hp_level);
    qemu_set_irq(s->lp_irq, lp_level);
}

static void stm32g474_usbfs_qtest_set_ctr(Object *obj, Visitor *v,
                                          const char *name, void *opaque,
                                          Error **errp)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(obj);
    uint32_t value;
    uint32_t ctr;
    unsigned int ep;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }

    ep = extract32(value, USBFS_QTEST_CTR_EP_SHIFT,
                   USBFS_QTEST_CTR_EP_LENGTH);
    ctr = value & USBFS_EPR_CTR_MASK;
    if (!ctr || value & ~(USBFS_EPR_CTR_MASK |
                          MAKE_64BIT_MASK(USBFS_QTEST_CTR_EP_SHIFT,
                                          USBFS_QTEST_CTR_EP_LENGTH))) {
        error_setg(errp, "%s: invalid endpoint completion 0x%x",
                   TYPE_STM32G474_USBFS, value);
        return;
    }
    if (s->resetting || s->peripheral_reset_asserted ||
        stm32g474_usbfs_fres_asserted(s) ||
        !stm32g474_usbfs_clocks_valid(s)) {
        return;
    }

    s->regs[stm32g474_usbfs_ep_regs[ep]] |= ctr;
    stm32g474_usbfs_update_irqs(s);
}

static bool stm32g474_usbfs_btable_valid(Stm32g474UsbFsState *s)
{
    uint32_t btable = s->regs[R_BTABLE] & R_BTABLE_TABLE_MASK;

    return btable <= STM32G474_USBFS_PMA_SIZE - USBFS_BDT_WINDOW_SIZE;
}

static void stm32g474_usbfs_rebind_bdt(Stm32g474UsbFsState *s)
{
    uint32_t btable = s->regs[R_BTABLE] & R_BTABLE_TABLE_MASK;

    if (!stm32g474_usbfs_btable_valid(s)) {
        return;
    }

    for (unsigned int i = 0; i < STM32G474_USBFS_NUM_BDT_REGS; i++) {
        s->bdt_regs[i].data = &s->pma[btable / sizeof(uint16_t) + i];
    }
}

static void stm32g474_usbfs_bus_reset(Stm32g474UsbFsState *s)
{
    for (unsigned int ep = 0; ep < USBFS_NUM_ENDPOINTS; ep++) {
        unsigned int reg = stm32g474_usbfs_ep_regs[ep];

        s->regs[reg] &= USBFS_EPR_CTR_MASK;
    }
    s->regs[R_DADDR] = 0;
    s->regs[R_ISTR] |= R_ISTR_RESET_MASK;
    stm32g474_usbfs_update_irqs(s);
}

static bool stm32g474_usbfs_apply_pending_fres(
    Stm32g474UsbFsState *s)
{
    if (!s->fres_reset_pending ||
        !stm32g474_usbfs_fres_asserted(s) ||
        !stm32g474_usbfs_clocks_valid(s)) {
        return false;
    }

    s->fres_reset_pending = false;
    stm32g474_usbfs_bus_reset(s);
    return true;
}

static uint64_t stm32g474_usbfs_epr_pre_write(RegisterInfo *reg,
                                               uint64_t val)
{
    uint32_t old = *(uint32_t *)reg->data;
    uint32_t ctr = old & val & USBFS_EPR_CTR_MASK;
    uint32_t toggle = (old ^ val) & USBFS_EPR_TOGGLE_MASK;
    uint32_t setup = old & R_EP0R_SETUP_MASK;
    uint32_t rw = val & USBFS_EPR_RW_MASK;

    return ctr | toggle | setup | rw;
}

static void stm32g474_usbfs_epr_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(reg->opaque);

    stm32g474_usbfs_update_irqs(s);
}

static uint64_t stm32g474_usbfs_istr_pre_write(RegisterInfo *reg,
                                                uint64_t val)
{
    uint32_t old = *(uint32_t *)reg->data;
    uint32_t status = old & val & USBFS_ISTR_STATUS_MASK;

    return status | (old & USBFS_ISTR_DERIVED_MASK);
}

static void stm32g474_usbfs_istr_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(reg->opaque);

    stm32g474_usbfs_update_irqs(s);
}

static void stm32g474_usbfs_cntr_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(reg->opaque);
    bool fres = (val & R_CNTR_FRES_MASK) != 0;

    if (s->resetting) {
        s->fres_active = fres;
        return;
    }

    if (val & R_CNTR_L1RESUME_MASK) {
        qemu_log_mask(LOG_UNIMP,
                      TYPE_STM32G474_USBFS
                      ": L1 resume signaling is not implemented\n");
        s->regs[R_CNTR] &= ~R_CNTR_L1RESUME_MASK;
    }
    if (val & R_CNTR_RESUME_MASK) {
        qemu_log_mask(LOG_UNIMP,
                      TYPE_STM32G474_USBFS
                      ": resume signaling is not implemented\n");
    }

    if (!s->fres_active && fres) {
        s->fres_reset_pending = true;
    } else if (!fres) {
        s->fres_reset_pending = false;
    }
    s->fres_active = fres;
    if (!stm32g474_usbfs_apply_pending_fres(s)) {
        stm32g474_usbfs_update_irqs(s);
    }
}

static void stm32g474_usbfs_btable_post_write(RegisterInfo *reg, uint64_t val)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(reg->opaque);

    stm32g474_usbfs_rebind_bdt(s);
}

#define USBFS_EP_ACCESS(_ep) \
    { \
        .name = "EP" #_ep "R", \
        .addr = A_EP ## _ep ## R, \
        .reset = 0, \
        .ro = R_EP0R_SETUP_MASK, \
        .rsvd = 0xffff0000, \
        .pre_write = stm32g474_usbfs_epr_pre_write, \
        .post_write = stm32g474_usbfs_epr_post_write, \
    }

static const RegisterAccessInfo stm32g474_usbfs_regs_info[] = {
    USBFS_EP_ACCESS(0),
    USBFS_EP_ACCESS(1),
    USBFS_EP_ACCESS(2),
    USBFS_EP_ACCESS(3),
    USBFS_EP_ACCESS(4),
    USBFS_EP_ACCESS(5),
    USBFS_EP_ACCESS(6),
    USBFS_EP_ACCESS(7),
    {
        .name = "CNTR",
        .addr = A_CNTR,
        .reset = 0x0003,
        .rsvd = 0xffff0040,
        .post_write = stm32g474_usbfs_cntr_post_write,
    }, {
        .name = "ISTR",
        .addr = A_ISTR,
        .ro = USBFS_ISTR_DERIVED_MASK,
        .rsvd = 0xffff0060,
        .pre_write = stm32g474_usbfs_istr_pre_write,
        .post_write = stm32g474_usbfs_istr_post_write,
    }, {
        .name = "FNR",
        .addr = A_FNR,
        .ro = 0x0000ffff,
        .rsvd = 0xffff0000,
    }, {
        .name = "DADDR",
        .addr = A_DADDR,
        .rsvd = 0xffffff00,
    }, {
        .name = "BTABLE",
        .addr = A_BTABLE,
        .rsvd = 0xffff0007,
        .post_write = stm32g474_usbfs_btable_post_write,
    }, {
        .name = "LPMCSR",
        .addr = A_LPMCSR,
        .ro = USBFS_LPMCSR_LINK_MASK,
        .rsvd = 0xffffff04,
    }, {
        .name = "BCDR",
        .addr = A_BCDR,
        .ro = USBFS_BCDR_PHY_MASK,
        .rsvd = 0xffff7f00,
    },
};

#undef USBFS_EP_ACCESS

#define USBFS_BDT_ADDR_TX_ACCESS(_ep) \
    { \
        .name = "EP" #_ep "_ADDR_TX", \
        .addr = (_ep) * 8, \
        .rsvd = 0x0001, \
    }
#define USBFS_BDT_COUNT_TX_ACCESS(_ep) \
    { \
        .name = "EP" #_ep "_COUNT_TX", \
        .addr = (_ep) * 8 + 2, \
    }
#define USBFS_BDT_ADDR_RX_ACCESS(_ep) \
    { \
        .name = "EP" #_ep "_ADDR_RX", \
        .addr = (_ep) * 8 + 4, \
        .rsvd = 0x0001, \
    }
#define USBFS_BDT_COUNT_RX_ACCESS(_ep) \
    { \
        .name = "EP" #_ep "_COUNT_RX", \
        .addr = (_ep) * 8 + 6, \
        .ro = 0x03ff, \
    }

static const RegisterAccessInfo stm32g474_usbfs_bdt_info[] = {
    USBFS_BDT_ADDR_TX_ACCESS(0),
    USBFS_BDT_COUNT_TX_ACCESS(0),
    USBFS_BDT_ADDR_RX_ACCESS(0),
    USBFS_BDT_COUNT_RX_ACCESS(0),
    USBFS_BDT_ADDR_TX_ACCESS(1),
    USBFS_BDT_COUNT_TX_ACCESS(1),
    USBFS_BDT_ADDR_RX_ACCESS(1),
    USBFS_BDT_COUNT_RX_ACCESS(1),
    USBFS_BDT_ADDR_TX_ACCESS(2),
    USBFS_BDT_COUNT_TX_ACCESS(2),
    USBFS_BDT_ADDR_RX_ACCESS(2),
    USBFS_BDT_COUNT_RX_ACCESS(2),
    USBFS_BDT_ADDR_TX_ACCESS(3),
    USBFS_BDT_COUNT_TX_ACCESS(3),
    USBFS_BDT_ADDR_RX_ACCESS(3),
    USBFS_BDT_COUNT_RX_ACCESS(3),
    USBFS_BDT_ADDR_TX_ACCESS(4),
    USBFS_BDT_COUNT_TX_ACCESS(4),
    USBFS_BDT_ADDR_RX_ACCESS(4),
    USBFS_BDT_COUNT_RX_ACCESS(4),
    USBFS_BDT_ADDR_TX_ACCESS(5),
    USBFS_BDT_COUNT_TX_ACCESS(5),
    USBFS_BDT_ADDR_RX_ACCESS(5),
    USBFS_BDT_COUNT_RX_ACCESS(5),
    USBFS_BDT_ADDR_TX_ACCESS(6),
    USBFS_BDT_COUNT_TX_ACCESS(6),
    USBFS_BDT_ADDR_RX_ACCESS(6),
    USBFS_BDT_COUNT_RX_ACCESS(6),
    USBFS_BDT_ADDR_TX_ACCESS(7),
    USBFS_BDT_COUNT_TX_ACCESS(7),
    USBFS_BDT_ADDR_RX_ACCESS(7),
    USBFS_BDT_COUNT_RX_ACCESS(7),
};

#undef USBFS_BDT_COUNT_RX_ACCESS
#undef USBFS_BDT_ADDR_RX_ACCESS
#undef USBFS_BDT_COUNT_TX_ACCESS
#undef USBFS_BDT_ADDR_TX_ACCESS

G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_usbfs_regs_info) == 15);
G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_usbfs_bdt_info) ==
                STM32G474_USBFS_NUM_BDT_REGS);
G_STATIC_ASSERT(STM32G474_USBFS_NUM_REGS == R_BCDR + 1);

static void stm32g474_usbfs_regs_write(void *opaque, hwaddr addr,
                                       uint64_t value, unsigned int size)
{
    RegisterInfoArray *reg_array = opaque;
    Stm32g474UsbFsState *s =
        STM32G474_USBFS(register_array_get_owner(reg_array));
    bool cntr_access = addr >= A_CNTR &&
                       addr + size <= A_CNTR + sizeof(uint32_t);

    /*
     * RM0440 45.6.1: FRES holds the USB peripheral in reset until cleared.
     * CNTR remains writable so software can release that reset.
     */
    if (s->resetting || s->peripheral_reset_asserted ||
        (stm32g474_usbfs_fres_asserted(s) && !cntr_access)) {
        return;
    }
    register_write_memory(opaque, addr, value, size);
}

static const MemoryRegionOps stm32g474_usbfs_regs_ops = {
    .read = register_read_memory,
    .write = stm32g474_usbfs_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 2,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static bool stm32g474_usbfs_pma_valid(hwaddr addr, unsigned int size)
{
    bool valid_size = size == 1 || size == 2;

    if (!valid_size || (addr & (size - 1)) ||
        addr > STM32G474_USBFS_PMA_SIZE - size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      TYPE_STM32G474_USBFS
                      ": invalid PMA access at 0x%" HWADDR_PRIx
                      " of size %u\n", addr, size);
        return false;
    }
    return true;
}

static bool stm32g474_usbfs_pma_is_bdt(Stm32g474UsbFsState *s,
                                        hwaddr addr, unsigned int size)
{
    uint32_t btable = s->regs[R_BTABLE] & R_BTABLE_TABLE_MASK;

    return stm32g474_usbfs_btable_valid(s) &&
           addr >= btable &&
           addr + size <= btable + USBFS_BDT_WINDOW_SIZE;
}

static uint64_t stm32g474_usbfs_pma_read(void *opaque, hwaddr addr,
                                         unsigned int size)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(opaque);
    unsigned int shift = (addr & 1) * 8;
    uint64_t mask = MAKE_64BIT_MASK(shift, size * 8);
    uint16_t value;

    if (!stm32g474_usbfs_pma_valid(addr, size)) {
        return 0;
    }

    if (stm32g474_usbfs_pma_is_bdt(s, addr, size)) {
        uint32_t btable = s->regs[R_BTABLE] & R_BTABLE_TABLE_MASK;
        unsigned int index = (addr - btable) / sizeof(uint16_t);

        value = register_read(&s->bdt_regs[index], mask,
                              TYPE_STM32G474_USBFS, false);
    } else {
        value = s->pma[addr / sizeof(uint16_t)];
    }

    return extract32(value, shift, size * 8);
}

static void stm32g474_usbfs_pma_write(void *opaque, hwaddr addr,
                                      uint64_t value, unsigned int size)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(opaque);
    unsigned int shift = (addr & 1) * 8;
    uint64_t mask = MAKE_64BIT_MASK(shift, size * 8);
    uint16_t lane_value = (value << shift) & mask;

    if (!stm32g474_usbfs_pma_valid(addr, size) ||
        s->resetting || s->peripheral_reset_asserted) {
        return;
    }

    if (stm32g474_usbfs_pma_is_bdt(s, addr, size)) {
        uint32_t btable = s->regs[R_BTABLE] & R_BTABLE_TABLE_MASK;
        unsigned int index = (addr - btable) / sizeof(uint16_t);

        register_write(&s->bdt_regs[index], lane_value, mask,
                       TYPE_STM32G474_USBFS, false);
    } else {
        unsigned int index = addr / sizeof(uint16_t);

        s->pma[index] = (s->pma[index] & ~mask) | lane_value;
    }
}

static const MemoryRegionOps stm32g474_usbfs_pma_ops = {
    .read = stm32g474_usbfs_pma_read,
    .write = stm32g474_usbfs_pma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 2,
        .unaligned = false,
    },
};

static void stm32g474_usbfs_reset_registers(Stm32g474UsbFsState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->pma, 0, sizeof(s->pma));

    for (unsigned int i = 0;
         i < ARRAY_SIZE(stm32g474_usbfs_regs_info); i++) {
        const RegisterAccessInfo *access = &stm32g474_usbfs_regs_info[i];

        register_reset(&s->regs_info[access->addr / sizeof(uint32_t)]);
    }

    stm32g474_usbfs_rebind_bdt(s);
    s->fres_active = (s->regs[R_CNTR] & R_CNTR_FRES_MASK) != 0;
    s->fres_reset_pending = false;
}

static void stm32g474_usbfs_reset_input(void *opaque, int n, int level)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(opaque);
    bool asserted = level != 0;
    bool was_resetting;

    if (asserted == s->peripheral_reset_asserted) {
        return;
    }

    s->peripheral_reset_asserted = asserted;
    if (asserted) {
        was_resetting = s->resetting;
        s->resetting = true;
        stm32g474_usbfs_reset_registers(s);
        qemu_set_irq(s->hp_irq, 0);
        qemu_set_irq(s->lp_irq, 0);
        s->resetting = was_resetting;
    } else if (!s->resetting) {
        stm32g474_usbfs_update_irqs(s);
    }
}

static void stm32g474_usbfs_reset_enter(Object *obj, ResetType type)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(obj);

    s->resetting = true;
}

static void stm32g474_usbfs_reset_hold(Object *obj, ResetType type)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(obj);

    stm32g474_usbfs_reset_registers(s);
    s->peripheral_reset_asserted = false;
    qemu_set_irq(s->hp_irq, 0);
    qemu_set_irq(s->lp_irq, 0);
}

static void stm32g474_usbfs_reset_exit(Object *obj, ResetType type)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(obj);

    s->resetting = false;
    stm32g474_usbfs_update_irqs(s);
}

static bool stm32g474_usbfs_fixed_slot_defined(unsigned int slot)
{
    for (unsigned int i = 0;
         i < ARRAY_SIZE(stm32g474_usbfs_regs_info); i++) {
        if (stm32g474_usbfs_regs_info[i].addr / sizeof(uint32_t) == slot) {
            return true;
        }
    }
    return false;
}

static bool stm32g474_usbfs_migration_valid(Stm32g474UsbFsState *s)
{
    for (unsigned int i = 0; i < STM32G474_USBFS_NUM_REGS; i++) {
        if (!stm32g474_usbfs_fixed_slot_defined(i) && s->regs[i]) {
            return false;
        }
    }

    for (unsigned int i = 0;
         i < ARRAY_SIZE(stm32g474_usbfs_regs_info); i++) {
        const RegisterAccessInfo *access = &stm32g474_usbfs_regs_info[i];
        uint32_t value = s->regs[access->addr / sizeof(uint32_t)];

        if (value & access->rsvd) {
            return false;
        }
    }

    return s->regs[R_FNR] == 0 &&
           !(s->regs[R_LPMCSR] & USBFS_LPMCSR_LINK_MASK) &&
           !(s->regs[R_BCDR] & USBFS_BCDR_PHY_MASK) &&
           !(s->regs[R_CNTR] & R_CNTR_L1RESUME_MASK) &&
           (!s->fres_reset_pending ||
            (stm32g474_usbfs_fres_asserted(s) &&
             !stm32g474_usbfs_clocks_valid(s)));
}

static int stm32g474_usbfs_post_load(void *opaque, int version_id)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(opaque);

    if (!stm32g474_usbfs_migration_valid(s)) {
        return -EINVAL;
    }

    s->resetting = true;
    if (s->peripheral_reset_asserted) {
        stm32g474_usbfs_reset_registers(s);
    } else {
        stm32g474_usbfs_rebind_bdt(s);
    }
    s->fres_active = (s->regs[R_CNTR] & R_CNTR_FRES_MASK) != 0;
    s->resetting = false;
    stm32g474_usbfs_update_irqs(s);
    return 0;
}

static void stm32g474_usbfs_clock_update(void *opaque, ClockEvent event)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(opaque);

    if (!stm32g474_usbfs_apply_pending_fres(s)) {
        stm32g474_usbfs_update_irqs(s);
    }
}

static const VMStateDescription vmstate_stm32g474_usbfs = {
    .name = TYPE_STM32G474_USBFS,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = stm32g474_usbfs_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Stm32g474UsbFsState,
                             STM32G474_USBFS_NUM_REGS),
        VMSTATE_UINT16_ARRAY(pma, Stm32g474UsbFsState,
                             STM32G474_USBFS_PMA_SIZE / sizeof(uint16_t)),
        VMSTATE_BOOL(peripheral_reset_asserted, Stm32g474UsbFsState),
        VMSTATE_BOOL(fres_reset_pending, Stm32g474UsbFsState),
        VMSTATE_CLOCK(pclk, Stm32g474UsbFsState),
        VMSTATE_CLOCK(usb, Stm32g474UsbFsState),
        VMSTATE_END_OF_LIST()
    },
};

static void stm32g474_usbfs_realize(DeviceState *dev, Error **errp)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(dev);

    if (!clock_has_source(s->pclk)) {
        error_setg(errp, TYPE_STM32G474_USBFS
                   ": pclk clock must be connected");
        return;
    }
    if (!clock_has_source(s->usb)) {
        error_setg(errp, TYPE_STM32G474_USBFS
                   ": usb clock must be connected");
    }
}

static void stm32g474_usbfs_init(Object *obj)
{
    Stm32g474UsbFsState *s = STM32G474_USBFS(obj);
    DeviceState *dev = DEVICE(obj);

    s->reg_array = register_init_block32(
        dev, stm32g474_usbfs_regs_info,
        ARRAY_SIZE(stm32g474_usbfs_regs_info), s->regs_info, s->regs,
        &stm32g474_usbfs_regs_ops, false, STM32G474_USBFS_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->reg_array->mem);

    memory_region_init_io(&s->pma_mr, obj, &stm32g474_usbfs_pma_ops, s,
                          TYPE_STM32G474_USBFS ".pma",
                          STM32G474_USBFS_PMA_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->pma_mr);

    for (unsigned int i = 0; i < STM32G474_USBFS_NUM_BDT_REGS; i++) {
        s->bdt_regs[i] = (RegisterInfo) {
            .data = &s->pma[i],
            .data_size = sizeof(uint16_t),
            .access = &stm32g474_usbfs_bdt_info[i],
            .opaque = s,
        };
    }

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->hp_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->lp_irq);
    s->pclk = qdev_init_clock_in(dev, "pclk",
                                 stm32g474_usbfs_clock_update,
                                 s, ClockUpdate);
    s->usb = qdev_init_clock_in(dev, "usb",
                                stm32g474_usbfs_clock_update,
                                s, ClockUpdate);
    qdev_init_gpio_in_named(dev, stm32g474_usbfs_reset_input, "reset", 1);

    if (qtest_enabled()) {
        object_property_add(obj, "qtest-set-ctr", "uint32", NULL,
                            stm32g474_usbfs_qtest_set_ctr, NULL, NULL);
    }
}

static void stm32g474_usbfs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = stm32g474_usbfs_realize;
    dc->vmsd = &vmstate_stm32g474_usbfs;
    dc->user_creatable = false;
    rc->phases.enter = stm32g474_usbfs_reset_enter;
    rc->phases.hold = stm32g474_usbfs_reset_hold;
    rc->phases.exit = stm32g474_usbfs_reset_exit;
}

static const TypeInfo stm32g474_usbfs_info = {
    .name = TYPE_STM32G474_USBFS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stm32g474UsbFsState),
    .instance_init = stm32g474_usbfs_init,
    .class_init = stm32g474_usbfs_class_init,
};

static void stm32g474_usbfs_register_types(void)
{
    type_register_static(&stm32g474_usbfs_info);
}

type_init(stm32g474_usbfs_register_types)
