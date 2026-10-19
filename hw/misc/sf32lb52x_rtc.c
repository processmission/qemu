/*
 * SiFli SF32LB52X RTC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register layout follows the vendor header rtc.h (RTC_TypeDef); behavior
 * follows the SiFli HAL/LL sources and the Zephyr rtc_sf32lb driver.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sf32lb52x_rtc.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/memory.h"

/* Register offsets (rtc.h:10-42). */
#define RTC_TR      0x00
#define RTC_DR      0x04
#define RTC_CR      0x08
#define RTC_ISR     0x0c
#define RTC_PSCLR   0x10
#define RTC_WUTR    0x14
#define RTC_ALRMTR  0x18
#define RTC_ALRMDR  0x1c
#define RTC_SHIFTR  0x20
#define RTC_TSTR    0x24
#define RTC_TSDR    0x28
#define RTC_OR      0x2c
#define RTC_BKP0R   0x30
#define RTC_PBRCR   0x58
#define RTC_PBR0R   0x5c
#define RTC_PAWK1R  0x6c

/* TR and ALRMTR shared BCD field layout (rtc.h:46-69, 211-234). */
#define RTC_SS_Pos    0
#define RTC_SS_Msk    0x3ffU
#define RTC_SEC_Pos   11
#define RTC_SEC_Msk   0x7fU
#define RTC_MIN_Pos   18
#define RTC_MIN_Msk   0x7fU
#define RTC_HOUR_Pos  25
#define RTC_HOUR_Msk  0x3fU
#define RTC_PM_Pos    31
#define RTC_PM_Msk    1U
#define RTC_TR_WRITE_Msk                                                       \
    (RTC_SS_Msk | (RTC_SEC_Msk << RTC_SEC_Pos) | (RTC_MIN_Msk << RTC_MIN_Pos) |\
     (RTC_HOUR_Msk << RTC_HOUR_Pos) | (RTC_PM_Msk << RTC_PM_Pos))

/* DR BCD field layout (rtc.h:72-98). */
#define RTC_DR_DAY_Pos    0
#define RTC_DR_DAY_Msk    0x3fU
#define RTC_DR_MONTH_Pos  8
#define RTC_DR_MONTH_Msk  0x1fU
#define RTC_DR_WD_Pos     13
#define RTC_DR_WD_Msk     0x7U
#define RTC_DR_YEAR_Pos   16
#define RTC_DR_YEAR_Msk   0xffU
#define RTC_DR_CB_Pos     24
#define RTC_DR_CB_Msk     1U
#define RTC_DR_ERR_Pos    31
#define RTC_DR_ERR_Msk    1U
#define RTC_DR_WRITE_Msk                                                       \
    ((RTC_DR_DAY_Msk << RTC_DR_DAY_Pos) |                                      \
     (RTC_DR_MONTH_Msk << RTC_DR_MONTH_Pos) |                                  \
     (RTC_DR_WD_Msk << RTC_DR_WD_Pos) |                                        \
     (RTC_DR_YEAR_Msk << RTC_DR_YEAR_Pos) |                                    \
     (RTC_DR_CB_Msk << RTC_DR_CB_Pos) | (RTC_DR_ERR_Msk << RTC_DR_ERR_Pos))

/* CR bits (rtc.h:101-157). */
#define RTC_CR_LPCKSEL     (1U << 0)
#define RTC_CR_WUCKSEL     (1U << 1)
#define RTC_CR_TSEDGE      (1U << 3)
#define RTC_CR_REFCKON     (1U << 4)
#define RTC_CR_BYPSHAD     (1U << 5)
#define RTC_CR_FMT         (1U << 6)
#define RTC_CR_ALRME       (1U << 8)
#define RTC_CR_WUTE        (1U << 9)
#define RTC_CR_TSE         (1U << 10)
#define RTC_CR_ALRMIE      (1U << 11)
#define RTC_CR_WUTIE       (1U << 12)
#define RTC_CR_TSIE        (1U << 13)
#define RTC_CR_ADD1H       (1U << 14)
#define RTC_CR_SUB1H       (1U << 15)
#define RTC_CR_BKP         (1U << 16)
#define RTC_CR_COSEL       (1U << 17)
#define RTC_CR_POL         (1U << 18)
#define RTC_CR_OSEL        (3U << 19)
#define RTC_CR_COE         (1U << 21)
#define RTC_CR_WRITE_Msk                                                       \
    (RTC_CR_LPCKSEL | RTC_CR_WUCKSEL | RTC_CR_TSEDGE | RTC_CR_REFCKON |        \
     RTC_CR_BYPSHAD | RTC_CR_FMT | RTC_CR_ALRME | RTC_CR_WUTE | RTC_CR_TSE |  \
     RTC_CR_ALRMIE | RTC_CR_WUTIE | RTC_CR_TSIE | RTC_CR_BKP | RTC_CR_COSEL | \
     RTC_CR_POL | RTC_CR_OSEL | RTC_CR_COE)

