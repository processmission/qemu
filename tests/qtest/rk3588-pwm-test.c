/*
 * QEMU RK3588 PWM tests
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu/bitops.h"
#include "libqtest.h"
#include "migration/migration-qmp.h"

#define ROCK5B_MACHINE_NAME     "rock-5b"

#define PWM_CNT                 0x00
#define PWM_PERIOD              0x04
#define PWM_DUTY                0x08
#define PWM_CTRL                0x0c
#define PWM_INTSTS              0x40
#define PWM_INT_EN              0x44
#define PWM_FIFO_INTSTS         0x54
#define PWM_VERSION_ID          0x5c
#define PWM_PWRMATCH_LPRE       0x84
#define PWM_PWRMATCH_VALUE0     0x98

#define PWM_CTRL_CLK_SEL        BIT(9)
#define PWM_CTRL_CLK_SRC_SEL    BIT(10)
#define PWM_CTRL_OUTPUT_MODE    BIT(5)
#define PWM_CTRL_CONLOCK        BIT(6)
#define PWM_CTRL_CH_CNT_EN      BIT(7)
#define PWM_CTRL_INACTIVE_POL   BIT(4)
#define PWM_CTRL_DUTY_POL       BIT(3)
#define PWM_CTRL_MODE_ONESHOT   0u
#define PWM_CTRL_MODE_CONTINUOUS (1u << 1)
#define PWM_CTRL_PWM_EN         BIT(0)

#define PWM_CLOCK_HZ            24000000
#define PWM_TEST_PERIOD         2400
#define PWM_TEST_DUTY           600
#define PWM_TEST_DUTY_NS        25000
#define PWM_TEST_PERIOD_NS      100000
#define PWM_FAN_PERIOD          1200
#define PWM_FAN_DUTY            600
#define PWM_ALT_CLOCK_HZ        12000000
#define PWM_FRACTIONAL_CLOCK_HZ 25000000
#define PWM_ROUNDING_CLOCK_HZ   30000000

#define RK3588_PWM_VERSION      0x02120b34

static const uint64_t rock5b_pwm_bases[] = {
    0xfd8b0000, 0xfebd0000, 0xfebe0000, 0xfebf0000,
};

typedef struct PWMHeaderChannel {
    unsigned int controller;
    unsigned int channel;
    const char *pins;
} PWMHeaderChannel;

static const PWMHeaderChannel pwm_header_channels[] = {
    { 0, 2, "36/PWM2_M1" },
    { 0, 3, "38/PWM3_IR_M1" },
    { 1, 1, "18/PWM5_M2" },
    { 1, 2, "28/PWM6_M2" },
    { 1, 3, "27/PWM7_IR_M3" },
    { 2, 0, "33/PWM8_M0" },
    { 3, 0, "12/PWM12_M0" },
    { 3, 1, "31/PWM13_M2, 35/PWM13_M0" },
    { 3, 2, "5/PWM14_M1, 32/PWM14_M0" },
    { 3, 3, "3/PWM15_IR_M1, 7/PWM15_IR_M0, 29/PWM15_IR_M3" },
};

static QTestState *rock5b_pwm_qtest_start(void)
{
    return qtest_init("-machine " ROCK5B_MACHINE_NAME
                      " -m 256M -serial null -display none");
}

static void pwm_write(QTestState *qts, uint64_t base, unsigned int channel,
                      uint32_t reg, uint32_t value)
{
    qtest_writel(qts, base + channel * 0x10 + reg, value);
}

static uint32_t pwm_read(QTestState *qts, uint64_t base, unsigned int channel,
                         uint32_t reg)
{
    return qtest_readl(qts, base + channel * 0x10 + reg);
}

static void pwm_enable_continuous_values(QTestState *qts, uint64_t base,
                                         unsigned int channel,
                                         bool duty_polarity,
                                         bool inactive_polarity,
                                         uint32_t period, uint32_t duty,
                                         uint32_t extra_ctrl)
{
    uint32_t ctrl = PWM_CTRL_MODE_CONTINUOUS | extra_ctrl;

    if (duty_polarity) {
        ctrl |= PWM_CTRL_DUTY_POL;
    }
    if (inactive_polarity) {
        ctrl |= PWM_CTRL_INACTIVE_POL;
    }

    pwm_write(qts, base, channel, PWM_PERIOD, period);
    pwm_write(qts, base, channel, PWM_DUTY, duty);
    pwm_write(qts, base, channel, PWM_CTRL, ctrl);
    pwm_write(qts, base, channel, PWM_CTRL, ctrl | PWM_CTRL_PWM_EN);
}

static void pwm_enable_continuous(QTestState *qts, uint64_t base,
                                  unsigned int channel, bool duty_polarity,
                                  bool inactive_polarity)
{
    pwm_enable_continuous_values(qts, base, channel, duty_polarity,
                                 inactive_polarity, PWM_TEST_PERIOD,
                                 PWM_TEST_DUTY, 0);
}

static void pwm_check_continuous_pulse(QTestState *qts, uint64_t base,
                                       unsigned int channel, uint32_t period,
                                       uint32_t duty, uint32_t extra_ctrl,
                                       int64_t period_ns, int64_t duty_ns)
{
    pwm_enable_continuous_values(qts, base, channel, true, false,
                                 period, duty, extra_ctrl);
    g_assert_true(qtest_get_irq(qts, channel));

    qtest_clock_step(qts, duty_ns - 1);
    g_assert_true(qtest_get_irq(qts, channel));
    qtest_clock_step(qts, 1);
    g_assert_false(qtest_get_irq(qts, channel));

    qtest_clock_step(qts, period_ns - duty_ns - 1);
    g_assert_false(qtest_get_irq(qts, channel));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, channel));
}

static void assert_qom_child(QTestState *qts, const char *parent,
                             const char *name, const char *type)
{
    g_autofree char *expected_type = g_strdup_printf("child<%s>", type);
    QDict *response;
    QList *properties;
    QListEntry *entry;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-list',"
                         "  'arguments': { 'path': %s } }", parent);
    g_assert_false(qdict_haskey(response, "error"));
    properties = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (!g_strcmp0(qdict_get_str(property, "name"), name)) {
            g_assert_cmpstr(qdict_get_str(property, "type"), ==,
                            expected_type);
            qobject_unref(response);
            return;
        }
    }

    qobject_unref(response);
    g_error("QOM child '%s' not found below '%s'", name, parent);
}

static void test_pwm_mapping_and_reset(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();

    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_pwm_bases); i++) {
        g_autofree char *name = g_strdup_printf("pwm%u", i);

        assert_qom_child(qts, "/machine/soc", name, "rk3588-pwm");
        g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], 0,
                                 PWM_VERSION_ID), ==, RK3588_PWM_VERSION);
        g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], 0,
                                 PWM_FIFO_INTSTS), ==, BIT(4));
        g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], 0,
                                 PWM_PWRMATCH_LPRE), ==, 0x238c22c4);

        for (unsigned int j = 0; j < 4; j++) {
            g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], j, PWM_CNT),
                            ==, 0);
            g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], j,
                                     PWM_PERIOD), ==, 0);
            g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], j,
                                     PWM_DUTY), ==, 0);
            g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], j,
                                     PWM_CTRL), ==, 0);
        }
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_pwm_bases); i++) {
        pwm_write(qts, rock5b_pwm_bases[i], 0, PWM_VERSION_ID, 0);

        for (unsigned int j = 0; j < 4; j++) {
            pwm_write(qts, rock5b_pwm_bases[i], j, PWM_PERIOD,
                      i * 4 + j + 1);
            pwm_write(qts, rock5b_pwm_bases[i], j, PWM_CNT, 0xdeadbeef);
        }
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_pwm_bases); i++) {
        g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], 0,
                                 PWM_VERSION_ID), ==, RK3588_PWM_VERSION);

        for (unsigned int j = 0; j < 4; j++) {
            g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], j,
                                     PWM_PERIOD), ==, i * 4 + j + 1);
            g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], j, PWM_CNT),
                            ==, 0);
        }
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_pwm_bases); i++) {
        for (unsigned int j = 0; j < 4; j++) {
            pwm_write(qts, rock5b_pwm_bases[i], j, PWM_CTRL, UINT32_MAX);
            g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], j, PWM_CTRL),
                            ==, 0xffff77ff);
        }

        qtest_writel(qts, rock5b_pwm_bases[i] + PWM_INT_EN, UINT32_MAX);
        g_assert_cmphex(qtest_readl(qts, rock5b_pwm_bases[i] + PWM_INT_EN),
                        ==, 0x8f);
        qtest_writel(qts, rock5b_pwm_bases[i] + PWM_FIFO_INTSTS, UINT32_MAX);
        g_assert_cmphex(qtest_readl(qts,
                                    rock5b_pwm_bases[i] + PWM_FIFO_INTSTS),
                        ==, BIT(4));
        qtest_writel(qts, rock5b_pwm_bases[i] + PWM_PWRMATCH_VALUE0,
                     0x12345678);
        g_assert_cmphex(qtest_readl(qts,
                                    rock5b_pwm_bases[i] + PWM_PWRMATCH_VALUE0),
                        ==, 0x12345678);
    }

    qtest_system_reset(qts);
    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_pwm_bases); i++) {
        for (unsigned int j = 0; j < 4; j++) {
            g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], j,
                                     PWM_CNT), ==, 0);
            g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], j,
                                     PWM_PERIOD), ==, 0);
            g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], j,
                                     PWM_DUTY), ==, 0);
            g_assert_cmphex(pwm_read(qts, rock5b_pwm_bases[i], j,
                                     PWM_CTRL), ==, 0);
        }
    }

    qtest_quit(qts);
}

static void test_pwm_output_reset(void)
{
    for (unsigned int controller = 0;
         controller < ARRAY_SIZE(rock5b_pwm_bases); controller++) {
        QTestState *qts = rock5b_pwm_qtest_start();
        g_autofree char *qom_path =
            g_strdup_printf("/machine/soc/pwm%u", controller);
        const uint64_t base = rock5b_pwm_bases[controller];

        qtest_irq_intercept_out_named(qts, qom_path, "pwm-out");

        for (unsigned int channel = 0; channel < 4; channel++) {
            pwm_enable_continuous(qts, base, channel, true, false);
        }
        for (unsigned int channel = 0; channel < 4; channel++) {
            g_assert_true(qtest_get_irq(qts, channel));
        }

        qtest_system_reset(qts);
        for (unsigned int channel = 0; channel < 4; channel++) {
            g_assert_false(qtest_get_irq(qts, channel));
        }

        qtest_quit(qts);
    }
}

static void test_pwm_continuous_output(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();
    const uint64_t base = rock5b_pwm_bases[0];

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");

    pwm_enable_continuous(qts, base, 0, true, false);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, PWM_TEST_DUTY_NS - 1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CNT), ==,
                    PWM_TEST_DUTY - 1);

    qtest_clock_step(qts, 1);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CNT), ==,
                    PWM_TEST_DUTY);

    qtest_clock_step(qts, PWM_TEST_PERIOD_NS - PWM_TEST_DUTY_NS - 1);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CNT), ==, 0);

    /* Inverting duty polarity starts the period in the inactive state. */
    pwm_write(qts, base, 0, PWM_CTRL, PWM_CTRL_INACTIVE_POL);
    pwm_enable_continuous(qts, base, 0, false, true);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, PWM_TEST_DUTY_NS);
    g_assert_true(qtest_get_irq(qts, 0));

    /* Disabling immediately applies the configured inactive polarity. */
    pwm_write(qts, base, 0, PWM_CTRL, PWM_CTRL_INACTIVE_POL);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CNT), ==, 0);

    qtest_system_reset(qts);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CTRL), ==, 0);
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_VERSION_ID), ==,
                    RK3588_PWM_VERSION);

    qtest_quit(qts);
}

