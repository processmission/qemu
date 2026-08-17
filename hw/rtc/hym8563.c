/*
 * Haoyu HYM8563 RTC model
 *
 * Register contract follows drivers/rtc/rtc-hym8563.c in the Rockchip
 * 6.1 kernel:
 *
 *   0x00 CTL1      (TEST/STOP/TESTC bits)
 *   0x01 CTL2      (TI_TP/AF/TF/AIE/TIE bits)
 *   0x02 SEC       (VL bit 7, BCD)
 *   0x03 MIN       (BCD)
 *   0x04 HOUR      (BCD)
 *   0x05 DAY       (BCD)
 *   0x06 WEEKDAY   (0 = Sunday)
 *   0x07 MONTH     (CENTURY bit 7, BCD)
 *   0x08 YEAR      (BCD, 2000-2099)
 *   0x09-0x0c alarm registers (bit 7 disables each compare)
 *   0x0d CLKOUT
 *   0x0e TMR_CTL / 0x0f TMR_CNT
 *
 * The open-drain INT output (active low) is exposed on the "irq" gpio
 * out; it asserts whenever an enabled alarm or timer flag is pending.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/rtc/hym8563.h"
#include "migration/vmstate.h"
#include "qemu/bcd.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "system/rtc.h"

#define HYM8563_CTL1        0x00
#define HYM8563_CTL1_STOP   BIT(5)
#define HYM8563_CTL2        0x01
#define HYM8563_CTL2_AF     BIT(3)
#define HYM8563_CTL2_TF     BIT(2)
#define HYM8563_CTL2_AIE    BIT(1)
#define HYM8563_CTL2_TIE    BIT(0)
#define HYM8563_SEC         0x02
#define HYM8563_SEC_VL      BIT(7)
#define HYM8563_MONTH       0x07
#define HYM8563_MONTH_CENT  BIT(7)
#define HYM8563_YEAR        0x08
#define HYM8563_NUM_REGS    0x10

struct Hym8563State {
    I2CSlave parent_obj;

    /* Seconds added to the qemu RTC time (see qemu_get_timedate()). */
    int64_t offset;
    uint8_t regs[HYM8563_NUM_REGS];
    uint8_t ptr;
    bool addr_byte;
    qemu_irq irq;
    QEMUTimer *tick_timer;
};

static void hym8563_update_irq(Hym8563State *s)
{
    bool pending = false;

    if (s->regs[HYM8563_CTL2] & HYM8563_CTL2_AF) {
        pending = s->regs[HYM8563_CTL2] & HYM8563_CTL2_AIE;
    }
    if (!pending && (s->regs[HYM8563_CTL2] & HYM8563_CTL2_TF)) {
        pending = s->regs[HYM8563_CTL2] & HYM8563_CTL2_TIE;
    }
    /* Open-drain output, active low. */
    qemu_set_irq(s->irq, !pending);
}

/*
 * One-second tick: expire the countdown timer and compare the alarm
 * registers with the current time.  Alarm fields whose disable bit (7)
 * is clear take part in the comparison.
 */
static void hym8563_tick(void *opaque)
{
    Hym8563State *s = opaque;
    uint8_t ctl2 = s->regs[HYM8563_CTL2];
    uint8_t *alarm = &s->regs[0x09];
    bool match = true;
    unsigned int i;

    if ((s->regs[0x0e] & BIT(7)) && s->regs[0x0f] > 0) {
        s->regs[0x0f]--;
        if (s->regs[0x0f] == 0) {
            ctl2 |= HYM8563_CTL2_TF;
        }
    }

    /*
     * Alarm registers 0x09..0x0c hold minute, hour, day and weekday;
     * each is compared against the matching live time register.
     */
    for (i = 0; i < 4; i++) {
        uint8_t mask = i == 0 ? 0x7f : 0x3f;

        if (!(alarm[i] & BIT(7)) &&
            (s->regs[HYM8563_SEC + 1 + i] & mask) != (alarm[i] & mask)) {
            match = false;
            break;
        }
    }
    if (match) {
        ctl2 |= HYM8563_CTL2_AF;
    }

    s->regs[HYM8563_CTL2] = ctl2;
    hym8563_update_irq(s);

    /* Keep the one-second cadence going. */
    timer_mod(s->tick_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND);
}

