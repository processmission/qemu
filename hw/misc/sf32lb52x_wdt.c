/*
 * SiFli SF32LB52X watchdog timer
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sf32lb52x_wdt.h"
#include "migration/vmstate.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/runstate.h"

static void sf32lb52x_wdt_update_irq(SF32LB52XWdtState *s)
{
    qemu_set_irq(s->irq, s->interrupt_asserted != 0);
}

static void sf32lb52x_wdt_stop_timer(SF32LB52XWdtState *s)
{
    ptimer_transaction_begin(s->timer);
    ptimer_stop(s->timer);
    ptimer_transaction_commit(s->timer);
}

static void sf32lb52x_wdt_reload(SF32LB52XWdtState *s, uint32_t count)
{
    ptimer_transaction_begin(s->timer);
    ptimer_set_count(s->timer, count);
    ptimer_run(s->timer, 1);
    ptimer_transaction_commit(s->timer);
}

static void sf32lb52x_wdt_reset_request(SF32LB52XWdtState *s)
{
    /*
     * The watchdog reset cause is kept in FG.RST_FG and survives the
     * system reset so that firmware can report why it restarted.
     */
    s->reset_flag = 1;
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void sf32lb52x_wdt_second_expire(void *opaque)
{
    SF32LB52XWdtState *s = opaque;

    if (s->running && s->second_stage) {
        sf32lb52x_wdt_reset_request(s);
    }
}

static void sf32lb52x_wdt_expire(void *opaque)
{
    SF32LB52XWdtState *s = opaque;

    if (!s->running) {
        return;
    }

    if (!(s->cr & SF32LB52X_WDT_CR_RESPONSE_MODE)) {
        /* Mode 0 (the Zephyr driver default): the first timeout resets. */
        sf32lb52x_wdt_reset_request(s);
        return;
    }

    /*
     * Mode 1: the first timeout raises the interrupt, the second one
     * resets. The second stage uses a plain QEMUTimer because ptimer
     * callbacks may run inside a ptimer transaction and must not call
     * the ptimer API again.
     */
    if (!s->second_stage) {
        s->second_stage = true;
        s->interrupt_asserted = 1;
        sf32lb52x_wdt_update_irq(s);
        if (s->cvr1 != 0) {
            int64_t ticks = (int64_t)s->cvr1 * NANOSECONDS_PER_SECOND /
                            SF32LB52X_WDT_CLK_HZ;
            timer_mod(s->second_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ticks);
        }
    }
}

static uint64_t sf32lb52x_wdt_read(void *opaque, hwaddr addr, unsigned int size)
{
    SF32LB52XWdtState *s = opaque;
    uint32_t value;

    switch (addr) {
    case SF32LB52X_WDT_CVR0:
        value = s->cvr0;
        break;
    case SF32LB52X_WDT_CVR1:
        value = s->cvr1;
        break;
    case SF32LB52X_WDT_CR:
        value = s->cr;
        break;
    case SF32LB52X_WDT_CCR:
        value = s->ccr;
        break;
    case SF32LB52X_WDT_ICR:
        value = 0;
        break;
    case SF32LB52X_WDT_SR:
        value = (s->interrupt_asserted ? SF32LB52X_WDT_SR_INT_ASSERT : 0) |
                (s->running ? SF32LB52X_WDT_SR_ACTIVE : 0);
        break;
    case SF32LB52X_WDT_WP:
        value = s->protected ? SF32LB52X_WDT_WP_ST : 0;
        break;
    case SF32LB52X_WDT_FG:
        value = s->reset_flag ? SF32LB52X_WDT_FG_RST_FG : 0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        value = 0;
        break;
    }

    return value;
}

