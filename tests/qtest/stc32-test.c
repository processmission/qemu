/*
 * STC32G144K246 machine tests
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/sockets.h"
#include "libqtest.h"

#define MACHINE "-M stc32g144k246-evb"
#define SOC "/machine/soc"
#define CPU SOC "/cpu"
#define GPIO SOC "/gpio"

#define SFR_BASE 0x01000000
#define SFR(address) (SFR_BASE + (address) - 0x80)

enum {
    IRQ_INT0,
    IRQ_TIMER0,
    IRQ_INT1,
    IRQ_TIMER1,
    IRQ_UART1,
};

static const uint8_t gpio_data_address[] = {
    0x80, 0x90, 0xa0, 0xb0, 0xc0, 0xc8, 0xe8, 0xf8,
};

static const uint8_t gpio_mode1_address[] = {
    0x93, 0x91, 0x95, 0xb1, 0xb3, 0xc9, 0xcb, 0xe1,
};

static const uint8_t gpio_mode0_address[] = {
    0x94, 0x92, 0x96, 0xb2, 0xb4, 0xca, 0xcc, 0xe2,
};

static uint8_t timer_run_mask(unsigned timer)
{
    return timer ? 0x40 : 0x10;
}

static uint8_t timer_flag_mask(unsigned timer)
{
    return timer ? 0x80 : 0x20;
}

static uint8_t timer_tl_address(unsigned timer)
{
    return timer ? 0x8b : 0x8a;
}

static uint8_t timer_th_address(unsigned timer)
{
    return timer ? 0x8d : 0x8c;
}

static unsigned timer_irq(unsigned timer)
{
    return timer ? IRQ_TIMER1 : IRQ_TIMER0;
}

static void timer_set_count(QTestState *qts, unsigned timer,
                            uint8_t high, uint8_t low)
{
    qtest_writeb(qts, SFR(timer_th_address(timer)), high);
    qtest_writeb(qts, SFR(timer_tl_address(timer)), low);
}

static void gpio_pulse_falling(QTestState *qts, unsigned pin)
{
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
}

static QTestState *uart_test_init(char *socket_path, int *socket_fd)
{
    QTestState *qts;
    int temporary_fd;

    temporary_fd = mkstemp(socket_path);
    g_assert_cmpint(temporary_fd, >=, 0);
    close(temporary_fd);

    qts = qtest_initf(
        MACHINE
        " -chardev socket,id=uart-socket,path=%s,server=on,wait=off"
        " -serial chardev:uart-socket",
        socket_path);
    *socket_fd = unix_connect(socket_path, NULL);
    g_assert_cmpint(*socket_fd, >=, 0);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    return qts;
}

static void uart_test_quit(QTestState *qts, int socket_fd,
                           const char *socket_path)
{
    close(socket_fd);
    qtest_quit(qts);
    unlink(socket_path);
}

static void test_reset_registers(void)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned port;

    g_assert_cmphex(qtest_readb(qts, SFR(0x81)), ==, 0x07);
    g_assert_cmphex(qtest_readb(qts, SFR(0x82)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x83)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x84)), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0x85)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x87)), ==, 0x30);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x89)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8a)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8b)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8c)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8d)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8e)), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8f)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x97)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xa8)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb7)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb8)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xba)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd0)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xd1)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe0)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe3)), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0xea)), ==, 0x07);
    g_assert_cmphex(qtest_readb(qts, SFR(0xeb)), ==, 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0xf0)), ==, 0x00);

    for (port = 0; port < G_N_ELEMENTS(gpio_data_address); port++) {
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_data_address[port])),
                        ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_mode1_address[port])),
                        ==, port == 3 ? 0xfc : 0xff);
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_mode0_address[port])),
                        ==, 0x00);
    }
    g_assert_cmphex(qtest_readb(qts, 0x7efea0), ==, 0x00);
    g_assert_cmphex(qtest_readb(qts, 0x7efea1), ==, 0x00);

    qtest_quit(qts);
}

static void test_memory_regions(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writeb(qts, 0x000123, 0x12);
    qtest_writeb(qts, 0x010123, 0x34);
    g_assert_cmphex(qtest_readb(qts, 0x000123), ==, 0x12);
    g_assert_cmphex(qtest_readb(qts, 0x010123), ==, 0x34);

    qtest_writeb(qts, 0x030123, 0x56);
    g_assert_cmphex(qtest_readb(qts, 0x800123), ==, 0x56);
    g_assert_cmphex(qtest_readb(qts, 0x030123), ==, 0x56);

    g_assert_cmphex(qtest_readb(qts, 0xfc2800), ==, 0x00);

    qtest_quit(qts);
}

static void test_cpu_control_registers(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writeb(qts, SFR(0x87), 0xa5);
    qtest_writeb(qts, SFR(0x8e), 0xe7);
    qtest_writeb(qts, SFR(0x8f), 0xaa);
    qtest_writeb(qts, SFR(0x97), 0xff);
    qtest_writeb(qts, SFR(0xa8), 0xff);
    qtest_writeb(qts, SFR(0xb7), 0xff);
    qtest_writeb(qts, SFR(0xb8), 0xff);
    qtest_writeb(qts, SFR(0xba), 0xff);
    qtest_writeb(qts, SFR(0xea), 0x55);
    qtest_writeb(qts, SFR(0xeb), 0xaa);

    g_assert_cmphex(qtest_readb(qts, SFR(0x87)), ==, 0xa5);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8e)), ==, 0xe7);
    g_assert_cmphex(qtest_readb(qts, SFR(0x8f)), ==, 0xaa);
    g_assert_cmphex(qtest_readb(qts, SFR(0x97)), ==, 0x40);
    g_assert_cmphex(qtest_readb(qts, SFR(0xa8)), ==, 0x9f);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb7)), ==, 0x1f);
    g_assert_cmphex(qtest_readb(qts, SFR(0xb8)), ==, 0x1f);
    g_assert_cmphex(qtest_readb(qts, SFR(0xba)), ==, 0x80);
    g_assert_cmphex(qtest_readb(qts, SFR(0xea)), ==, 0x55);
    g_assert_cmphex(qtest_readb(qts, SFR(0xeb)), ==, 0xaa);

    qtest_writeb(qts, SFR(0xe3), 0x08);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe3)), ==, 0x00);
    qtest_writeb(qts, SFR(0xae), 0xaa);
    qtest_writeb(qts, SFR(0xae), 0x55);
    qtest_writeb(qts, SFR(0xe3), 0x08);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe3)), ==, 0x08);
    qtest_writeb(qts, SFR(0xe3), 0xff);
    g_assert_cmphex(qtest_readb(qts, SFR(0xe3)), ==, 0xf9);

    qtest_quit(qts);
}

static void test_gpio_registers(void)
{
    QTestState *qts = qtest_init(MACHINE);
    unsigned port;

    for (port = 0; port < G_N_ELEMENTS(gpio_data_address); port++) {
        uint8_t value = 0x11 * (port + 1);

        qtest_writeb(qts, SFR(gpio_mode1_address[port]), 0x00);
        qtest_writeb(qts, SFR(gpio_mode0_address[port]), 0xff);
        qtest_writeb(qts, SFR(gpio_data_address[port]), value);
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_data_address[port])),
                        ==, value);
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_mode1_address[port])),
                        ==, 0x00);
        g_assert_cmphex(qtest_readb(qts, SFR(gpio_mode0_address[port])),
                        ==, 0xff);
    }

    qtest_quit(qts);
}

static void test_gpio_modes(void)
{
    QTestState *qts = qtest_init(MACHINE);
    const unsigned pin = 8;

    qtest_irq_intercept_out_named(qts, GPIO, "gpio-out");

    /* Quasi-bidirectional: a zero drives low; a one samples the pin. */
    qtest_writeb(qts, SFR(0x91), 0x00);
    qtest_writeb(qts, SFR(0x92), 0x00);
    qtest_writeb(qts, SFR(0x90), 0x01);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 0);
    g_assert_false(qtest_get_irq(qts, pin));
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 1);
    g_assert_true(qtest_get_irq(qts, pin));
    qtest_writeb(qts, SFR(0x90), 0x00);
    g_assert_false(qtest_get_irq(qts, pin));

    /* Push-pull output ignores the sampled input. */
    qtest_writeb(qts, SFR(0x91), 0x00);
    qtest_writeb(qts, SFR(0x92), 0x01);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
    qtest_writeb(qts, SFR(0x90), 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 1);
    g_assert_true(qtest_get_irq(qts, pin));

    /* High-impedance input follows the sampled input. */
    qtest_writeb(qts, SFR(0x91), 0x01);
    qtest_writeb(qts, SFR(0x92), 0x00);
    qtest_writeb(qts, SFR(0x90), 0x00);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 1);
    g_assert_true(qtest_get_irq(qts, pin));
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
    g_assert_false(qtest_get_irq(qts, pin));

    /* Open drain drives a zero and samples the pin when released. */
    qtest_writeb(qts, SFR(0x91), 0x01);
    qtest_writeb(qts, SFR(0x92), 0x01);
    qtest_writeb(qts, SFR(0x90), 0x01);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 1);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x90)) & 1, ==, 0);
    qtest_writeb(qts, SFR(0x90), 0x00);
    qtest_set_irq_in(qts, GPIO, "gpio-in", pin, 1);
    g_assert_false(qtest_get_irq(qts, pin));

    qtest_quit(qts);
}