/*
 * ISR bits (rtc.h:160-192). ALRMF/WUTF/TSF/TSOVF are rw0c
 * (ll_rtc.h:294-360).
 */
#define RTC_ISR_ALRMWF     (1U << 0)
#define RTC_ISR_ALRMF      (1U << 1)
#define RTC_ISR_WUTWF      (1U << 2)
#define RTC_ISR_WUTF       (1U << 3)
#define RTC_ISR_TSF        (1U << 4)
#define RTC_ISR_TSOVF      (1U << 5)
#define RTC_ISR_SHPF       (1U << 6)
#define RTC_ISR_RSF        (1U << 7)
#define RTC_ISR_INITS      (1U << 8)
#define RTC_ISR_INITF      (1U << 9)
#define RTC_ISR_INIT       (1U << 10)
#define RTC_ISR_FLAG_Msk   (RTC_ISR_ALRMF | RTC_ISR_WUTF | RTC_ISR_TSF | \
                            RTC_ISR_TSOVF)

/* PSCLR fields (rtc.h:195-203). */
#define RTC_PSCLR_DIVB_Msk      0x3ffU
#define RTC_PSCLR_DIVA_FRAC_Msk (0x3fffU << 10)
#define RTC_PSCLR_DIVA_INT_Msk  (0xffU << 24)
#define RTC_PSCLR_WRITE_Msk                                                    \
    (RTC_PSCLR_DIVB_Msk | RTC_PSCLR_DIVA_FRAC_Msk | RTC_PSCLR_DIVA_INT_Msk)

/* ALRMDR field/mask bits (rtc.h:237-272). Mask bit 1 = field ignored. */
#define RTC_ALRMDR_DAY_Msk    0x3fU
#define RTC_ALRMDR_MONTH_Msk  0x1fU
#define RTC_ALRMDR_WD_Msk     0x7U
#define RTC_ALRMDR_MSKSS_Msk  (0xfU << 20)
#define RTC_ALRMDR_MSKS       (1U << 24)
#define RTC_ALRMDR_MSKMN      (1U << 25)
#define RTC_ALRMDR_MSKH       (1U << 26)
#define RTC_ALRMDR_MSKD       (1U << 27)
#define RTC_ALRMDR_MSKM       (1U << 28)
#define RTC_ALRMDR_MSKWD      (1U << 29)
#define RTC_ALRMDR_WRITE_Msk                                                   \
    (RTC_ALRMDR_DAY_Msk | (RTC_ALRMDR_MONTH_Msk << 8) |                        \
     (RTC_ALRMDR_WD_Msk << 13) | RTC_ALRMDR_MSKSS_Msk | RTC_ALRMDR_MSKS |      \
     RTC_ALRMDR_MSKMN | RTC_ALRMDR_MSKH | RTC_ALRMDR_MSKD |                    \
     RTC_ALRMDR_MSKM | RTC_ALRMDR_MSKWD)

/* ALRMTR uses the same BCD time layout as TR. */
#define RTC_ALRMTR_WRITE_Msk                                                   \
    (RTC_SS_Msk | (RTC_SEC_Msk << RTC_SEC_Pos) | (RTC_MIN_Msk << RTC_MIN_Pos) |\
     (RTC_HOUR_Msk << RTC_HOUR_Pos) | (RTC_PM_Msk << RTC_PM_Pos))

static const uint8_t rtc_days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
};

static uint32_t rtc_field_get(uint32_t reg, unsigned pos, uint32_t msk)
{
    return (reg & (msk << pos)) >> pos;
}

static unsigned rtc_bcd_get(uint32_t reg, unsigned pos, uint32_t msk)
{
    uint32_t field = (reg >> pos) & msk;

    return ((field >> 4) * 10U) + (field & 0xfU);
}

