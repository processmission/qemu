/*
 * SiFli SF32LB52X ATIM/GPTIM PWM timers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * STM32-compatible timer bank used as a PWM source by the Zephyr
 * pwm_sf32lb_atim/pwm_sf32lb_gpt drivers. Neither driver uses interrupts:
 * the model keeps the counter free-running on the virtual clock, exposes
 * CNT, and derives update/compare flags on register reads.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/timer/sf32lb52x_pwm.h"
#include "migration/vmstate.h"
#include "qemu/host-utils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

/* Register offsets (SVD ATIM1/GPTIM2). */
#define R_CR1    0x00
#define R_DIER   0x0c
#define R_SR     0x10
#define R_EGR    0x14
#define R_CCMR1  0x18
#define R_CCMR2  0x1c
#define R_CCER   0x20
#define R_CNT    0x24
#define R_PSC    0x28
#define R_ARR    0x2c
#define R_RCR    0x30
#define R_CCR1   0x34
#define R_CCR2   0x38
#define R_CCR3   0x3c
#define R_CCR4   0x40
#define R_BDTR   0x44
#define R_CCMR3  0x54
#define R_CCR5   0x58
#define R_CCR6   0x5c
#define R_AF1    0x60
#define R_AF2    0x64

#define CR1_CEN   (1U << 0)
#define CR1_ARPE  (1U << 7)
#define SR_UIF    (1U << 0)
#define SR_CCIF(ch) (1U << (ch))       /* ch 1..4 -> bits 1..4 */
#define SR_TIF    (1U << 6)
#define EGR_UG    (1U << 0)
#define EGR_CCG(ch) (1U << (ch))
#define EGR_TG    (1U << 6)
#define DIER_MASK 0x7fU
#define BDTR_MOE  (1U << 15)
#define CCER_CCE(ch) (1U << (4 * ((ch) - 1)))

#define ATIM_FREQ_HZ 120000000ULL
#define GPTIM_FREQ_HZ 24000000ULL

static uint32_t pwm_width_mask(SF32LB52XPWMState *s)
{
    return s->gptim ? 0xffffU : 0xffffffffU;
}

static uint32_t pwm_counter_mask(SF32LB52XPWMState *s)
{
    return s->gptim ? 0xffffU : 0xffffffffU;
}

static uint32_t pwm_arr_value(SF32LB52XPWMState *s)
{
    uint32_t arr = s->regs[R_ARR / 4] & pwm_width_mask(s);

    return arr;
}

static uint64_t pwm_period(SF32LB52XPWMState *s)
{
    return (uint64_t)pwm_arr_value(s) + 1;
}

static uint64_t pwm_counter_clock(SF32LB52XPWMState *s)
{
    uint64_t freq = s->gptim ? GPTIM_FREQ_HZ : ATIM_FREQ_HZ;
    uint32_t psc = (s->regs[R_PSC / 4] & 0xffffU) + 1;

    return freq / psc;
}

static void sf32lb52x_pwm_update_irq(SF32LB52XPWMState *s)
{
    qemu_set_irq(s->irq, (s->regs[R_SR / 4] & s->regs[R_DIER / 4] &
                          DIER_MASK) != 0);
}

/* Advance the free-running counter and derive flags since the last access. */
static void sf32lb52x_pwm_refresh(SF32LB52XPWMState *s)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t elapsed, ticks, period, total;
    uint64_t counter_clock;
    uint32_t count;
    int ch;

    if (!(s->regs[R_CR1 / 4] & CR1_CEN)) {
        s->base_ns = now;
        return;
    }

    counter_clock = pwm_counter_clock(s);
    elapsed = now - s->base_ns;
    ticks = muldiv64(elapsed, counter_clock, NANOSECONDS_PER_SECOND);
    if (ticks == 0) {
        return;
    }

    period = pwm_period(s);
    total = (uint64_t)s->base_count + ticks;
    count = total % period;

    if (total >= period) {
        s->regs[R_SR / 4] |= SR_UIF;
    }
    for (ch = 1; ch <= 4; ch++) {
        uint32_t ccr = s->regs[R_CCR1 / 4 + (ch - 1)] & pwm_width_mask(s);

        if ((s->regs[R_CCER / 4] & CCER_CCE(ch)) &&
            (count <= ccr || total - s->base_count > period)) {
            s->regs[R_SR / 4] |= SR_CCIF(ch);
        }
    }
    if (s->regs[R_CR1 / 4] & CR1_ARPE) {
        /* Preload: reading the update event is all the model needs. */
        s->regs[R_SR / 4] |= 0;
    }

    s->base_count = count;
    s->base_ns = now;
    sf32lb52x_pwm_update_irq(s);
}