static void test_external_interrupts(void)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_irq_intercept_in(qts, CPU);

    /* IT0=0: either edge latches IE0 and asserts the INT0 source. */
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 2, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x02, ==, 0x02);
    g_assert_true(qtest_get_irq(qts, IRQ_INT0));
    qtest_writeb(qts, SFR(0x88), 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_INT0));
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 2, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x02, ==, 0x02);
    g_assert_true(qtest_get_irq(qts, IRQ_INT0));

    /* IT0=1: rising is ignored and falling latches IE0. */
    qtest_writeb(qts, SFR(0x88), 0x01);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x02, ==, 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_INT0));
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 2, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x02, ==, 0x02);
    g_assert_true(qtest_get_irq(qts, IRQ_INT0));
    qtest_writeb(qts, SFR(0x88), 0x01);
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 2, 0);
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 2, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x02, ==, 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_INT0));

    /* INT1 has the same edge-selection behavior on P3.3. */
    qtest_writeb(qts, SFR(0x88), 0x04);
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 3, 0);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x08, ==, 0x08);
    g_assert_true(qtest_get_irq(qts, IRQ_INT1));
    qtest_writeb(qts, SFR(0x88), 0x04);
    g_assert_false(qtest_get_irq(qts, IRQ_INT1));
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 3, 1);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & 0x08, ==, 0x00);
    qtest_set_irq_in(qts, GPIO, "gpio-in", 3 * 8 + 3, 0);
    g_assert_true(qtest_get_irq(qts, IRQ_INT1));

    qtest_quit(qts);
}