static void rtc_bcd_set(uint32_t *reg, unsigned pos, uint32_t msk,
                        unsigned value)
{
    uint32_t field = ((value / 10U) << 4) | (value % 10U);

    *reg = (*reg & ~(msk << pos)) | ((field & msk) << pos);
}

static unsigned rtc_leap_year(unsigned year)
{
    /*
     * The calendar range is 1970-2099 (vendor doc rtc.md:11-13): years are
     * stored as two BCD digits, so assume 20xx for the leap rule.
     */
    return (year % 4U) == 0U;
}

static void rtc_update_irq(SF32LB52XRtcState *s)
{
    qemu_set_irq(s->irq, (s->isr_flags & RTC_ISR_ALRMF) &&
                         (s->cr & RTC_CR_ALRMIE));
}

/*
 * The HPSYS_CFG block exposes RTC_TR/RTC_DR shadow copies that the Zephyr
 * driver reads back through the SYS_CFG phandle (vendor HAL rtc.h).
 * Publish the current calendar after every
 * guest-visible change so get_time() sees fresh values.
 */
#define RTC_CFG_TR_OFFSET 0x14
#define RTC_CFG_DR_OFFSET 0x18

static void rtc_update_cfg_shadow(SF32LB52XRtcState *s)
{
    if (!s->cfg) {
        return;
    }

    memory_region_dispatch_write(s->cfg, RTC_CFG_TR_OFFSET, s->tr,
                                 size_memop(4), MEMTXATTRS_UNSPECIFIED);
    memory_region_dispatch_write(s->cfg, RTC_CFG_DR_OFFSET, s->dr,
                                 size_memop(4), MEMTXATTRS_UNSPECIFIED);
}

static void rtc_tick_restart(SF32LB52XRtcState *s)
{
    ptimer_transaction_begin(s->tick_timer);
    ptimer_stop(s->tick_timer);
    ptimer_set_limit(s->tick_timer, 1, 1);
    ptimer_run(s->tick_timer, 0);
    ptimer_transaction_commit(s->tick_timer);
}

static void rtc_calendar_advance(SF32LB52XRtcState *s)
{
    unsigned sec = rtc_bcd_get(s->tr, RTC_SEC_Pos, RTC_SEC_Msk);
    unsigned min = rtc_bcd_get(s->tr, RTC_MIN_Pos, RTC_MIN_Msk);
    unsigned hour = rtc_bcd_get(s->tr, RTC_HOUR_Pos, RTC_HOUR_Msk);
    unsigned day = rtc_bcd_get(s->dr, RTC_DR_DAY_Pos, RTC_DR_DAY_Msk);
    unsigned month = rtc_bcd_get(s->dr, RTC_DR_MONTH_Pos, RTC_DR_MONTH_Msk);
    unsigned wd = rtc_bcd_get(s->dr, RTC_DR_WD_Pos, RTC_DR_WD_Msk);
    unsigned year = rtc_bcd_get(s->dr, RTC_DR_YEAR_Pos, RTC_DR_YEAR_Msk);

    /* Keep out-of-range guest values from indexing outside the month table. */
    month = (month < 1U || month > 12U) ? 1U : month;
    wd = (wd < 1U || wd > 7U) ? 1U : wd;
    day = (day < 1U) ? 1U : day;
    hour %= 24U;
    min %= 60U;
    sec %= 60U;

    sec++;
    if (sec >= 60U) {
        unsigned dim;

        sec = 0;
        min++;
        if (min >= 60U) {
            min = 0;
            hour++;
            if (hour >= 24U) {
                hour = 0;
                day++;
                wd = (wd % 7U) + 1U;
                dim = rtc_days_in_month[month - 1U] +
                      ((month == 2U) ? rtc_leap_year(year) : 0U);
                if (day > dim) {
                    day = 1;
                    month++;
                    if (month > 12U) {
                        month = 1;
                        year = (year + 1U) % 100U;
                    }
                }
            }
        }
    }

    rtc_bcd_set(&s->tr, RTC_SS_Pos, RTC_SS_Msk, 0);
    rtc_bcd_set(&s->tr, RTC_SEC_Pos, RTC_SEC_Msk, sec);
    rtc_bcd_set(&s->tr, RTC_MIN_Pos, RTC_MIN_Msk, min);
    rtc_bcd_set(&s->tr, RTC_HOUR_Pos, RTC_HOUR_Msk, hour);
    rtc_bcd_set(&s->dr, RTC_DR_DAY_Pos, RTC_DR_DAY_Msk, day);
    rtc_bcd_set(&s->dr, RTC_DR_MONTH_Pos, RTC_DR_MONTH_Msk, month);
    rtc_bcd_set(&s->dr, RTC_DR_WD_Pos, RTC_DR_WD_Msk, wd);
    rtc_bcd_set(&s->dr, RTC_DR_YEAR_Pos, RTC_DR_YEAR_Msk, year);

    rtc_update_cfg_shadow(s);
}

