/*
 * QTest for the SiFli SF32LB52X RTC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Covers the RTC register model (vendor rtc.h, SiFli HAL/LL,
 * Zephyr rtc_sf32lb): reset values, the INIT/INITF handshake, BCD calendar
 * increment, RSF
 * resynchronization, masked alarm matching and IRQ49, rw0c flag clearing,
 * and backup-register persistence across a system reset.
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define MACHINE "sf32lb52-devkit-lcd"
#define RTC_BASE 0x500cb000

/* Register offsets (vendor header rtc.h:10-42). */
#define RTC_TR      0x00
#define RTC_DR      0x04
#define RTC_CR      0x08
#define RTC_ISR     0x0c
#define RTC_PSCLR   0x10
#define RTC_WUTR    0x14
#define RTC_ALRMTR  0x18
#define RTC_ALRMDR  0x1c
#define RTC_OR      0x2c
#define RTC_BKP0R   0x30
#define RTC_BKP9R   0x54

/* ISR bits (rtc.h:160-192). */
#define RTC_ISR_ALRMWF (1U << 0)
#define RTC_ISR_ALRMF  (1U << 1)
#define RTC_ISR_WUTWF  (1U << 2)
#define RTC_ISR_WUTF   (1U << 3)
#define RTC_ISR_TSF    (1U << 4)
#define RTC_ISR_TSOVF  (1U << 5)
#define RTC_ISR_RSF    (1U << 7)
#define RTC_ISR_INITS  (1U << 8)
#define RTC_ISR_INITF  (1U << 9)
#define RTC_ISR_INIT   (1U << 10)
#define RTC_ISR_RESET  (RTC_ISR_ALRMWF | RTC_ISR_WUTWF | RTC_ISR_RSF)

/* CR bits (rtc.h:101-157). */
#define RTC_CR_ALRME  (1U << 8)
#define RTC_CR_ALRMIE (1U << 11)

/* ALRMDR mask bits (rtc.h:252-272). MSK=1 means the field is ignored. */
#define RTC_ALRMDR_MSKS  (1U << 24)
#define RTC_ALRMDR_MSKMN (1U << 25)
#define RTC_ALRMDR_MSKH  (1U << 26)
#define RTC_ALRMDR_MSKD  (1U << 27)
#define RTC_ALRMDR_MSKM  (1U << 28)
#define RTC_ALRMDR_MSKWD (1U << 29)

/* The RTC IRQ49 output is a sysbus IRQ line named "sysbus-irq". */
#define RTC_QOM_PATH "/machine/soc/rtc"
#define RTC_IRQ_NAME "sysbus-irq"

#define ONE_SEC_NS 1000000000LL

static QTestState *rtc_qtest_start(void)
{
    return qtest_init("-machine " MACHINE " -serial null");
}

static void rtc_irq_intercept(QTestState *qts)
{
    qtest_irq_intercept_out_named(qts, RTC_QOM_PATH, RTC_IRQ_NAME);
}

static uint32_t rtc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, RTC_BASE + offset);
}

static void rtc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, RTC_BASE + offset, value);
}

/* BCD encoders for the TR/DR register layout (rtc.h:46-98). */
static uint32_t bcd_time(unsigned hour, unsigned min, unsigned sec)
{
    return (((hour / 10U) & 0x3U) << 29) | ((hour % 10U) << 25) |
           (((min / 10U) & 0x7U) << 22) | ((min % 10U) << 18) |
           (((sec / 10U) & 0x7U) << 15) | ((sec % 10U) << 11);
}

static unsigned bcd_field(uint32_t reg, unsigned pos, uint32_t msk)
{
    uint32_t field = (reg >> pos) & msk;

    return ((field >> 4) * 10U) + (field & 0xfU);
}

static unsigned tr_sec(uint32_t tr)
{
    return bcd_field(tr, 11, 0x7f);
}

static unsigned tr_min(uint32_t tr)
{
    return bcd_field(tr, 18, 0x7f);
}

static unsigned tr_hour(uint32_t tr)
{
    return bcd_field(tr, 25, 0x3f);
}

static uint32_t bcd_date(unsigned year, unsigned month, unsigned day,
                         unsigned weekday)
{
    return (((year / 10U) & 0xfU) << 20) | ((year % 10U) << 16) |
           ((weekday & 0x7U) << 13) | (((month / 10U) & 0x1U) << 12) |
           ((month % 10U) << 8) | (((day / 10U) & 0x3U) << 4) | (day % 10U);
}