static void check_timer_mode(unsigned timer, unsigned mode)
{
    QTestState *qts = qtest_init(MACHINE);
    uint8_t tmod = mode << (timer * 4);
    uint8_t flag = timer_flag_mask(timer);

    qtest_irq_intercept_in(qts, CPU);
    qtest_writeb(qts, SFR(0x89), tmod);
    if (mode == 2) {
        timer_set_count(qts, timer, 0xa5, 0xfe);
    } else {
        timer_set_count(qts, timer, 0xff, 0xfe);
    }
    qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
    qtest_clock_step(qts, 1000);

    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) & flag, ==, flag);
    g_assert_true(qtest_get_irq(qts, timer_irq(timer)));
    if (mode == 1) {
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0x00);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0x00);
    } else if (mode == 2) {
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0xa5);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xa5);
    } else {
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xfe);
    }
    qtest_writeb(qts, SFR(0x88), 0x00);
    g_assert_false(qtest_get_irq(qts, timer_irq(timer)));

    qtest_quit(qts);
}

static void test_timer_modes(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        check_timer_mode(timer, 0);
        check_timer_mode(timer, 1);
        check_timer_mode(timer, 2);
    }
    check_timer_mode(0, 3);
}

static void test_timer_reload_while_running(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        QTestState *qts = qtest_init(MACHINE);

        qtest_writeb(qts, SFR(0x89), 0x00);
        timer_set_count(qts, timer, 0xff, 0xfc);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        timer_set_count(qts, timer, 0xaa, 0xbb);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0xff);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xfc);

        qtest_clock_step(qts, 2000);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(timer),
                        ==, timer_flag_mask(timer));
        g_assert_cmphex(qtest_readb(qts, SFR(timer_th_address(timer))),
                        ==, 0xaa);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xbb);

        qtest_quit(qts);
    }
}