static void sf32lb52x_wdt_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned int size)
{
    SF32LB52XWdtState *s = opaque;

    if (s->protected &&
        (addr == SF32LB52X_WDT_CVR0 || addr == SF32LB52X_WDT_CVR1 ||
         addr == SF32LB52X_WDT_CR || addr == SF32LB52X_WDT_CCR)) {
        qemu_log_mask(LOG_UNIMP,
                      "%s: write to protected register 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case SF32LB52X_WDT_CVR0:
        s->cvr0 = value & SF32LB52X_WDT_CVR_MASK;
        break;
    case SF32LB52X_WDT_CVR1:
        s->cvr1 = value & SF32LB52X_WDT_CVR_MASK;
        break;
    case SF32LB52X_WDT_CR:
        s->cr = value;
        break;
    case SF32LB52X_WDT_CCR:
        s->ccr = value;
        if (value == SF32LB52X_WDT_CMD_START) {
            s->running = true;
            s->second_stage = false;
            timer_del(s->second_timer);
            /* A start/feed clears a pending mode-1 interrupt. */
            s->interrupt_asserted = 0;
            sf32lb52x_wdt_update_irq(s);
            sf32lb52x_wdt_reload(s, s->cvr0);
        } else if (value == SF32LB52X_WDT_CMD_STOP) {
            s->running = false;
            s->second_stage = false;
            timer_del(s->second_timer);
            sf32lb52x_wdt_stop_timer(s);
        }
        break;
    case SF32LB52X_WDT_ICR:
        if (value & 1U) {
            s->interrupt_asserted = 0;
            sf32lb52x_wdt_update_irq(s);
        }
        break;
    case SF32LB52X_WDT_WP:
        if (value == SF32LB52X_WDT_WP_SET) {
            s->protected = 1;
        } else if (value == SF32LB52X_WDT_WP_RELEASE) {
            s->protected = 0;
        }
        break;
    case SF32LB52X_WDT_FG:
        if (value & SF32LB52X_WDT_FG_RST_FG_CLR) {
            s->reset_flag = 0;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps sf32lb52x_wdt_ops = {
    .read = sf32lb52x_wdt_read,
    .write = sf32lb52x_wdt_write,
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

static void sf32lb52x_wdt_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XWdtState *s = SF32LB52X_WDT(obj);

    sf32lb52x_wdt_stop_timer(s);
    timer_del(s->second_timer);

    s->cvr0 = 0;
    s->cvr1 = 0;
    s->cr = 0;
    s->ccr = 0;
    s->interrupt_asserted = 0;
    s->protected = 0;
    s->running = false;
    s->second_stage = false;
    /* reset_flag is a reset cause and deliberately survives the reset. */
    sf32lb52x_wdt_update_irq(s);
}

static void sf32lb52x_wdt_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XWdtState *s = SF32LB52X_WDT(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sf32lb52x_wdt_ops, s,
                          TYPE_SF32LB52X_WDT, SF32LB52X_WDT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->timer = ptimer_init(sf32lb52x_wdt_expire, s,
                           PTIMER_POLICY_NO_IMMEDIATE_TRIGGER |
                           PTIMER_POLICY_NO_IMMEDIATE_RELOAD |
                           PTIMER_POLICY_NO_COUNTER_ROUND_DOWN);
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, SF32LB52X_WDT_CLK_HZ);
    ptimer_transaction_commit(s->timer);

    s->second_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   sf32lb52x_wdt_second_expire, s);
}

static const VMStateDescription vmstate_sf32lb52x_wdt = {
    .name = TYPE_SF32LB52X_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_PTIMER(timer, SF32LB52XWdtState),
        VMSTATE_TIMER_PTR(second_timer, SF32LB52XWdtState),
        VMSTATE_UINT32(cvr0, SF32LB52XWdtState),
        VMSTATE_UINT32(cvr1, SF32LB52XWdtState),
        VMSTATE_UINT32(cr, SF32LB52XWdtState),
        VMSTATE_UINT32(ccr, SF32LB52XWdtState),
        VMSTATE_UINT32(interrupt_asserted, SF32LB52XWdtState),
        VMSTATE_UINT32(protected, SF32LB52XWdtState),
        VMSTATE_UINT32(reset_flag, SF32LB52XWdtState),
        VMSTATE_BOOL(running, SF32LB52XWdtState),
        VMSTATE_BOOL(second_stage, SF32LB52XWdtState),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb52x_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_wdt_reset_hold;
    dc->realize = sf32lb52x_wdt_realize;
    dc->desc = "SiFli SF32LB52X watchdog";
    dc->vmsd = &vmstate_sf32lb52x_wdt;
    dc->user_creatable = false;
}

static const TypeInfo sf32lb52x_wdt_types[] = {
    {
        .name = TYPE_SF32LB52X_WDT,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XWdtState),
        .class_init = sf32lb52x_wdt_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_wdt_types);