static void test_pwm_runtime_update(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();
    const uint64_t base = rock5b_pwm_bases[0];

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");
    pwm_enable_continuous(qts, base, 0, true, false);

    qtest_clock_step(qts, PWM_TEST_DUTY_NS);
    g_assert_false(qtest_get_irq(qts, 0));

    /* The old period remains effective through its current boundary. */
    pwm_write(qts, base, 0, PWM_PERIOD, 4800);
    pwm_write(qts, base, 0, PWM_DUTY, 1200);
    qtest_clock_step(qts, PWM_TEST_PERIOD_NS - PWM_TEST_DUTY_NS - 1);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    /* The new 200 us period and 50 us duty take effect from that boundary. */
    qtest_clock_step(qts, 49999);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 149999);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_pwm_channel_independence(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();
    const uint64_t base = rock5b_pwm_bases[0];

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");

    pwm_enable_continuous(qts, base, 0, true, false);
    pwm_enable_continuous(qts, base, 1, false, true);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_clock_step(qts, PWM_TEST_DUTY_NS);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    qtest_clock_step(qts, PWM_TEST_PERIOD_NS - PWM_TEST_DUTY_NS);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_pwm_header_channels(void)
{
    for (unsigned int controller = 0;
         controller < ARRAY_SIZE(rock5b_pwm_bases); controller++) {
        QTestState *qts = rock5b_pwm_qtest_start();
        g_autofree char *qom_path =
            g_strdup_printf("/machine/soc/pwm%u", controller);

        qtest_irq_intercept_out_named(qts, qom_path, "pwm-out");

        for (unsigned int channel = 0; channel < 4; channel++) {
            g_assert_false(qtest_get_irq(qts, channel));
        }

        for (unsigned int i = 0; i < ARRAY_SIZE(pwm_header_channels); i++) {
            const PWMHeaderChannel *mapping = &pwm_header_channels[i];

            if (mapping->controller != controller) {
                continue;
            }

            g_test_message("header %s: pwm%u channel %u",
                           mapping->pins, controller, mapping->channel);
            g_assert_false(qtest_get_irq(qts, mapping->channel));
            pwm_check_continuous_pulse(qts,
                                       rock5b_pwm_bases[controller],
                                       mapping->channel,
                                       PWM_TEST_PERIOD, PWM_TEST_DUTY, 0,
                                       PWM_TEST_PERIOD_NS, PWM_TEST_DUTY_NS);
            qtest_system_reset(qts);
        }

        qtest_quit(qts);
    }
}

static void test_pwm_board_fan_channel(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();
    const uint64_t base = rock5b_pwm_bases[0];

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");
    g_assert_false(qtest_get_irq(qts, 1));

    /*
     * ROCK 5B uses &pwm1 with a 50000 ns period. In RK3588 terms that is
     * PWM0 channel 1 at 24 MHz: 1200 ticks and 50 us per cycle.
     */
    pwm_check_continuous_pulse(qts, base, 1, PWM_FAN_PERIOD, PWM_FAN_DUTY, 0,
                               50000, 25000);

    /* Normal polarity stops at inactive_pol=0. */
    pwm_write(qts, base, 1, PWM_CTRL, 0);
    g_assert_false(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_pwm_runtime_polarity_update(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();
    const uint64_t base = rock5b_pwm_bases[0];
    const uint32_t inverse_ctrl = PWM_CTRL_MODE_CONTINUOUS |
                                  PWM_CTRL_INACTIVE_POL | PWM_CTRL_PWM_EN;

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");
    pwm_enable_continuous(qts, base, 0, true, false);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, PWM_TEST_DUTY_NS);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, PWM_TEST_DUTY_NS);
    g_assert_false(qtest_get_irq(qts, 0));

    /*
     * Keep pwm_en set so the new polarity is shadowed. The current cycle
     * must finish with the old polarity; the next cycle starts low.
     */
    pwm_write(qts, base, 0, PWM_CTRL, inverse_ctrl);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, PWM_TEST_PERIOD_NS / 2 - 1);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, PWM_TEST_DUTY_NS - 1);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_pwm_input_clock_select(void)
{
    QTestState *qts =
        qtest_initf("-machine " ROCK5B_MACHINE_NAME
                    " -m 256M -serial null -display none"
                    " -global driver=rk3588-pwm,property=clock-hz,value=%u",
                    PWM_ALT_CLOCK_HZ);
    const uint64_t base = rock5b_pwm_bases[0];

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");

    /* clk_src_sel=0 selects the variable root clock: 12 MHz here. */
    pwm_check_continuous_pulse(qts, base, 0, PWM_ALT_CLOCK_HZ / 10000,
                               PWM_ALT_CLOCK_HZ / 40000, 0,
                               PWM_TEST_PERIOD_NS, PWM_TEST_DUTY_NS);

    /*
     * clk_src_sel=1 selects the fixed 24 MHz oscillator. Twice as many
     * ticks therefore describe the same 100 us/25 us waveform.
     */
    qtest_system_reset(qts);
    pwm_check_continuous_pulse(qts, base, 0, PWM_TEST_PERIOD,
                               PWM_TEST_DUTY, PWM_CTRL_CLK_SRC_SEL,
                               PWM_TEST_PERIOD_NS, PWM_TEST_DUTY_NS);

    qtest_quit(qts);
}