static void test_timer_gates(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        QTestState *qts = qtest_init(MACHINE);
        unsigned gate_pin = 3 * 8 + 2 + timer;
        uint8_t tmod = 0x09 << (timer * 4);

        qtest_set_irq_in(qts, GPIO, "gpio-in", gate_pin, 0);
        qtest_writeb(qts, SFR(0x89), tmod);
        timer_set_count(qts, timer, 0xff, 0xfe);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        qtest_clock_step(qts, 2000);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(timer), ==, 0);

        qtest_set_irq_in(qts, GPIO, "gpio-in", gate_pin, 1);
        qtest_clock_step(qts, 1000);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(timer),
                        ==, timer_flag_mask(timer));

        qtest_quit(qts);
    }
}

static void test_timer_external_counters(void)
{
    unsigned timer;

    for (timer = 0; timer < 2; timer++) {
        QTestState *qts = qtest_init(MACHINE);
        unsigned counter_pin = 3 * 8 + 4 + timer;
        uint8_t tmod = 0x05 << (timer * 4);

        qtest_writeb(qts, SFR(0x89), tmod);
        timer_set_count(qts, timer, 0xff, 0xfe);
        qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
        qtest_writeb(qts, 0x7efea0 + timer, 0x01);

        gpio_pulse_falling(qts, counter_pin);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xfe);
        gpio_pulse_falling(qts, counter_pin);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xff);

        qtest_set_irq_in(qts, GPIO, "gpio-in", counter_pin, 1);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xff);
        qtest_set_irq_in(qts, GPIO, "gpio-in", counter_pin, 0);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0xff);
        gpio_pulse_falling(qts, counter_pin);
        g_assert_cmphex(qtest_readb(qts, SFR(timer_tl_address(timer))),
                        ==, 0x00);
        g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                        timer_flag_mask(timer),
                        ==, timer_flag_mask(timer));

        qtest_quit(qts);
    }
}

static void check_timer_rate(unsigned timer, uint8_t auxr,
                             uint8_t prescaler, int64_t before_ns,
                             int64_t final_ns)
{
    QTestState *qts = qtest_init(MACHINE);

    qtest_writeb(qts, SFR(0x8e), auxr);
    qtest_writeb(qts, 0x7efea0 + timer, prescaler);
    qtest_writeb(qts, SFR(0x89), 0x01 << (timer * 4));
    timer_set_count(qts, timer, 0xff, 0xfe);
    qtest_writeb(qts, SFR(0x88), timer_run_mask(timer));
    qtest_clock_step(qts, before_ns);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                    timer_flag_mask(timer), ==, 0);
    qtest_clock_step(qts, final_ns);
    g_assert_cmphex(qtest_readb(qts, SFR(0x88)) &
                    timer_flag_mask(timer),
                    ==, timer_flag_mask(timer));

    qtest_quit(qts);
}

static void test_timer_clock_and_prescaler(void)
{
    check_timer_rate(0, 0x81, 0, 83, 1);
    check_timer_rate(1, 0x41, 0, 83, 1);
    check_timer_rate(0, 0x01, 1, 1999, 1);
    check_timer_rate(1, 0x01, 1, 1999, 1);
}