static void hym8563_capture_current_time(Hym8563State *s)
{
    struct tm now;

    qemu_get_timedate(&now, s->offset);
    s->regs[HYM8563_SEC] = to_bcd(now.tm_sec);
    s->regs[HYM8563_SEC + 1] = to_bcd(now.tm_min);
    s->regs[HYM8563_SEC + 2] = to_bcd(now.tm_hour);
    s->regs[HYM8563_SEC + 3] = to_bcd(now.tm_mday);
    s->regs[HYM8563_SEC + 4] = now.tm_wday;
    if (now.tm_year >= 200) {
        s->regs[HYM8563_MONTH] = HYM8563_MONTH_CENT | to_bcd(now.tm_mon + 1);
    } else {
        s->regs[HYM8563_MONTH] = to_bcd(now.tm_mon + 1);
    }
    s->regs[HYM8563_YEAR] = to_bcd(now.tm_year % 100);
}

/* Recompute offset so the current qemu time decodes to regs[0x02..0x08]. */
static void hym8563_set_time_from_regs(Hym8563State *s)
{
    struct tm tm = { 0 };

    tm.tm_sec = from_bcd(s->regs[HYM8563_SEC] & ~HYM8563_SEC_VL);
    tm.tm_min = from_bcd(s->regs[HYM8563_SEC + 1]);
    tm.tm_hour = from_bcd(s->regs[HYM8563_SEC + 2]);
    tm.tm_mday = from_bcd(s->regs[HYM8563_SEC + 3]);
    tm.tm_wday = s->regs[HYM8563_SEC + 4] & 0x07;
    tm.tm_mon = from_bcd(s->regs[HYM8563_MONTH] & 0x1f) - 1;
    tm.tm_year = from_bcd(s->regs[HYM8563_YEAR]);
    if (s->regs[HYM8563_MONTH] & HYM8563_MONTH_CENT) {
        tm.tm_year += 200;
    } else {
        tm.tm_year += 100;
    }
    s->offset = qemu_timedate_diff(&tm);
}

static int hym8563_event(I2CSlave *i2c, enum i2c_event event)
{
    Hym8563State *s = HYM8563(i2c);

    switch (event) {
    case I2C_START_RECV:
        hym8563_capture_current_time(s);
        break;
    case I2C_START_SEND:
        s->addr_byte = true;
        break;
    default:
        break;
    }

    return 0;
}

static int hym8563_send(I2CSlave *i2c, uint8_t data)
{
    Hym8563State *s = HYM8563(i2c);

    if (s->addr_byte) {
        /* First byte of a write transaction is the register address. */
        s->ptr = data & 0x0f;
        s->addr_byte = false;
        return 0;
    }

    s->regs[s->ptr] = data;
    if (s->ptr == HYM8563_CTL2 || s->ptr == 0x0e) {
        hym8563_update_irq(s);
    }
    if (s->ptr >= HYM8563_SEC && s->ptr <= HYM8563_YEAR) {
        hym8563_set_time_from_regs(s);
    }
    s->ptr = (s->ptr + 1) & 0x0f;
    return 0;
}

static uint8_t hym8563_recv(I2CSlave *i2c)
{
    Hym8563State *s = HYM8563(i2c);
    uint8_t res = s->regs[s->ptr & 0x0f];

    s->ptr = (s->ptr + 1) & 0x0f;
    return res;
}

static void hym8563_reset(DeviceState *dev)
{
    Hym8563State *s = HYM8563(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->offset = 0;
    s->ptr = 0;
    s->addr_byte = true;
    hym8563_capture_current_time(s);
    hym8563_update_irq(s);
    timer_mod(s->tick_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND);
}

static void hym8563_init(Object *obj)
{
    Hym8563State *s = HYM8563(obj);

    qdev_init_gpio_out_named(DEVICE(obj), &s->irq, "irq", 1);
    s->tick_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, hym8563_tick, s);
}

static const VMStateDescription vmstate_hym8563 = {
    .name = "hym8563",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Hym8563State),
        VMSTATE_INT64(offset, Hym8563State),
        VMSTATE_UINT8_ARRAY(regs, Hym8563State, HYM8563_NUM_REGS),
        VMSTATE_UINT8(ptr, Hym8563State),
        VMSTATE_TIMER_PTR_V(tick_timer, Hym8563State, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void hym8563_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    device_class_set_legacy_reset(dc, hym8563_reset);
    dc->vmsd = &vmstate_hym8563;
    sc->event = hym8563_event;
    sc->send = hym8563_send;
    sc->recv = hym8563_recv;
}

static const TypeInfo hym8563_info = {
    .name = TYPE_HYM8563,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Hym8563State),
    .instance_init = hym8563_init,
    .class_init = hym8563_class_init,
};

static void hym8563_register_types(void)
{
    type_register_static(&hym8563_info);
}

type_init(hym8563_register_types)