static unsigned dr_day(uint32_t dr)
{
    return bcd_field(dr, 0, 0x3f);
}

static unsigned dr_month(uint32_t dr)
{
    return bcd_field(dr, 8, 0x1f);
}

static unsigned dr_year(uint32_t dr)
{
    return bcd_field(dr, 16, 0xff);
}

static unsigned dr_wd(uint32_t dr)
{
    return bcd_field(dr, 13, 0x7);
}

/* Driver-style ISR update: read-modify-write, INIT/flag bits as written. */
static void rtc_isr_rmw(QTestState *qts, uint32_t set, uint32_t clear)
{
    uint32_t isr = rtc_readl(qts, RTC_ISR);

    rtc_writel(qts, RTC_ISR, (isr | set) & ~clear);
}

static void rtc_enter_init(QTestState *qts)
{
    rtc_isr_rmw(qts, RTC_ISR_INIT, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_INITF, ==, RTC_ISR_INITF);
}

static void rtc_exit_init(QTestState *qts)
{
    rtc_isr_rmw(qts, 0, RTC_ISR_INIT);
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_INITF, ==, 0);
}

static void rtc_set_datetime(QTestState *qts, uint32_t tr, uint32_t dr)
{
    rtc_enter_init(qts);
    rtc_writel(qts, RTC_TR, tr);
    rtc_writel(qts, RTC_DR, dr);
    rtc_exit_init(qts);
}

static void test_reset_values(void)
{
    QTestState *qts = rtc_qtest_start();

    g_assert_cmphex(rtc_readl(qts, RTC_TR), ==, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_DR), ==, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_CR), ==, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_PSCLR), ==, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_ALRMTR), ==, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_ALRMDR), ==, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_BKP0R), ==, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_BKP9R), ==, 0);
    /* ALRMWF/WUTWF are write-ready, RSF reports shadow registers synced. */
    g_assert_cmphex(rtc_readl(qts, RTC_ISR), ==, RTC_ISR_RESET);

    qtest_quit(qts);
}

static void test_init_mode_handshake(void)
{
    QTestState *qts = rtc_qtest_start();
    uint32_t tr = bcd_time(12, 34, 56);
    uint32_t dr = bcd_date(24, 2, 29, 4);

    rtc_enter_init(qts);
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_INIT, ==, RTC_ISR_INIT);
    rtc_writel(qts, RTC_TR, tr);
    rtc_writel(qts, RTC_DR, dr);
    g_assert_cmphex(rtc_readl(qts, RTC_TR), ==, tr);
    g_assert_cmphex(rtc_readl(qts, RTC_DR), ==, dr);
    rtc_exit_init(qts);
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_INITS, ==, RTC_ISR_INITS);

    qtest_quit(qts);
}

static void test_time_write_requires_init_mode(void)
{
    QTestState *qts = rtc_qtest_start();

    rtc_writel(qts, RTC_TR, bcd_time(1, 2, 3));
    rtc_writel(qts, RTC_DR, bcd_date(24, 1, 1, 1));
    g_assert_cmphex(rtc_readl(qts, RTC_TR), ==, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_DR), ==, 0);

    qtest_quit(qts);
}

static void test_calendar_rollover(void)
{
    QTestState *qts = rtc_qtest_start();
    uint32_t tr = bcd_time(23, 59, 58) | 0x123; /* non-zero subsecond */
    uint32_t dr = bcd_date(20, 1, 7, 2);
    uint32_t reg;

    rtc_set_datetime(qts, tr, dr);
    qtest_clock_step(qts, ONE_SEC_NS);
    reg = rtc_readl(qts, RTC_TR);
    g_assert_cmpuint(tr_hour(reg), ==, 23);
    g_assert_cmpuint(tr_min(reg), ==, 59);
    g_assert_cmpuint(tr_sec(reg), ==, 59);
    g_assert_cmphex(reg & 0x3ff, ==, 0); /* SS cleared on the tick */

    qtest_clock_step(qts, ONE_SEC_NS);
    reg = rtc_readl(qts, RTC_TR);
    g_assert_cmpuint(tr_hour(reg), ==, 0);
    g_assert_cmpuint(tr_min(reg), ==, 0);
    g_assert_cmpuint(tr_sec(reg), ==, 0);
    reg = rtc_readl(qts, RTC_DR);
    g_assert_cmpuint(dr_day(reg), ==, 8);
    g_assert_cmpuint(dr_month(reg), ==, 1);
    g_assert_cmpuint(dr_year(reg), ==, 20);
    g_assert_cmpuint(dr_wd(reg), ==, 3);

    qtest_quit(qts);
}