static void test_pwm_clock_divider(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();
    const uint64_t base = rock5b_pwm_bases[0];
    uint32_t ctrl = PWM_CTRL_MODE_CONTINUOUS | PWM_CTRL_PWM_EN |
                    PWM_CTRL_DUTY_POL | PWM_CTRL_CLK_SEL | (12u << 16);

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");

    /*
     * Select scale=12: 24 MHz / (2 * 12) = 1 MHz. The 100-tick period and
     * 25-tick duty therefore represent 100 us and 25 us.
     */
    pwm_write(qts, base, 0, PWM_PERIOD, 100);
    pwm_write(qts, base, 0, PWM_DUTY, 25);
    pwm_write(qts, base, 0, PWM_CTRL, ctrl);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 10000);
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CNT), ==, 10);
    qtest_clock_step(qts, 14999);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 74999);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    /* A prescale of 1 divides 24 MHz by two, giving 50 us and 200 us. */
    qtest_system_reset(qts);
    ctrl = PWM_CTRL_MODE_CONTINUOUS | PWM_CTRL_PWM_EN |
           PWM_CTRL_DUTY_POL | (1u << 12);
    pwm_write(qts, base, 0, PWM_PERIOD, PWM_TEST_PERIOD);
    pwm_write(qts, base, 0, PWM_DUTY, PWM_TEST_DUTY);
    pwm_write(qts, base, 0, PWM_CTRL, ctrl);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 49999);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 149999);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_pwm_fractional_divided_clock(void)
{
    QTestState *qts =
        qtest_initf("-machine " ROCK5B_MACHINE_NAME
                    " -m 256M -serial null -display none"
                    " -global driver=rk3588-pwm,property=clock-hz,value=%u",
                    PWM_FRACTIONAL_CLOCK_HZ);
    const uint64_t base = rock5b_pwm_bases[0];
    const uint32_t ctrl = PWM_CTRL_MODE_CONTINUOUS | PWM_CTRL_PWM_EN |
                          PWM_CTRL_DUTY_POL | (2u << 12);

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");

    /*
     * 25 MHz / 2^2 = 6.25 MHz. Eight ticks are 1280 ns and four ticks are
     * 640 ns; truncating the divided clock to 6 MHz would give 666 ns and
     * 1332 ns instead.
     */
    pwm_write(qts, base, 0, PWM_PERIOD, 8);
    pwm_write(qts, base, 0, PWM_DUTY, 4);
    pwm_write(qts, base, 0, PWM_CTRL, ctrl);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 639);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 639);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_pwm_period_rounding(void)
{
    QTestState *qts =
        qtest_initf("-machine " ROCK5B_MACHINE_NAME
                    " -m 256M -serial null -display none"
                    " -global driver=rk3588-pwm,property=clock-hz,value=%u",
                    PWM_ROUNDING_CLOCK_HZ);
    const uint64_t base = rock5b_pwm_bases[0];

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");

    /*
     * Four ticks at 30 MHz last 133 ns after truncation. The 2-tick duty
     * edge is 66 ns. The period boundary must use the complete four ticks,
     * not the independently rounded 66 ns + 66 ns remainder (132 ns).
     */
    pwm_write(qts, base, 0, PWM_PERIOD, 4);
    pwm_write(qts, base, 0, PWM_DUTY, 2);
    pwm_write(qts, base, 0, PWM_CTRL,
              PWM_CTRL_MODE_CONTINUOUS | PWM_CTRL_DUTY_POL |
              PWM_CTRL_PWM_EN);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 65);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_clock_step(qts, 66);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    /* The next complete period boundary is anchored at 133 + 133 ns. */
    qtest_clock_step(qts, 132);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_pwm_zero_and_full_duty(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();
    const uint64_t base = rock5b_pwm_bases[0];

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");

    /* 0% normal polarity holds the inverse of duty_pol for a whole period. */
    pwm_write(qts, base, 0, PWM_PERIOD, PWM_TEST_PERIOD);
    pwm_write(qts, base, 0, PWM_DUTY, 0);
    pwm_write(qts, base, 0, PWM_CTRL,
              PWM_CTRL_MODE_CONTINUOUS | PWM_CTRL_DUTY_POL |
              PWM_CTRL_PWM_EN);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, PWM_TEST_PERIOD_NS);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CNT), ==, 0);

    /* A staged update still commits at the next flat-output boundary. */
    pwm_write(qts, base, 0, PWM_DUTY, PWM_TEST_DUTY);
    qtest_clock_step(qts, PWM_TEST_PERIOD_NS - 1);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    /* 0% inversed polarity is the opposite flat level. */
    qtest_system_reset(qts);
    pwm_write(qts, base, 0, PWM_PERIOD, PWM_TEST_PERIOD);
    pwm_write(qts, base, 0, PWM_DUTY, 0);
    pwm_write(qts, base, 0, PWM_CTRL,
              PWM_CTRL_MODE_CONTINUOUS | PWM_CTRL_INACTIVE_POL |
              PWM_CTRL_PWM_EN);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, PWM_TEST_PERIOD_NS);
    g_assert_true(qtest_get_irq(qts, 0));

    /* 100% normal polarity holds duty_pol for a whole period. */
    qtest_system_reset(qts);
    pwm_write(qts, base, 0, PWM_PERIOD, PWM_TEST_PERIOD);
    pwm_write(qts, base, 0, PWM_DUTY, PWM_TEST_PERIOD);
    pwm_write(qts, base, 0, PWM_CTRL,
              PWM_CTRL_MODE_CONTINUOUS | PWM_CTRL_DUTY_POL |
              PWM_CTRL_PWM_EN);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, PWM_TEST_PERIOD_NS / 2);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, PWM_TEST_PERIOD_NS / 2);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CNT), ==, 0);

    /* 100% inversed polarity holds the low level. */
    qtest_system_reset(qts);
    pwm_write(qts, base, 0, PWM_PERIOD, PWM_TEST_PERIOD);
    pwm_write(qts, base, 0, PWM_DUTY, PWM_TEST_PERIOD);
    pwm_write(qts, base, 0, PWM_CTRL,
              PWM_CTRL_MODE_CONTINUOUS | PWM_CTRL_INACTIVE_POL |
              PWM_CTRL_PWM_EN);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, PWM_TEST_PERIOD_NS);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_pwm_invalid_period_and_duty(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();
    const uint64_t base = rock5b_pwm_bases[0];
    uint32_t ctrl = PWM_CTRL_MODE_CONTINUOUS | PWM_CTRL_DUTY_POL |
                    PWM_CTRL_PWM_EN;

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");

    /* period=0 cannot define a periodic waveform. */
    pwm_write(qts, base, 0, PWM_PERIOD, 0);
    pwm_write(qts, base, 0, PWM_DUTY, 0);
    pwm_write(qts, base, 0, PWM_CTRL, ctrl);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CNT), ==, 0);
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CTRL), ==, ctrl);
    qtest_clock_step(qts, PWM_TEST_PERIOD_NS);
    g_assert_false(qtest_get_irq(qts, 0));

    /* duty greater than period is an invalid Rockchip configuration. */
    qtest_system_reset(qts);
    pwm_write(qts, base, 0, PWM_PERIOD, PWM_TEST_PERIOD);
    pwm_write(qts, base, 0, PWM_DUTY, PWM_TEST_PERIOD + 1);
    pwm_write(qts, base, 0, PWM_CTRL, ctrl);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CNT), ==, 0);
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CTRL), ==, ctrl);
    qtest_clock_step(qts, PWM_TEST_PERIOD_NS);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_pwm_conlock_across_period(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();
    const uint64_t base = rock5b_pwm_bases[0];
    const uint32_t active_ctrl = PWM_CTRL_MODE_CONTINUOUS |
                                 PWM_CTRL_DUTY_POL | PWM_CTRL_PWM_EN;

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");
    pwm_enable_continuous(qts, base, 0, true, false);

    qtest_clock_step(qts, 75000);
    g_assert_false(qtest_get_irq(qts, 0));

    /*
     * Hold the lock across the old 100 us period boundary. The staged
     * 200 us/75 us configuration must not become effective.
     */
    pwm_write(qts, base, 0, PWM_CTRL, active_ctrl | PWM_CTRL_CONLOCK);
    pwm_write(qts, base, 0, PWM_PERIOD, 4800);
    pwm_write(qts, base, 0, PWM_DUTY, 1800);
    qtest_clock_step(qts, 24999);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    /* The old 25 us duty edge still occurs at 125 us. */
    qtest_clock_step(qts, 25000);
    g_assert_false(qtest_get_irq(qts, 0));

    /* Unlocking waits for the next old period boundary at 200 us. */
    pwm_write(qts, base, 0, PWM_CTRL, active_ctrl);
    qtest_clock_step(qts, 74999);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    /* The new 75 us duty now applies, then the new 200 us period wraps. */
    qtest_clock_step(qts, 74999);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 124999);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, 1);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_pwm_unimplemented_modes(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();
    const uint64_t base = rock5b_pwm_bases[0];

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");

    pwm_write(qts, base, 0, PWM_PERIOD, PWM_TEST_PERIOD);
    pwm_write(qts, base, 0, PWM_DUTY, PWM_TEST_DUTY);
    pwm_write(qts, base, 0, PWM_CTRL,
              PWM_CTRL_MODE_ONESHOT | PWM_CTRL_DUTY_POL |
              PWM_CTRL_PWM_EN);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CTRL) & PWM_CTRL_PWM_EN,
                    ==, PWM_CTRL_PWM_EN);

    pwm_write(qts, base, 0, PWM_CTRL,
              PWM_CTRL_MODE_CONTINUOUS | PWM_CTRL_OUTPUT_MODE |
              PWM_CTRL_DUTY_POL | PWM_CTRL_PWM_EN);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CTRL) & PWM_CTRL_PWM_EN,
                    ==, PWM_CTRL_PWM_EN);

    qtest_quit(qts);
}

