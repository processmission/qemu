/*
 * K230 Real-Time Clock
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_RTC_K230_RTC_H
#define HW_RTC_K230_RTC_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_K230_RTC "riscv.k230.rtc"
OBJECT_DECLARE_SIMPLE_TYPE(K230RtcState, K230_RTC)

struct K230RtcState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    QEMUTimer *timer;
    qemu_irq irq;

    int64_t tick_offset;
    int64_t count_base_ns;
    uint32_t alarm_date;
    uint32_t alarm_time;
    uint32_t count;
    uint32_t int_ctrl;
    bool irq_pending;
};

#endif /* HW_RTC_K230_RTC_H */