static void test_calendar_frozen_in_init_mode(void)
{
    QTestState *qts = rtc_qtest_start();

    rtc_set_datetime(qts, bcd_time(10, 0, 0), bcd_date(24, 6, 1, 6));
    rtc_enter_init(qts);
    qtest_clock_step(qts, 2 * ONE_SEC_NS);
    g_assert_cmpuint(tr_sec(rtc_readl(qts, RTC_TR)), ==, 0);
    g_assert_cmpuint(tr_min(rtc_readl(qts, RTC_TR)), ==, 0);
    rtc_exit_init(qts);
    qtest_clock_step(qts, ONE_SEC_NS);
    g_assert_cmpuint(tr_sec(rtc_readl(qts, RTC_TR)), ==, 1);

    qtest_quit(qts);
}

static void test_rsf_resync(void)
{
    QTestState *qts = rtc_qtest_start();
    uint32_t isr;

    isr = rtc_readl(qts, RTC_ISR);
    g_assert_cmphex(isr & RTC_ISR_RSF, ==, RTC_ISR_RSF);
    /* rc_w0: writing 0 clears RSF (ll_rtc.h:298-307). */
    rtc_writel(qts, RTC_ISR, isr & ~RTC_ISR_RSF);
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_RSF, ==, 0);
    qtest_clock_step(qts, 1000000); /* 1 ms > 2 RTCCLK resync delay */
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_RSF, ==, RTC_ISR_RSF);

    qtest_quit(qts);
}

static void test_alarm_flag_and_irq(void)
{
    QTestState *qts = rtc_qtest_start();
    uint32_t isr;

    rtc_irq_intercept(qts);
    rtc_set_datetime(qts, bcd_time(12, 0, 0), bcd_date(24, 1, 7, 1));
    rtc_writel(qts, RTC_ALRMTR, bcd_time(12, 0, 1));
    /* Ignore the date fields so the alarm is time-of-day only. */
    rtc_writel(qts, RTC_ALRMDR,
               RTC_ALRMDR_MSKD | RTC_ALRMDR_MSKM | RTC_ALRMDR_MSKWD);
    rtc_writel(qts, RTC_CR, RTC_CR_ALRME | RTC_CR_ALRMIE);

    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_ALRMF, ==, 0);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 0);

    qtest_clock_step(qts, ONE_SEC_NS);
    isr = rtc_readl(qts, RTC_ISR);
    g_assert_cmphex(isr & RTC_ISR_ALRMF, ==, RTC_ISR_ALRMF);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 1);

    /* rw0c: writing the flag back as 0 clears it (Zephyr driver path). */
    rtc_writel(qts, RTC_ISR, isr & ~RTC_ISR_ALRMF);
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_ALRMF, ==, 0);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 0);

    qtest_quit(qts);
}

static void test_alarm_masked_seconds(void)
{
    QTestState *qts = rtc_qtest_start();

    rtc_set_datetime(qts, bcd_time(12, 0, 0), bcd_date(24, 1, 7, 1));
    rtc_writel(qts, RTC_ALRMTR, bcd_time(12, 0, 0));
    rtc_writel(qts, RTC_ALRMDR,
               RTC_ALRMDR_MSKS | RTC_ALRMDR_MSKD | RTC_ALRMDR_MSKM |
               RTC_ALRMDR_MSKWD);
    rtc_writel(qts, RTC_CR, RTC_CR_ALRME | RTC_CR_ALRMIE);

    /* Next tick is 12:00:01; seconds are masked so the alarm matches. */
    qtest_clock_step(qts, ONE_SEC_NS);
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_ALRMF, ==, RTC_ISR_ALRMF);

    qtest_quit(qts);
}

static void test_alarm_irq_disabled(void)
{
    QTestState *qts = rtc_qtest_start();

    rtc_irq_intercept(qts);
    rtc_set_datetime(qts, bcd_time(8, 0, 0), bcd_date(24, 3, 3, 7));
    rtc_writel(qts, RTC_ALRMTR, bcd_time(8, 0, 1));
    rtc_writel(qts, RTC_ALRMDR,
               RTC_ALRMDR_MSKD | RTC_ALRMDR_MSKM | RTC_ALRMDR_MSKWD);
    rtc_writel(qts, RTC_CR, RTC_CR_ALRME);

    qtest_clock_step(qts, ONE_SEC_NS);
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_ALRMF, ==, RTC_ISR_ALRMF);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 0);

    /* Enabling ALRMIE with ALRMF already set raises IRQ49. */
    rtc_writel(qts, RTC_CR, RTC_CR_ALRME | RTC_CR_ALRMIE);
    g_assert_cmpint(qtest_get_irq(qts, 0), ==, 1);

    qtest_quit(qts);
}