static void test_uart1_transmit(void)
{
    char socket_path[] = "stc32-uart-tx.XXXXXX";
    QTestState *qts;
    uint8_t received;
    int socket_fd;
    int ret;

    qts = uart_test_init(socket_path, &socket_fd);
    qtest_irq_intercept_in(qts, CPU);
    qtest_writeb(qts, SFR(0x98), 0xfc);
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)), ==, 0xfc);
    qtest_writeb(qts, SFR(0x98), 0x00);
    qtest_writeb(qts, SFR(0x99), 0xa5);
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x02, ==, 0x02);
    g_assert_true(qtest_get_irq(qts, IRQ_UART1));
    ret = recv(socket_fd, &received, 1, 0);
    g_assert_cmpint(ret, ==, 1);
    g_assert_cmphex(received, ==, 0xa5);

    qtest_writeb(qts, SFR(0x98), 0x00);
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x02, ==, 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_UART1));

    uart_test_quit(qts, socket_fd, socket_path);
}

static void test_uart1_receive(void)
{
    char socket_path[] = "stc32-uart-rx.XXXXXX";
    QTestState *qts;
    int socket_fd;
    int ret;

    qts = uart_test_init(socket_path, &socket_fd);
    qtest_irq_intercept_in(qts, CPU);

    /* REN gates delivery from the character backend. */
    ret = send(socket_fd, "A", 1, 0);
    g_assert_cmpint(ret, ==, 1);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x01, ==, 0);
    qtest_writeb(qts, SFR(0x98), 0x10);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x99)), ==, 'A');
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x01, ==, 1);
    g_assert_true(qtest_get_irq(qts, IRQ_UART1));

    /* RI applies backpressure and prevents an unread byte being replaced. */
    ret = send(socket_fd, "B", 1, 0);
    g_assert_cmpint(ret, ==, 1);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x99)), ==, 'A');
    qtest_writeb(qts, SFR(0x98), 0x10);
    qtest_qmp_assert_success(qts, "{'execute': 'query-status'}");
    g_assert_cmphex(qtest_readb(qts, SFR(0x99)), ==, 'B');
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x01, ==, 1);

    /* TI and RI share one level interrupt; either flag keeps it asserted. */
    qtest_writeb(qts, SFR(0x99), 0x5a);
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x03, ==, 0x03);
    qtest_writeb(qts, SFR(0x98), 0x12);
    g_assert_cmphex(qtest_readb(qts, SFR(0x98)) & 0x03, ==, 0x02);
    g_assert_true(qtest_get_irq(qts, IRQ_UART1));
    qtest_writeb(qts, SFR(0x98), 0x00);
    g_assert_false(qtest_get_irq(qts, IRQ_UART1));

    uart_test_quit(qts, socket_fd, socket_path);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/stc32/reset-registers", test_reset_registers);
    qtest_add_func("/stc32/memory-regions", test_memory_regions);
    qtest_add_func("/stc32/cpu-control-registers",
                   test_cpu_control_registers);
    qtest_add_func("/stc32/gpio/registers", test_gpio_registers);
    qtest_add_func("/stc32/gpio/modes", test_gpio_modes);
    qtest_add_func("/stc32/gpio/external-interrupts",
                   test_external_interrupts);
    qtest_add_func("/stc32/timer/modes", test_timer_modes);
    qtest_add_func("/stc32/timer/reload-while-running",
                   test_timer_reload_while_running);
    qtest_add_func("/stc32/timer/gates", test_timer_gates);
    qtest_add_func("/stc32/timer/external-counters",
                   test_timer_external_counters);
    qtest_add_func("/stc32/timer/clock-and-prescaler",
                   test_timer_clock_and_prescaler);
    qtest_add_func("/stc32/uart1/transmit", test_uart1_transmit);
    qtest_add_func("/stc32/uart1/receive", test_uart1_receive);

    return g_test_run();
}
