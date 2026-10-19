/*
 * SiFli SF32LB52X watchdog timer
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SF32LB52X_WDT_H
#define HW_MISC_SF32LB52X_WDT_H

#include "hw/core/ptimer.h"
#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_WDT "sifli.sf32lb52x-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XWdtState, SF32LB52X_WDT)

#define SF32LB52X_WDT_SIZE 0x1000

/* Register offsets. */
#define SF32LB52X_WDT_CVR0   0x00
#define SF32LB52X_WDT_CVR1   0x04
#define SF32LB52X_WDT_CR     0x08
#define SF32LB52X_WDT_CCR    0x0c
#define SF32LB52X_WDT_ICR    0x10
#define SF32LB52X_WDT_SR     0x14
#define SF32LB52X_WDT_WP     0x18
#define SF32LB52X_WDT_FG     0x1c

#define SF32LB52X_WDT_CVR_MASK  0x00ffffff
#define SF32LB52X_WDT_CMD_START 0x00000076
#define SF32LB52X_WDT_CMD_STOP  0x00000034

/* CR */
#define SF32LB52X_WDT_CR_RESPONSE_MODE (1U << 4)

/* SR */
#define SF32LB52X_WDT_SR_INT_ASSERT (1U << 0)
#define SF32LB52X_WDT_SR_ACTIVE     (1U << 1)

/* WP magic values */
#define SF32LB52X_WDT_WP_SET     0x58ab99fc
#define SF32LB52X_WDT_WP_RELEASE 0x51ff8621
#define SF32LB52X_WDT_WP_ST      (1U << 31)

/* FG */
#define SF32LB52X_WDT_FG_RST_FG     (1U << 1)
#define SF32LB52X_WDT_FG_RST_FG_CLR (1U << 0)

/*
 * The watchdog is clocked from the 10 kHz LRC10 oscillator; the Zephyr
 * driver programs its counters assuming 10 counts per millisecond.
 */
#define SF32LB52X_WDT_CLK_HZ 10000

struct SF32LB52XWdtState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    ptimer_state *timer;
    QEMUTimer *second_timer;

    uint32_t cvr0;
    uint32_t cvr1;
    uint32_t cr;
    uint32_t ccr;
    uint32_t interrupt_asserted;
    uint32_t protected;
    uint32_t reset_flag;
    bool running;
    bool second_stage;
};

#endif /* HW_MISC_SF32LB52X_WDT_H */