static void test_pwm_ch_cnt_en_unimplemented(void)
{
    QTestState *qts = rock5b_pwm_qtest_start();
    const uint64_t base = rock5b_pwm_bases[0];
    const uint32_t active_ctrl = PWM_CTRL_MODE_CONTINUOUS |
                                 PWM_CTRL_DUTY_POL | PWM_CTRL_PWM_EN;
    const uint32_t unsupported_ctrl = active_ctrl | PWM_CTRL_CH_CNT_EN;

    qtest_irq_intercept_out_named(qts, "/machine/soc/pwm0", "pwm-out");

    pwm_write(qts, base, 0, PWM_PERIOD, PWM_TEST_PERIOD);
    pwm_write(qts, base, 0, PWM_DUTY, PWM_TEST_DUTY);
    pwm_write(qts, base, 0, PWM_CTRL, unsupported_ctrl);
    g_assert_cmphex(pwm_read(qts, base, 0, PWM_CTRL), ==,
                    unsupported_ctrl);
    g_assert_false(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, PWM_TEST_PERIOD_NS);
    g_assert_false(qtest_get_irq(qts, 0));

    /* Clearing the unsupported control restarts the regular waveform. */
    pwm_write(qts, base, 0, PWM_CTRL, active_ctrl);
    g_assert_true(qtest_get_irq(qts, 0));
    qtest_clock_step(qts, PWM_TEST_DUTY_NS);
    g_assert_false(qtest_get_irq(qts, 0));

    qtest_quit(qts);
}