static void test_backup_register_persistence(void)
{
    QTestState *qts = rtc_qtest_start();

    rtc_writel(qts, RTC_BKP0R, 0xdeadbeef);
    rtc_writel(qts, RTC_BKP9R, 0xcafef00d);
    g_assert_cmphex(rtc_readl(qts, RTC_BKP0R), ==, 0xdeadbeef);
    g_assert_cmphex(rtc_readl(qts, RTC_BKP9R), ==, 0xcafef00d);

    rtc_writel(qts, RTC_PSCLR, 0x12345678);
    qtest_system_reset(qts);

    /* Normal registers reset, battery-domain backup registers persist. */
    g_assert_cmphex(rtc_readl(qts, RTC_PSCLR), ==, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_TR), ==, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_BKP0R), ==, 0xdeadbeef);
    g_assert_cmphex(rtc_readl(qts, RTC_BKP9R), ==, 0xcafef00d);

    qtest_quit(qts);
}

static void test_unimplemented_and_bad_offsets(void)
{
    QTestState *qts = rtc_qtest_start();

    /* Wakeup timer / timestamp / output registers are unimplemented. */
    g_assert_cmphex(rtc_readl(qts, RTC_WUTR), ==, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_OR), ==, 0);
    rtc_writel(qts, RTC_WUTR, 0xffffffff);
    g_assert_cmphex(rtc_readl(qts, RTC_WUTR), ==, 0);

    /* Unmapped offsets in the 4 KiB window are a guest error, not a hang. */
    g_assert_cmphex(qtest_readl(qts, RTC_BASE + 0xffc), ==, 0);
    qtest_writel(qts, RTC_BASE + 0xffc, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, RTC_BASE + 0xffc), ==, 0);

    qtest_quit(qts);
}

/* The HPSYS_CFG block mirrors TR/DR for the driver's get_time() path. */
static void test_rtc_cfg_shadow(void)
{
    QTestState *qts = rtc_qtest_start();
    const uint32_t hpsys_cfg = 0x5000b000;
    uint32_t tr = bcd_time(12, 34, 56);
    uint32_t dr = bcd_date(24, 6, 15, 6);

    rtc_set_datetime(qts, tr, dr);
    g_assert_cmphex(qtest_readl(qts, hpsys_cfg + 0x14), ==, tr);
    g_assert_cmphex(qtest_readl(qts, hpsys_cfg + 0x18), ==, dr);

    /* One calendar tick publishes the advanced time again. */
    qtest_clock_step(qts, ONE_SEC_NS);
    g_assert_cmphex(qtest_readl(qts, hpsys_cfg + 0x14),
                    ==, rtc_readl(qts, RTC_TR));

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sf32lb52x/rtc/reset", test_reset_values);
    qtest_add_func("/sf32lb52x/rtc/init-mode", test_init_mode_handshake);
    qtest_add_func("/sf32lb52x/rtc/time-write-requires-init",
                   test_time_write_requires_init_mode);
    qtest_add_func("/sf32lb52x/rtc/calendar-rollover",
                   test_calendar_rollover);
    qtest_add_func("/sf32lb52x/rtc/calendar-frozen-in-init",
                   test_calendar_frozen_in_init_mode);
    qtest_add_func("/sf32lb52x/rtc/rsf-resync", test_rsf_resync);
    qtest_add_func("/sf32lb52x/rtc/alarm-flag-irq",
                   test_alarm_flag_and_irq);
    qtest_add_func("/sf32lb52x/rtc/alarm-masked-seconds",
                   test_alarm_masked_seconds);
    qtest_add_func("/sf32lb52x/rtc/alarm-irq-disabled",
                   test_alarm_irq_disabled);
    qtest_add_func("/sf32lb52x/rtc/backup-persistence",
                   test_backup_register_persistence);
    qtest_add_func("/sf32lb52x/rtc/unimplemented-access",
                   test_unimplemented_and_bad_offsets);
    qtest_add_func("/sf32lb52x/rtc/cfg-shadow", test_rtc_cfg_shadow);

    return g_test_run();
}
