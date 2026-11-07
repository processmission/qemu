/*
 * SiFli SF32LB52X RTC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SF32LB52X_RTC_H
#define HW_MISC_SF32LB52X_RTC_H

#include "hw/core/ptimer.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SF32LB52X_RTC "sifli.sf32lb52x-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(SF32LB52XRtcState, SF32LB52X_RTC)

#define SF32LB52X_RTC_SIZE 0x1000

/* BKP0R .. BKP9R: battery-backed registers. */
#define SF32LB52X_RTC_NUM_BKP 10

/*
 * The calendar is driven by a fixed 1 Hz timer: the RTC clock source and the
 * PSCLR divider chain are not modeled (no clock input is wired on the SoC).
 */
#define SF32LB52X_RTC_TICK_HZ 1

/* Shadow-register resynchronization latency (about 2 RTCCLK periods). */
#define SF32LB52X_RTC_SYNC_DELAY_NS 200000

struct SF32LB52XRtcState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;
    /* HPSYS_CFG register bank: receives the TR/DR shadow mirrors. */
    MemoryRegion *cfg;

    ptimer_state *tick_timer;   /* 1 Hz BCD calendar */
    QEMUTimer *sync_timer;      /* RSF re-assert after guest clears it */

    uint32_t tr;                /* BCD time */
    uint32_t dr;                /* BCD date */
    uint32_t cr;
    uint32_t psc;
    uint32_t alrmtr;
    uint32_t alrmdr;
    uint32_t isr_flags;         /* rw0c flag bits: ALRMF, WUTF, TSF, TSOVF */
    uint32_t bkp[SF32LB52X_RTC_NUM_BKP];

    bool init_mode;             /* ISR.INIT */
    bool inits;                 /* ISR.INITS, sticky after first init */
    bool rsf;                   /* ISR.RSF */
};

#endif /* HW_MISC_SF32LB52X_RTC_H */