static uint64_t sf32lb52x_pwm_read(void *opaque, hwaddr addr, unsigned int size)
{
    SF32LB52XPWMState *s = opaque;
    uint32_t value;

    if (addr >= SF32LB52X_PWM_SIZE || (addr % 4) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    switch (addr) {
    case R_CNT:
        sf32lb52x_pwm_refresh(s);
        return s->base_count & pwm_counter_mask(s);
    case R_SR:
        sf32lb52x_pwm_refresh(s);
        return s->regs[R_SR / 4];
    default:
        value = s->regs[addr / 4];
        return value;
    }
}

static void sf32lb52x_pwm_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned int size)
{
    SF32LB52XPWMState *s = opaque;
    uint32_t val = value;

    if (addr >= SF32LB52X_PWM_SIZE || (addr % 4) != 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case R_CR1:
        sf32lb52x_pwm_refresh(s);
        s->regs[R_CR1 / 4] = val;
        if (val & CR1_CEN) {
            s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        }
        break;
    case R_CNT:
        sf32lb52x_pwm_refresh(s);
        s->base_count = val & pwm_counter_mask(s);
        s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        break;
    case R_PSC:
    case R_ARR:
        sf32lb52x_pwm_refresh(s);
        s->regs[addr / 4] = val & (addr == R_PSC ? 0xffffU
                                                  : pwm_width_mask(s));
        break;
    case R_SR:
        /* rc_w0: writing 0 clears a flag, writing 1 leaves it set. */
        s->regs[R_SR / 4] &= val;
        sf32lb52x_pwm_update_irq(s);
        break;
    case R_EGR:
        sf32lb52x_pwm_refresh(s);
        if (val & EGR_UG) {
            s->base_count = 0;
            s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->regs[R_SR / 4] |= SR_UIF;
        }
        if (val & EGR_TG) {
            s->regs[R_SR / 4] |= SR_TIF;
        }
        for (int ch = 1; ch <= 4; ch++) {
            if (val & EGR_CCG(ch)) {
                s->regs[R_SR / 4] |= SR_CCIF(ch);
            }
        }
        sf32lb52x_pwm_update_irq(s);
        break;
    case R_DIER:
        s->regs[R_DIER / 4] = val & DIER_MASK;
        sf32lb52x_pwm_update_irq(s);
        break;
    case R_CCR1:
    case R_CCR2:
    case R_CCR3:
    case R_CCR4:
    case R_CCR5:
    case R_CCR6:
        s->regs[addr / 4] = val & pwm_width_mask(s);
        break;
    case R_RCR:
        s->regs[addr / 4] = val & (s->gptim ? 0xffU : 0xffffU);
        break;
    case R_BDTR:
        s->regs[addr / 4] = val;
        break;
    default:
        s->regs[addr / 4] = val;
        break;
    }
}

static const MemoryRegionOps sf32lb52x_pwm_ops = {
    .read = sf32lb52x_pwm_read,
    .write = sf32lb52x_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void sf32lb52x_pwm_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XPWMState *s = SF32LB52X_PWM(obj);

    memset(s->regs, 0, sizeof(s->regs));
    if (!s->gptim) {
        s->regs[R_AF1 / 4] = 1;
        s->regs[R_AF2 / 4] = 1;
    }
    s->base_count = 0;
    s->base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    sf32lb52x_pwm_update_irq(s);
}

static void sf32lb52x_pwm_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XPWMState *s = SF32LB52X_PWM(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sf32lb52x_pwm_ops, s,
                          TYPE_SF32LB52X_PWM, SF32LB52X_PWM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const Property sf32lb52x_pwm_properties[] = {
    DEFINE_PROP_BOOL("gptim", SF32LB52XPWMState, gptim, false),
};

static const VMStateDescription vmstate_sf32lb52x_pwm = {
    .name = TYPE_SF32LB52X_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, SF32LB52XPWMState, 0x400),
        VMSTATE_UINT64(base_ns, SF32LB52XPWMState),
        VMSTATE_UINT32(base_count, SF32LB52XPWMState),
        VMSTATE_BOOL(gptim, SF32LB52XPWMState),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb52x_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_pwm_reset_hold;
    dc->realize = sf32lb52x_pwm_realize;
    dc->desc = "SiFli SF32LB52X ATIM/GPTIM PWM";
    dc->vmsd = &vmstate_sf32lb52x_pwm;
    dc->user_creatable = false;
    device_class_set_props(dc, sf32lb52x_pwm_properties);
}

static const TypeInfo sf32lb52x_pwm_types[] = {
    {
        .name = TYPE_SF32LB52X_PWM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XPWMState),
        .class_init = sf32lb52x_pwm_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_pwm_types);