static void rtc_alarm_hour_shift(SF32LB52XRtcState *s, int delta)
{
    unsigned hour = rtc_bcd_get(s->tr, RTC_HOUR_Pos, RTC_HOUR_Msk);

    hour = (hour + 24U + delta) % 24U;
    rtc_bcd_set(&s->tr, RTC_HOUR_Pos, RTC_HOUR_Msk, hour);

    rtc_update_cfg_shadow(s);
}

static bool rtc_alarm_match(SF32LB52XRtcState *s)
{
    uint32_t adr = s->alrmdr;
    uint32_t now_sec = rtc_field_get(s->tr, RTC_SEC_Pos, RTC_SEC_Msk);
    uint32_t now_min = rtc_field_get(s->tr, RTC_MIN_Pos, RTC_MIN_Msk);
    uint32_t now_hour = rtc_field_get(s->tr, RTC_HOUR_Pos, RTC_HOUR_Msk);
    uint32_t now_day = rtc_field_get(s->dr, RTC_DR_DAY_Pos, RTC_DR_DAY_Msk);
    uint32_t now_month = rtc_field_get(s->dr, RTC_DR_MONTH_Pos,
                                       RTC_DR_MONTH_Msk);
    uint32_t now_wd = rtc_field_get(s->dr, RTC_DR_WD_Pos, RTC_DR_WD_Msk);
    uint32_t alr_sec = rtc_field_get(s->alrmtr, RTC_SEC_Pos, RTC_SEC_Msk);
    uint32_t alr_min = rtc_field_get(s->alrmtr, RTC_MIN_Pos, RTC_MIN_Msk);
    uint32_t alr_hour = rtc_field_get(s->alrmtr, RTC_HOUR_Pos,
                                      RTC_HOUR_Msk);
    uint32_t alr_day = rtc_field_get(adr, 0, RTC_ALRMDR_DAY_Msk);
    uint32_t alr_month = rtc_field_get(adr, 8, RTC_ALRMDR_MONTH_Msk);
    uint32_t alr_wd = rtc_field_get(adr, 13, RTC_ALRMDR_WD_Msk);

    if (!(adr & RTC_ALRMDR_MSKS) && (now_sec != alr_sec)) {
        return false;
    }
    if (!(adr & RTC_ALRMDR_MSKMN) && (now_min != alr_min)) {
        return false;
    }
    if (!(adr & RTC_ALRMDR_MSKH) && (now_hour != alr_hour)) {
        return false;
    }
    if (!(adr & RTC_ALRMDR_MSKD) && (now_day != alr_day)) {
        return false;
    }
    if (!(adr & RTC_ALRMDR_MSKM) && (now_month != alr_month)) {
        return false;
    }
    if (!(adr & RTC_ALRMDR_MSKWD) && (now_wd != alr_wd)) {
        return false;
    }

    return true;
}

static void rtc_tick(void *opaque)
{
    SF32LB52XRtcState *s = opaque;

    if (s->init_mode) {
        /* The calendar is frozen while the firmware updates it. */
        return;
    }

    rtc_calendar_advance(s);
    s->rsf = true;

    if ((s->cr & RTC_CR_ALRME) && rtc_alarm_match(s)) {
        s->isr_flags |= RTC_ISR_ALRMF;
        rtc_update_irq(s);
    }
}

static void rtc_sync_expire(void *opaque)
{
    SF32LB52XRtcState *s = opaque;

    /*
     * Shadow-register copy completed: RSF reads as set again
     * (ll_rtc.h:298-307).
     */
    s->rsf = true;
}

static uint32_t rtc_isr_value(SF32LB52XRtcState *s)
{
    uint32_t value = s->isr_flags & RTC_ISR_FLAG_Msk;

    value |= RTC_ISR_ALRMWF;
    if (!(s->cr & RTC_CR_WUTE)) {
        value |= RTC_ISR_WUTWF;
    }
    if (s->rsf) {
        value |= RTC_ISR_RSF;
    }
    if (s->inits) {
        value |= RTC_ISR_INITS;
    }
    if (s->init_mode) {
        value |= RTC_ISR_INITF | RTC_ISR_INIT;
    }

    return value;
}

