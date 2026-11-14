/*
 * Rockchip RK3588 PWM controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_RK3588_PWM_H
#define HW_MISC_RK3588_PWM_H

#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_RK3588_PWM "rk3588-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588PWMState, RK3588_PWM)

#define RK3588_PWM_MMIO_SIZE        0x100
#define RK3588_PWM_NUM_CHANNELS     4
#define RK3588_PWM_DEFAULT_CLOCK_HZ 24000000
#define RK3588_PWM_OSC_CLOCK_HZ     24000000
/* TRM V1.0 p1411; p1425 conflicts (0x021311b6, RW). */
#define RK3588_PWM_VERSION_ID       0x02120b34

typedef struct RK3588PWMChannel {
    RK3588PWMState *parent;
    QEMUTimer *timer;
    uint32_t active_period;
    uint32_t active_duty;
    uint32_t active_ctrl;
    uint32_t active_root_hz;
    uint32_t active_divider;
    int64_t cycle_start_ns;
    bool running;
    bool timer_at_period_end;
    bool output_level;
} RK3588PWMChannel;

struct RK3588PWMState {
    SysBusDevice parent_obj;

    uint32_t regs[RK3588_PWM_MMIO_SIZE / 4];
    RegisterInfo regs_info[RK3588_PWM_MMIO_SIZE / 4];
    RegisterInfoArray *reg_array;
    uint32_t clock_hz;
    uint32_t osc_clock_hz;
    RK3588PWMChannel channels[RK3588_PWM_NUM_CHANNELS];
    qemu_irq outputs[RK3588_PWM_NUM_CHANNELS];
};

#endif /* HW_MISC_RK3588_PWM_H */