static void test_pwm_migration(void)
{
    QTestState *src;
    QTestState *dst;
    g_autofree char *tmpdir =
        g_dir_make_tmp("rk3588-pwm-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);
    const uint64_t base = rock5b_pwm_bases[0];
    const uint32_t ctrl = PWM_CTRL_MODE_CONTINUOUS |
                          PWM_CTRL_DUTY_POL | PWM_CTRL_PWM_EN;
    int64_t now_ns;

    g_assert_nonnull(tmpdir);
    src = rock5b_pwm_qtest_start();
    dst = qtest_init("-machine " ROCK5B_MACHINE_NAME
                     " -m 256M -serial null -display none -incoming defer");
    qtest_irq_intercept_out_named(src, "/machine/soc/pwm0", "pwm-out");
    qtest_irq_intercept_out_named(dst, "/machine/soc/pwm0", "pwm-out");

    pwm_enable_continuous(src, base, 0, true, false);
    now_ns = qtest_clock_step(src, 10000);
    g_assert_true(qtest_get_irq(src, 0));
    g_assert_cmpint(qtest_clock_set(dst, now_ns), ==, now_ns);

    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    g_assert_cmphex(pwm_read(dst, base, 0, PWM_PERIOD), ==,
                    PWM_TEST_PERIOD);
    g_assert_cmphex(pwm_read(dst, base, 0, PWM_DUTY), ==, PWM_TEST_DUTY);
    g_assert_cmphex(pwm_read(dst, base, 0, PWM_CTRL), ==, ctrl);
    g_assert_cmphex(pwm_read(dst, base, 0, PWM_CNT), ==,
                    PWM_TEST_DUTY * 10000 / PWM_TEST_DUTY_NS);
    g_assert_true(qtest_get_irq(dst, 0));

    qtest_clock_step(dst, PWM_TEST_DUTY_NS - 10000 - 1);
    g_assert_true(qtest_get_irq(dst, 0));
    qtest_clock_step(dst, 1);
    g_assert_false(qtest_get_irq(dst, 0));
    qtest_clock_step(dst, PWM_TEST_PERIOD_NS - PWM_TEST_DUTY_NS - 1);
    g_assert_false(qtest_get_irq(dst, 0));
    qtest_clock_step(dst, 1);
    g_assert_true(qtest_get_irq(dst, 0));

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(ROCK5B_MACHINE_NAME)) {
        g_test_skip(ROCK5B_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3588/pwm/mapping-reset",
                   test_pwm_mapping_and_reset);
    qtest_add_func("/rk3588/pwm/output-reset",
                   test_pwm_output_reset);
    qtest_add_func("/rk3588/pwm/continuous-output",
                   test_pwm_continuous_output);
    qtest_add_func("/rk3588/pwm/runtime-update",
                   test_pwm_runtime_update);
    qtest_add_func("/rk3588/pwm/channel-independence",
                   test_pwm_channel_independence);
    qtest_add_func("/rk3588/pwm/header-channels",
                   test_pwm_header_channels);
    qtest_add_func("/rk3588/pwm/board-fan-channel",
                   test_pwm_board_fan_channel);
    qtest_add_func("/rk3588/pwm/runtime-polarity-update",
                   test_pwm_runtime_polarity_update);
    qtest_add_func("/rk3588/pwm/input-clock-select",
                   test_pwm_input_clock_select);
    qtest_add_func("/rk3588/pwm/clock-divider",
                   test_pwm_clock_divider);
    qtest_add_func("/rk3588/pwm/fractional-divided-clock",
                   test_pwm_fractional_divided_clock);
    qtest_add_func("/rk3588/pwm/period-rounding",
                   test_pwm_period_rounding);
    qtest_add_func("/rk3588/pwm/zero-and-full-duty",
                   test_pwm_zero_and_full_duty);
    qtest_add_func("/rk3588/pwm/invalid-period-duty",
                   test_pwm_invalid_period_and_duty);
    qtest_add_func("/rk3588/pwm/conlock-across-period",
                   test_pwm_conlock_across_period);
    qtest_add_func("/rk3588/pwm/unimplemented-modes",
                   test_pwm_unimplemented_modes);
    qtest_add_func("/rk3588/pwm/ch-cnt-en-unimplemented",
                   test_pwm_ch_cnt_en_unimplemented);
    qtest_add_func("/rk3588/pwm/migration", test_pwm_migration);

    return g_test_run();
}