static bool rtc_addr_is_unimplemented(hwaddr addr)
{
    switch (addr) {
    case RTC_WUTR:
    case RTC_SHIFTR:
    case RTC_TSTR:
    case RTC_TSDR:
    case RTC_OR:
    case RTC_PBRCR:
    case RTC_PBR0R:
    case RTC_PBR0R + 0x04:
    case RTC_PBR0R + 0x08:
    case RTC_PBR0R + 0x0c:
    case RTC_PAWK1R:
    case RTC_PAWK1R + 0x04:
    case RTC_PAWK1R + 0x08:
        return true;
    default:
        return false;
    }
}

static uint64_t sf32lb52x_rtc_read(void *opaque, hwaddr addr, unsigned int size)
{
    SF32LB52XRtcState *s = opaque;
    hwaddr bkp_offset = addr - RTC_BKP0R;

    if (addr >= SF32LB52X_RTC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    if (bkp_offset < SF32LB52X_RTC_NUM_BKP * 4U && (bkp_offset % 4U) == 0U) {
        return s->bkp[bkp_offset / 4U];
    }

    if (rtc_addr_is_unimplemented(addr)) {
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented register 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }

    switch (addr) {
    case RTC_TR:
        return s->tr;
    case RTC_DR:
        return s->dr;
    case RTC_CR:
        return s->cr;
    case RTC_ISR:
        return rtc_isr_value(s);
    case RTC_PSCLR:
        return s->psc;
    case RTC_ALRMTR:
        return s->alrmtr;
    case RTC_ALRMDR:
        return s->alrmdr;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void sf32lb52x_rtc_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned int size)
{
    SF32LB52XRtcState *s = opaque;
    hwaddr bkp_offset = addr - RTC_BKP0R;
    uint32_t val = value;

    if (addr >= SF32LB52X_RTC_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    if (bkp_offset < SF32LB52X_RTC_NUM_BKP * 4U && (bkp_offset % 4U) == 0U) {
        s->bkp[bkp_offset / 4U] = val;
        return;
    }

    if (rtc_addr_is_unimplemented(addr)) {
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented register 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return;
    }

    switch (addr) {
    case RTC_TR:
    case RTC_DR:
        if (!s->init_mode) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: TR/DR written outside init mode\n", __func__);
            return;
        }
        if (addr == RTC_TR) {
            s->tr = val & RTC_TR_WRITE_Msk;
        } else {
            s->dr = val & RTC_DR_WRITE_Msk;
        }
        rtc_update_cfg_shadow(s);
        break;
    case RTC_CR: {
        uint32_t new_cr = val & RTC_CR_WRITE_Msk;
        bool was_wute = (s->cr & RTC_CR_WUTE) != 0;

        /* ADD1H/SUB1H are write-only calendar adjustments. */
        if (val & RTC_CR_ADD1H) {
            rtc_alarm_hour_shift(s, 1);
        }
        if (val & RTC_CR_SUB1H) {
            rtc_alarm_hour_shift(s, -1);
        }
        new_cr &= ~(RTC_CR_ADD1H | RTC_CR_SUB1H);
        s->cr = new_cr;
        if ((new_cr & RTC_CR_WUTE) && !was_wute) {
            qemu_log_mask(LOG_UNIMP, "%s: wakeup timer is not modeled\n",
                          __func__);
        }
        rtc_update_irq(s);
        break;
    }
    case RTC_ISR: {
        bool new_init = (val & RTC_ISR_INIT) != 0;

        /* ALRMF/WUTF/TSF/TSOVF are rw0c: writing 0 clears the flag. */
        s->isr_flags &= val;
        if (!(val & RTC_ISR_RSF)) {
            if (s->rsf) {
                s->rsf = false;
                timer_mod(s->sync_timer,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          SF32LB52X_RTC_SYNC_DELAY_NS);
            }
        } else {
            s->rsf = true;
        }
        if (new_init != s->init_mode) {
            s->init_mode = new_init;
            if (new_init) {
                s->inits = true; /* sticky initialization status */
            } else {
                rtc_tick_restart(s);
            }
        }
        rtc_update_irq(s);
        break;
    }
    case RTC_PSCLR:
        s->psc = val & RTC_PSCLR_WRITE_Msk;
        break;
    case RTC_ALRMTR:
        s->alrmtr = val & RTC_ALRMTR_WRITE_Msk;
        break;
    case RTC_ALRMDR:
        s->alrmdr = val & RTC_ALRMDR_WRITE_Msk;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps sf32lb52x_rtc_ops = {
    .read = sf32lb52x_rtc_read,
    .write = sf32lb52x_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sf32lb52x_rtc_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XRtcState *s = SF32LB52X_RTC(obj);

    timer_del(s->sync_timer);
    rtc_tick_restart(s);

    s->tr = 0;
    /*
     * Vendor HAL_RTC_DeInit hints DR=0x2101, but that is a de-initialized
     * value and no TRM/SVD documents a different power-on value, so keep
     * the documented plausible reset value 0.
     */
    s->dr = 0;
    s->cr = 0;
    s->psc = 0;
    s->alrmtr = 0;
    s->alrmdr = 0;
    s->isr_flags = 0;
    s->init_mode = false;
    s->inits = false;
    s->rsf = true;
    rtc_update_cfg_shadow(s);

    /*
     * BKP0R..BKP9R are battery-domain registers: a system reset does not
     * clear them (vendor doc rtc.md:7-11). They start zeroed on power-on.
     */
    qemu_set_irq(s->irq, 0);
}

static void sf32lb52x_rtc_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XRtcState *s = SF32LB52X_RTC(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sf32lb52x_rtc_ops, s,
                          TYPE_SF32LB52X_RTC, SF32LB52X_RTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->tick_timer = ptimer_init(rtc_tick, s, 0);
    ptimer_transaction_begin(s->tick_timer);
    ptimer_set_freq(s->tick_timer, SF32LB52X_RTC_TICK_HZ);
    ptimer_transaction_commit(s->tick_timer);

    s->sync_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, rtc_sync_expire, s);
}

static void sf32lb52x_rtc_finalize(Object *obj)
{
    SF32LB52XRtcState *s = SF32LB52X_RTC(obj);

    ptimer_free(s->tick_timer);
    timer_free(s->sync_timer);
}

static int sf32lb52x_rtc_post_load(void *opaque, int version_id)
{
    SF32LB52XRtcState *s = opaque;

    rtc_update_irq(s);

    return 0;
}

static const VMStateDescription vmstate_sf32lb52x_rtc = {
    .name = TYPE_SF32LB52X_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sf32lb52x_rtc_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_PTIMER(tick_timer, SF32LB52XRtcState),
        VMSTATE_TIMER_PTR(sync_timer, SF32LB52XRtcState),
        VMSTATE_UINT32(tr, SF32LB52XRtcState),
        VMSTATE_UINT32(dr, SF32LB52XRtcState),
        VMSTATE_UINT32(cr, SF32LB52XRtcState),
        VMSTATE_UINT32(psc, SF32LB52XRtcState),
        VMSTATE_UINT32(alrmtr, SF32LB52XRtcState),
        VMSTATE_UINT32(alrmdr, SF32LB52XRtcState),
        VMSTATE_UINT32(isr_flags, SF32LB52XRtcState),
        VMSTATE_UINT32_ARRAY(bkp, SF32LB52XRtcState, SF32LB52X_RTC_NUM_BKP),
        VMSTATE_BOOL(init_mode, SF32LB52XRtcState),
        VMSTATE_BOOL(inits, SF32LB52XRtcState),
        VMSTATE_BOOL(rsf, SF32LB52XRtcState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property sf32lb52x_rtc_properties[] = {
    DEFINE_PROP_LINK("cfg", SF32LB52XRtcState, cfg,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static void sf32lb52x_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_rtc_reset_hold;
    dc->realize = sf32lb52x_rtc_realize;
    device_class_set_props(dc, sf32lb52x_rtc_properties);
    dc->desc = "SiFli SF32LB52X RTC";
    dc->vmsd = &vmstate_sf32lb52x_rtc;
    dc->user_creatable = false;
}

static const TypeInfo sf32lb52x_rtc_types[] = {
    {
        .name = TYPE_SF32LB52X_RTC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XRtcState),
        .instance_finalize = sf32lb52x_rtc_finalize,
        .class_init = sf32lb52x_rtc_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_rtc_types)
