/*
 * K230 Real-Time Clock
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/rtc/k230_rtc.h"
#include "migration/vmstate.h"
#include "qemu/bitops.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/rtc.h"
#include "system/system.h"

#define K230_RTC_MMIO_SIZE       0x400

#define K230_RTC_DATE            0x00
#define K230_RTC_TIME            0x04
#define K230_RTC_ALARM_DATE      0x08
#define K230_RTC_ALARM_TIME      0x0c
#define K230_RTC_COUNT           0x10
#define K230_RTC_INT_CTRL        0x14

#define K230_RTC_COUNT_CURR_MASK MAKE_64BIT_MASK(0, 15)
#define K230_RTC_COUNT_SUM_MASK  MAKE_64BIT_MASK(16, 15)
#define K230_RTC_COUNT_DEFAULT   (32767u << 16)

#define K230_RTC_INT_TIMER_W_EN  BIT(0)
#define K230_RTC_INT_TIMER_R_EN  BIT(1)
#define K230_RTC_INT_TICK_EN     BIT(8)
#define K230_RTC_INT_TICK_SEL    MAKE_64BIT_MASK(9, 4)
#define K230_RTC_INT_ALARM_EN    BIT(16)
#define K230_RTC_INT_ALARM_CLR   BIT(17)
#define K230_RTC_INT_SECOND_CMP  BIT(24)
#define K230_RTC_INT_MINUTE_CMP  BIT(25)
#define K230_RTC_INT_HOUR_CMP    BIT(26)
#define K230_RTC_INT_WEEK_CMP    BIT(27)
#define K230_RTC_INT_DAY_CMP     BIT(28)
#define K230_RTC_INT_MONTH_CMP   BIT(29)
#define K230_RTC_INT_YEAR_CMP    BIT(30)
#define K230_RTC_INT_CMP_MASK    MAKE_64BIT_MASK(24, 7)

static bool k230_rtc_year_is_leap(int year)
{
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int64_t k230_rtc_get_seconds(K230RtcState *s)
{
    return s->tick_offset +
           qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;
}

static void k230_rtc_set_seconds(K230RtcState *s, int64_t seconds)
{
    s->tick_offset = seconds -
        qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;
}

static void k230_rtc_get_time(K230RtcState *s, struct tm *tm)
{
    time_t seconds = k230_rtc_get_seconds(s);

    gmtime_r(&seconds, tm);
}

static uint32_t k230_rtc_pack_date(const struct tm *tm)
{
    int year = tm->tm_year + 1900;
    int year_l = year % 100;
    int year_h = year / 100;

    if (year_l == 0) {
        year_l = 100;
        year_h--;
    }

    return (tm->tm_mday & 0x1f) |
           ((tm->tm_mon + 1) & 0xf) << 8 |
           (year_l & 0x7f) << 16 |
           (k230_rtc_year_is_leap(year) ? BIT(23) : 0) |
           (year_h & 0x7f) << 24;
}

static uint32_t k230_rtc_pack_time(const struct tm *tm)
{
    return (tm->tm_sec & 0x3f) |
           (tm->tm_min & 0x3f) << 8 |
           (tm->tm_hour & 0x1f) << 16 |
           (tm->tm_wday & 0x7) << 24;
}

static bool k230_rtc_decode_date(uint32_t value, struct tm *tm)
{
    int day = extract32(value, 0, 5);
    int month = extract32(value, 8, 4);
    int year_l = extract32(value, 16, 7);
    int year_h = extract32(value, 24, 7);

    if (day < 1 || day > 31 || month < 1 || month > 12) {
        return false;
    }

    tm->tm_mday = day;
    tm->tm_mon = month - 1;
    tm->tm_year = year_h * 100 + year_l - 1900;
    return true;
}

static bool k230_rtc_decode_time(uint32_t value, struct tm *tm)
{
    int second = extract32(value, 0, 6);
    int minute = extract32(value, 8, 6);
    int hour = extract32(value, 16, 5);
    int week = extract32(value, 24, 3);

    if (second > 59 || minute > 59 || hour > 23 || week > 6) {
        return false;
    }

    tm->tm_sec = second;
    tm->tm_min = minute;
    tm->tm_hour = hour;
    tm->tm_wday = week;
    return true;
}

static uint32_t k230_rtc_get_count(K230RtcState *s)
{
    uint32_t sum = (s->count & K230_RTC_COUNT_SUM_MASK) >> 16;
    int64_t elapsed_ns = qemu_clock_get_ns(rtc_clock) - s->count_base_ns;
    uint64_t ticks;

    if (elapsed_ns < 0) {
        elapsed_ns = 0;
    }

    ticks = muldiv64(elapsed_ns, sum + 1, NANOSECONDS_PER_SECOND);
    return (s->count & K230_RTC_COUNT_SUM_MASK) | (ticks % (sum + 1));
}

static void k230_rtc_set_count(K230RtcState *s, uint32_t value)
{
    uint32_t sum = (value & K230_RTC_COUNT_SUM_MASK) >> 16;
    uint32_t curr = value & K230_RTC_COUNT_CURR_MASK;
    int64_t elapsed_ns;

    curr %= sum + 1;
    elapsed_ns = muldiv64(curr, NANOSECONDS_PER_SECOND, sum + 1);

    s->count = value & K230_RTC_COUNT_SUM_MASK;
    s->count_base_ns = qemu_clock_get_ns(rtc_clock) - elapsed_ns;
}

static void k230_rtc_update_irq(K230RtcState *s)
{
    bool enabled = s->int_ctrl & (K230_RTC_INT_ALARM_EN |
                                  K230_RTC_INT_TICK_EN);

    qemu_set_irq(s->irq, s->irq_pending && enabled);
}

static bool k230_rtc_alarm_matches(K230RtcState *s, const struct tm *tm)
{
    uint32_t cmp = s->int_ctrl & K230_RTC_INT_CMP_MASK;
    uint32_t date = s->alarm_date;
    uint32_t time = s->alarm_time;
    int alarm_year = extract32(date, 24, 7) * 100 +
                     extract32(date, 16, 7);

    if ((cmp & K230_RTC_INT_YEAR_CMP) &&
        alarm_year != tm->tm_year + 1900) {
        return false;
    }
    if ((cmp & K230_RTC_INT_MONTH_CMP) &&
        extract32(date, 8, 4) != tm->tm_mon + 1) {
        return false;
    }
    if ((cmp & K230_RTC_INT_DAY_CMP) &&
        extract32(date, 0, 5) != tm->tm_mday) {
        return false;
    }
    if ((cmp & K230_RTC_INT_WEEK_CMP) &&
        extract32(time, 24, 3) != tm->tm_wday) {
        return false;
    }
    if ((cmp & K230_RTC_INT_HOUR_CMP) &&
        extract32(time, 16, 5) != tm->tm_hour) {
        return false;
    }
    if ((cmp & K230_RTC_INT_MINUTE_CMP) &&
        extract32(time, 8, 6) != tm->tm_min) {
        return false;
    }
    if ((cmp & K230_RTC_INT_SECOND_CMP) &&
        extract32(time, 0, 6) != tm->tm_sec) {
        return false;
    }

    return cmp != 0;
}

static int64_t k230_rtc_next_alarm_ns(K230RtcState *s)
{
    int64_t now = k230_rtc_get_seconds(s);
    uint32_t cmp = s->int_ctrl & K230_RTC_INT_CMP_MASK;

    if (!(s->int_ctrl & K230_RTC_INT_ALARM_EN) || !cmp) {
        return -1;
    }

    if ((cmp & (K230_RTC_INT_YEAR_CMP | K230_RTC_INT_MONTH_CMP |
                K230_RTC_INT_DAY_CMP | K230_RTC_INT_HOUR_CMP |
                K230_RTC_INT_MINUTE_CMP | K230_RTC_INT_SECOND_CMP)) ==
        (K230_RTC_INT_YEAR_CMP | K230_RTC_INT_MONTH_CMP |
         K230_RTC_INT_DAY_CMP | K230_RTC_INT_HOUR_CMP |
         K230_RTC_INT_MINUTE_CMP | K230_RTC_INT_SECOND_CMP)) {
        struct tm tm = { 0 };
        int64_t alarm;

        if (k230_rtc_decode_date(s->alarm_date, &tm) &&
            k230_rtc_decode_time(s->alarm_time, &tm)) {
            alarm = mktimegm(&tm);
            if (alarm > now) {
                return (alarm - s->tick_offset) * NANOSECONDS_PER_SECOND;
            }
        }
        return -1;
    }

    for (int i = 1; i <= 86400; i++) {
        struct tm tm;
        time_t seconds = now + i;

        gmtime_r(&seconds, &tm);
        if (k230_rtc_alarm_matches(s, &tm)) {
            return (seconds - s->tick_offset) * NANOSECONDS_PER_SECOND;
        }
    }

    return -1;
}

static int64_t k230_rtc_tick_period_ns(K230RtcState *s)
{
    uint32_t tick_sel = (s->int_ctrl & K230_RTC_INT_TICK_SEL) >> 9;

    if (!(s->int_ctrl & K230_RTC_INT_TICK_EN)) {
        return -1;
    }

    switch (tick_sel) {
    case 0:
        return NANOSECONDS_PER_SECOND / 64;
    case 1:
        return NANOSECONDS_PER_SECOND / 8;
    case 2:
        return NANOSECONDS_PER_SECOND;
    case 3:
        return 60 * NANOSECONDS_PER_SECOND;
    case 4:
        return 60 * 60 * NANOSECONDS_PER_SECOND;
    case 5:
        return 7 * 24 * 60 * 60 * NANOSECONDS_PER_SECOND;
    case 6:
        return 24 * 60 * 60 * NANOSECONDS_PER_SECOND;
    case 7:
        return 30 * 24 * 60 * 60 * NANOSECONDS_PER_SECOND;
    case 8:
        return 365 * 24 * 60 * 60 * NANOSECONDS_PER_SECOND;
    default:
        return -1;
    }
}

static void k230_rtc_schedule(K230RtcState *s)
{
    int64_t now_ns = qemu_clock_get_ns(rtc_clock);
    int64_t alarm_ns = k230_rtc_next_alarm_ns(s);
    int64_t tick_period_ns = k230_rtc_tick_period_ns(s);
    int64_t next_ns = -1;

    if (alarm_ns >= 0) {
        next_ns = alarm_ns;
    }
    if (tick_period_ns > 0 &&
        (next_ns < 0 || now_ns + tick_period_ns < next_ns)) {
        next_ns = now_ns + tick_period_ns;
    }

    if (next_ns >= 0) {
        timer_mod(s->timer, next_ns);
    } else {
        timer_del(s->timer);
    }
}

static void k230_rtc_timer_cb(void *opaque)
{
    K230RtcState *s = K230_RTC(opaque);

    s->irq_pending = true;
    k230_rtc_update_irq(s);
    k230_rtc_schedule(s);
}

static uint64_t k230_rtc_read(void *opaque, hwaddr addr, unsigned int size)
{
    K230RtcState *s = K230_RTC(opaque);
    struct tm tm;

    switch (addr) {
    case K230_RTC_DATE:
        k230_rtc_get_time(s, &tm);
        return k230_rtc_pack_date(&tm);
    case K230_RTC_TIME:
        k230_rtc_get_time(s, &tm);
        return k230_rtc_pack_time(&tm);
    case K230_RTC_ALARM_DATE:
        return s->alarm_date;
    case K230_RTC_ALARM_TIME:
        return s->alarm_time;
    case K230_RTC_COUNT:
        return k230_rtc_get_count(s);
    case K230_RTC_INT_CTRL:
        return s->int_ctrl;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad offset 0x%x\n", __func__, (uint32_t)addr);
        return 0;
    }
}

static void k230_rtc_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned int size)
{
    K230RtcState *s = K230_RTC(opaque);
    struct tm tm;

    switch (addr) {
    case K230_RTC_DATE:
        k230_rtc_get_time(s, &tm);
        if (k230_rtc_decode_date(value, &tm)) {
            k230_rtc_set_seconds(s, mktimegm(&tm));
        }
        k230_rtc_schedule(s);
        break;
    case K230_RTC_TIME:
        k230_rtc_get_time(s, &tm);
        if (k230_rtc_decode_time(value, &tm)) {
            k230_rtc_set_seconds(s, mktimegm(&tm));
        }
        k230_rtc_schedule(s);
        break;
    case K230_RTC_ALARM_DATE:
        s->alarm_date = value;
        k230_rtc_schedule(s);
        break;
    case K230_RTC_ALARM_TIME:
        s->alarm_time = value;
        k230_rtc_schedule(s);
        break;
    case K230_RTC_COUNT:
        k230_rtc_set_count(s, value);
        break;
    case K230_RTC_INT_CTRL:
        if (value & K230_RTC_INT_ALARM_CLR) {
            s->irq_pending = false;
        }
        s->int_ctrl = value & ~K230_RTC_INT_ALARM_CLR;
        k230_rtc_update_irq(s);
        k230_rtc_schedule(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad offset 0x%x\n", __func__, (uint32_t)addr);
        break;
    }
}

static const MemoryRegionOps k230_rtc_ops = {
    .read = k230_rtc_read,
    .write = k230_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void k230_rtc_reset(DeviceState *dev)
{
    K230RtcState *s = K230_RTC(dev);

    s->alarm_date = 0;
    s->alarm_time = 0;
    k230_rtc_set_count(s, K230_RTC_COUNT_DEFAULT);
    s->int_ctrl = 0;
    s->irq_pending = false;
    k230_rtc_update_irq(s);
    k230_rtc_schedule(s);
}

static int k230_rtc_post_load(void *opaque, int version_id)
{
    K230RtcState *s = K230_RTC(opaque);

    k230_rtc_update_irq(s);
    k230_rtc_schedule(s);
    return 0;
}

static const VMStateDescription vmstate_k230_rtc = {
    .name = TYPE_K230_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = k230_rtc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(tick_offset, K230RtcState),
        VMSTATE_INT64(count_base_ns, K230RtcState),
        VMSTATE_UINT32(alarm_date, K230RtcState),
        VMSTATE_UINT32(alarm_time, K230RtcState),
        VMSTATE_UINT32(count, K230RtcState),
        VMSTATE_UINT32(int_ctrl, K230RtcState),
        VMSTATE_BOOL(irq_pending, K230RtcState),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_rtc_init(Object *obj)
{
    K230RtcState *s = K230_RTC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    struct tm tm;

    memory_region_init_io(&s->mmio, obj, &k230_rtc_ops, s,
                          TYPE_K230_RTC, K230_RTC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    qemu_get_timedate(&tm, 0);
    s->tick_offset = mktimegm(&tm) -
        qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;
    s->timer = timer_new_ns(rtc_clock, k230_rtc_timer_cb, s);
}

static void k230_rtc_finalize(Object *obj)
{
    K230RtcState *s = K230_RTC(obj);

    timer_free(s->timer);
}

static void k230_rtc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, k230_rtc_reset);
    dc->vmsd = &vmstate_k230_rtc;
    dc->desc = "K230 real-time clock";
}

static const TypeInfo k230_rtc_type_info = {
    .name = TYPE_K230_RTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230RtcState),
    .instance_init = k230_rtc_init,
    .instance_finalize = k230_rtc_finalize,
    .class_init = k230_rtc_class_init,
};

static void k230_rtc_register_types(void)
{
    type_register_static(&k230_rtc_type_info);
}

type_init(k230_rtc_register_types)
