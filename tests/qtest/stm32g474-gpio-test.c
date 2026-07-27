/*
 * QTest for the STM32G474 GPIO ports
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "libqtest.h"
#include "migration/migration-qmp.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qemu/bitops.h"

#define STM32G474_MACHINE "stm32g474"
#define MCU_QOM_PATH "/machine/mcu"

#define GPIO_BASE 0x48000000ULL
#define GPIO_STRIDE 0x400ULL
#define GPIO_PORT_COUNT 7
#define GPIO_PIN_COUNT 16

#define GPIO_MODER 0x00
#define GPIO_OTYPER 0x04
#define GPIO_OSPEEDR 0x08
#define GPIO_PUPDR 0x0c
#define GPIO_IDR 0x10
#define GPIO_ODR 0x14
#define GPIO_BSRR 0x18
#define GPIO_LCKR 0x1c
#define GPIO_AFRL 0x20
#define GPIO_AFRH 0x24
#define GPIO_BRR 0x28
#define GPIO_G4_BOUND 0x2c

#define GPIO_LCKR_LCKK (1U << 16)

#define GPIO_MODE_INPUT 0U
#define GPIO_MODE_OUTPUT 1U
#define GPIO_MODE_AF 2U
#define GPIO_MODE_ANALOG 3U

#define GPIO_PUSH_PULL 0U
#define GPIO_OPEN_DRAIN 1U

#define GPIO_PULL_NONE 0U
#define GPIO_PULL_UP 1U
#define GPIO_PULL_DOWN 2U

#define RCC_BASE 0x40021000ULL
#define RCC_AHB2RSTR 0x2c
#define RCC_AHB2ENR 0x4c

#define CLOCK_PERIOD_1SEC (1000000000ULL << 32)

typedef struct GpioPort {
    const char *name;
    const char *child_name;
    const char *path;
    uint64_t base;
    uint32_t moder_reset;
    uint32_t ospeedr_reset;
    uint32_t pupdr_reset;
    uint32_t idr_reset;
    unsigned int index;
} GpioPort;

static const GpioPort gpio_ports[GPIO_PORT_COUNT] = {
    {
        .name = "GPIOA",
        .child_name = "gpioa",
        .path = MCU_QOM_PATH "/gpioa",
        .base = GPIO_BASE + 0 * GPIO_STRIDE,
        .moder_reset = 0xabffffff,
        .ospeedr_reset = 0x0c000000,
        .pupdr_reset = 0x64000000,
        .idr_reset = 0x0000a000,
        .index = 0,
    }, {
        .name = "GPIOB",
        .child_name = "gpiob",
        .path = MCU_QOM_PATH "/gpiob",
        .base = GPIO_BASE + 1 * GPIO_STRIDE,
        .moder_reset = 0xfffffebf,
        .ospeedr_reset = 0,
        .pupdr_reset = 0x00000100,
        .idr_reset = 0x00000010,
        .index = 1,
    }, {
        .name = "GPIOC",
        .child_name = "gpioc",
        .path = MCU_QOM_PATH "/gpioc",
        .base = GPIO_BASE + 2 * GPIO_STRIDE,
        .moder_reset = 0xffffffff,
        .ospeedr_reset = 0,
        .pupdr_reset = 0,
        .idr_reset = 0,
        .index = 2,
    }, {
        .name = "GPIOD",
        .child_name = "gpiod",
        .path = MCU_QOM_PATH "/gpiod",
        .base = GPIO_BASE + 3 * GPIO_STRIDE,
        .moder_reset = 0xffffffff,
        .ospeedr_reset = 0,
        .pupdr_reset = 0,
        .idr_reset = 0,
        .index = 3,
    }, {
        .name = "GPIOE",
        .child_name = "gpioe",
        .path = MCU_QOM_PATH "/gpioe",
        .base = GPIO_BASE + 4 * GPIO_STRIDE,
        .moder_reset = 0xffffffff,
        .ospeedr_reset = 0,
        .pupdr_reset = 0,
        .idr_reset = 0,
        .index = 4,
    }, {
        .name = "GPIOF",
        .child_name = "gpiof",
        .path = MCU_QOM_PATH "/gpiof",
        .base = GPIO_BASE + 5 * GPIO_STRIDE,
        .moder_reset = 0xffffffff,
        .ospeedr_reset = 0,
        .pupdr_reset = 0,
        .idr_reset = 0,
        .index = 5,
    }, {
        .name = "GPIOG",
        .child_name = "gpiog",
        .path = MCU_QOM_PATH "/gpiog",
        .base = GPIO_BASE + 6 * GPIO_STRIDE,
        .moder_reset = 0xffffffff,
        .ospeedr_reset = 0,
        .pupdr_reset = 0,
        .idr_reset = 0,
        .index = 6,
    },
};

static QTestState *stm32g474_qtest_start(void)
{
    return qtest_init("-M " STM32G474_MACHINE
                      " -serial null -serial null -serial null");
}

static bool qom_has_child(QTestState *qts, const char *path, const char *name)
{
    QDict *response;
    QList *properties;
    QListEntry *entry;
    bool found = false;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-list',"
                         "  'arguments': { 'path': %s } }", path);
    g_assert_false(qdict_haskey(response, "error"));
    properties = qdict_get_qlist(response, "return");
    QLIST_FOREACH_ENTRY(properties, entry) {
        QDict *property = qobject_to(QDict, qlist_entry_obj(entry));

        if (!g_strcmp0(qdict_get_str(property, "name"), name)) {
            found = true;
            break;
        }
    }
    qobject_unref(response);

    return found;
}

static uint32_t gpio_readl(QTestState *qts, const GpioPort *port,
                           uint32_t offset)
{
    return qtest_readl(qts, port->base + offset);
}

static void gpio_writel(QTestState *qts, const GpioPort *port,
                        uint32_t offset, uint32_t value)
{
    qtest_writel(qts, port->base + offset, value);
}

static uint32_t rcc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, RCC_BASE + offset);
}

static void rcc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, RCC_BASE + offset, value);
}

static uint32_t replace_field(uint32_t value, unsigned int shift,
                              unsigned int width, uint32_t field)
{
    uint32_t mask = MAKE_64BIT_MASK(shift, width);

    return (value & ~mask) | ((field << shift) & mask);
}

static void gpio_set_mode(QTestState *qts, const GpioPort *port,
                          unsigned int pin, uint32_t mode)
{
    uint32_t value = gpio_readl(qts, port, GPIO_MODER);

    gpio_writel(qts, port, GPIO_MODER,
                replace_field(value, pin * 2, 2, mode));
}

static void gpio_set_type(QTestState *qts, const GpioPort *port,
                          unsigned int pin, uint32_t type)
{
    uint32_t value = gpio_readl(qts, port, GPIO_OTYPER);

    gpio_writel(qts, port, GPIO_OTYPER,
                replace_field(value, pin, 1, type));
}

static void gpio_set_pull(QTestState *qts, const GpioPort *port,
                          unsigned int pin, uint32_t pull)
{
    uint32_t value = gpio_readl(qts, port, GPIO_PUPDR);

    gpio_writel(qts, port, GPIO_PUPDR,
                replace_field(value, pin * 2, 2, pull));
}

static void gpio_set_external(QTestState *qts, const GpioPort *port,
                              unsigned int pin, int level)
{
    qtest_set_irq_in(qts, port->path, "pin-in", pin, level);
}

static bool gpio_idr_level(QTestState *qts, const GpioPort *port,
                           unsigned int pin)
{
    return extract32(gpio_readl(qts, port, GPIO_IDR), pin, 1);
}

static void assert_pin_level(QTestState *qts, const GpioPort *port,
                             unsigned int pin, bool expected)
{
    g_test_message("%s pin %u", port->name, pin);
    g_assert_cmpint(gpio_idr_level(qts, port, pin), ==, expected);
    g_assert_cmpint(qtest_get_irq(qts, pin), ==, expected);
}

static uint64_t clock_period_from_hz(uint64_t hz)
{
    return hz ? CLOCK_PERIOD_1SEC / hz : 0;
}

static uint64_t gpio_clock_period(QTestState *qts, const GpioPort *port)
{
    g_autofree char *path = g_strdup_printf("%s/clk", port->path);
    QDict *response;
    uint64_t period;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-get', 'arguments': {"
                         "  'path': %s,"
                         "  'property': 'qtest-clock-period' } }", path);
    g_assert_false(qdict_haskey(response, "error"));
    period = qdict_get_int(response, "return");
    qobject_unref(response);

    return period;
}

static void assert_gpio_clock_hz(QTestState *qts, const GpioPort *port,
                                 uint64_t hz)
{
    g_test_message("%s clock", port->name);
    g_assert_cmphex(gpio_clock_period(qts, port), ==,
                    clock_period_from_hz(hz));
}

static void assert_port_reset_image(QTestState *qts, const GpioPort *port,
                                    uint32_t expected_idr)
{
    g_assert_cmphex(gpio_readl(qts, port, GPIO_MODER), ==,
                    port->moder_reset);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_OTYPER), ==, 0);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_OSPEEDR), ==,
                    port->ospeedr_reset);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_PUPDR), ==,
                    port->pupdr_reset);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_IDR), ==, expected_idr);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_BSRR), ==, 0);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_LCKR), ==, 0);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRL), ==, 0);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRH), ==, 0);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_BRR), ==, 0);
}

static void assert_intercepted_outputs_match_idr(QTestState *qts,
                                                  const GpioPort *port)
{
    uint32_t idr = gpio_readl(qts, port, GPIO_IDR);

    for (unsigned int pin = 0; pin < GPIO_PIN_COUNT; pin++) {
        g_assert_cmpint(qtest_get_irq(qts, pin), ==,
                        extract32(idr, pin, 1));
    }
}

static void complete_lock(QTestState *qts, const GpioPort *port,
                          uint16_t mask)
{
    uint32_t value = mask;

    gpio_writel(qts, port, GPIO_LCKR, value | GPIO_LCKR_LCKK);
    gpio_writel(qts, port, GPIO_LCKR, value);
    gpio_writel(qts, port, GPIO_LCKR, value | GPIO_LCKR_LCKK);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_LCKR), ==,
                    value | GPIO_LCKR_LCKK);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_LCKR), ==,
                    value | GPIO_LCKR_LCKK);
}

static uint32_t expand_pin_mask(uint16_t pins, unsigned int width,
                                unsigned int first_pin,
                                unsigned int pin_count)
{
    uint32_t mask = 0;

    for (unsigned int pin = first_pin;
         pin < first_pin + pin_count; pin++) {
        if (pins & (1U << pin)) {
            mask |= MAKE_64BIT_MASK((pin - first_pin) * width, width);
        }
    }

    return mask;
}

static void test_topology_reset_masks(const void *opaque)
{
    const GpioPort *port = opaque;
    QTestState *qts = stm32g474_qtest_start();
    uint32_t idr;

    g_assert_true(qom_has_child(qts, MCU_QOM_PATH, port->child_name));
    g_assert_false(qom_has_child(qts, MCU_QOM_PATH, "gpioh"));

    qtest_irq_intercept_out_named(qts, port->path, "pin-out");
    qtest_system_reset(qts);
    assert_port_reset_image(qts, port, port->idr_reset);
    assert_intercepted_outputs_match_idr(qts, port);

    gpio_writel(qts, port, GPIO_MODER, UINT32_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_MODER), ==, UINT32_MAX);
    gpio_writel(qts, port, GPIO_OTYPER, UINT32_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_OTYPER), ==, 0x0000ffff);
    gpio_writel(qts, port, GPIO_OSPEEDR, UINT32_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_OSPEEDR), ==, UINT32_MAX);
    gpio_writel(qts, port, GPIO_PUPDR, UINT32_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_PUPDR), ==, UINT32_MAX);
    gpio_writel(qts, port, GPIO_ODR, UINT32_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0x0000ffff);
    gpio_writel(qts, port, GPIO_AFRL, UINT32_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRL), ==, UINT32_MAX);
    gpio_writel(qts, port, GPIO_AFRH, UINT32_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRH), ==, UINT32_MAX);

    idr = gpio_readl(qts, port, GPIO_IDR);
    gpio_writel(qts, port, GPIO_IDR, UINT32_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_IDR), ==, idr);

    gpio_writel(qts, port, GPIO_BSRR, UINT32_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_BSRR), ==, 0);
    gpio_writel(qts, port, GPIO_BRR, UINT32_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_BRR), ==, 0);

    gpio_writel(qts, port, GPIO_AFRH, 0x5aa55aa5);
    gpio_writel(qts, port, GPIO_G4_BOUND, UINT32_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_G4_BOUND), ==, 0);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRH), ==, 0x5aa55aa5);

    for (unsigned int reset = 0; reset < 2; reset++) {
        qtest_system_reset(qts);
        assert_port_reset_image(qts, port, port->idr_reset);
        assert_intercepted_outputs_match_idr(qts, port);
    }

    qtest_quit(qts);
}

static void test_lane_adapter(void)
{
    const GpioPort *port = &gpio_ports[3];
    QTestState *qts = stm32g474_qtest_start();

    gpio_writel(qts, port, GPIO_AFRL, 0x11223344);
    qtest_writeb(qts, port->base + GPIO_AFRL + 0, 0xaa);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRL), ==, 0x112233aa);
    qtest_writeb(qts, port->base + GPIO_AFRL + 1, 0xbb);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRL), ==, 0x1122bbaa);
    qtest_writeb(qts, port->base + GPIO_AFRL + 2, 0xcc);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRL), ==, 0x11ccbbaa);
    qtest_writeb(qts, port->base + GPIO_AFRL + 3, 0xdd);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRL), ==, 0xddccbbaa);
    g_assert_cmphex(qtest_readb(qts, port->base + GPIO_AFRL + 0), ==, 0xaa);
    g_assert_cmphex(qtest_readb(qts, port->base + GPIO_AFRL + 1), ==, 0xbb);
    g_assert_cmphex(qtest_readb(qts, port->base + GPIO_AFRL + 2), ==, 0xcc);
    g_assert_cmphex(qtest_readb(qts, port->base + GPIO_AFRL + 3), ==, 0xdd);

    gpio_writel(qts, port, GPIO_AFRL, 0x89abcdef);
    qtest_writew(qts, port->base + GPIO_AFRL, 0x1357);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRL), ==, 0x89ab1357);
    qtest_writew(qts, port->base + GPIO_AFRL + 2, 0x2468);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRL), ==, 0x24681357);
    g_assert_cmphex(qtest_readw(qts, port->base + GPIO_AFRL), ==, 0x1357);
    g_assert_cmphex(qtest_readw(qts, port->base + GPIO_AFRL + 2), ==, 0x2468);

    gpio_writel(qts, port, GPIO_AFRL, 0xdeadbeef);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRL), ==, 0xdeadbeef);

    gpio_writel(qts, port, GPIO_OTYPER, 0x1234);
    qtest_writew(qts, port->base + GPIO_OTYPER + 2, UINT16_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_OTYPER), ==, 0x1234);
    g_assert_cmphex(qtest_readw(qts, port->base + GPIO_OTYPER + 2), ==, 0);
    qtest_writew(qts, port->base + GPIO_IDR + 2, UINT16_MAX);
    g_assert_cmphex(qtest_readw(qts, port->base + GPIO_IDR + 2), ==, 0);
    gpio_writel(qts, port, GPIO_ODR, 0x5678);
    qtest_writew(qts, port->base + GPIO_ODR + 2, UINT16_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0x5678);
    g_assert_cmphex(qtest_readw(qts, port->base + GPIO_ODR + 2), ==, 0);
    qtest_writew(qts, port->base + GPIO_BRR + 2, UINT16_MAX);
    g_assert_cmphex(qtest_readw(qts, port->base + GPIO_BRR + 2), ==, 0);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0x5678);

    gpio_writel(qts, port, GPIO_AFRH, 0xcafebabe);
    gpio_writel(qts, port, GPIO_G4_BOUND, 0x0badf00d);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_G4_BOUND), ==, 0);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRH), ==, 0xcafebabe);

    gpio_writel(qts, port, GPIO_ODR, 0);
    qtest_writeb(qts, port->base + GPIO_BSRR + 0, 0x81);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0x0081);
    qtest_writeb(qts, port->base + GPIO_BSRR + 1, 0x42);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0x4281);
    qtest_writeb(qts, port->base + GPIO_BSRR + 2, 0x81);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0x4200);
    qtest_writeb(qts, port->base + GPIO_BSRR + 3, 0x42);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0);
    for (unsigned int lane = 0; lane < 4; lane++) {
        g_assert_cmphex(qtest_readb(qts, port->base + GPIO_BSRR + lane),
                        ==, 0);
    }

    qtest_writew(qts, port->base + GPIO_BSRR, 0xa55a);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0xa55a);
    qtest_writew(qts, port->base + GPIO_BSRR + 2, 0x0f0f);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0xa050);
    g_assert_cmphex(qtest_readw(qts, port->base + GPIO_BSRR), ==, 0);
    g_assert_cmphex(qtest_readw(qts, port->base + GPIO_BSRR + 2), ==, 0);

    gpio_writel(qts, port, GPIO_ODR, 0xffff);
    qtest_writeb(qts, port->base + GPIO_BRR, 0x81);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0xff7e);
    qtest_writeb(qts, port->base + GPIO_BRR + 1, 0x42);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0xbd7e);
    qtest_writeb(qts, port->base + GPIO_BRR + 2, UINT8_MAX);
    qtest_writeb(qts, port->base + GPIO_BRR + 3, UINT8_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0xbd7e);
    gpio_writel(qts, port, GPIO_ODR, 0xa55a);
    qtest_writew(qts, port->base + GPIO_BRR, 0x0f0f);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0xa050);
    g_assert_cmphex(qtest_readw(qts, port->base + GPIO_BRR), ==, 0);
    for (unsigned int lane = 0; lane < 4; lane++) {
        g_assert_cmphex(qtest_readb(qts, port->base + GPIO_BRR + lane),
                        ==, 0);
    }

    qtest_quit(qts);
}

static void test_pad_resolution(void)
{
    const GpioPort *port = &gpio_ports[2];
    QTestState *qts = stm32g474_qtest_start();
    uint32_t odr;

    qtest_irq_intercept_out_named(qts, port->path, "pin-out");
    qtest_system_reset(qts);

    gpio_set_mode(qts, port, 0, GPIO_MODE_OUTPUT);
    gpio_writel(qts, port, GPIO_ODR, 0);
    assert_pin_level(qts, port, 0, false);
    gpio_writel(qts, port, GPIO_ODR, 1U << 0);
    assert_pin_level(qts, port, 0, true);
    gpio_writel(qts, port, GPIO_ODR, 0);
    assert_pin_level(qts, port, 0, false);

    gpio_set_mode(qts, port, 1, GPIO_MODE_INPUT);
    gpio_writel(qts, port, GPIO_ODR, 1U << 1);
    assert_pin_level(qts, port, 1, false);
    gpio_set_mode(qts, port, 1, GPIO_MODE_OUTPUT);
    assert_pin_level(qts, port, 1, true);

    gpio_set_mode(qts, port, 2, GPIO_MODE_OUTPUT);
    gpio_writel(qts, port, GPIO_BSRR, 1U << 2);
    assert_pin_level(qts, port, 2, true);
    gpio_writel(qts, port, GPIO_BSRR, 1U << (2 + 16));
    assert_pin_level(qts, port, 2, false);
    gpio_writel(qts, port, GPIO_BSRR, 1U << 2);
    gpio_writel(qts, port, GPIO_BRR, 1U << 2);
    assert_pin_level(qts, port, 2, false);

    gpio_set_mode(qts, port, 3, GPIO_MODE_OUTPUT);
    gpio_writel(qts, port, GPIO_ODR, 0);
    gpio_writel(qts, port, GPIO_BSRR,
                (1U << 3) | (1U << (3 + 16)));
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 1U << 3);
    assert_pin_level(qts, port, 3, true);

    gpio_writel(qts, port, GPIO_ODR, 0x00f0);
    gpio_writel(qts, port, GPIO_BSRR, 0x0c03 | (0x00fcU << 16));
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0x0c03);

    gpio_set_mode(qts, port, 4, GPIO_MODE_INPUT);
    gpio_set_pull(qts, port, 4, GPIO_PULL_NONE);
    gpio_set_external(qts, port, 4, 0);
    assert_pin_level(qts, port, 4, false);
    gpio_set_external(qts, port, 4, 1);
    assert_pin_level(qts, port, 4, true);
    gpio_set_external(qts, port, 4, -1);
    assert_pin_level(qts, port, 4, false);
    gpio_set_pull(qts, port, 4, GPIO_PULL_UP);
    assert_pin_level(qts, port, 4, true);
    gpio_set_pull(qts, port, 4, GPIO_PULL_DOWN);
    assert_pin_level(qts, port, 4, false);
    gpio_set_pull(qts, port, 4, GPIO_PULL_NONE);
    assert_pin_level(qts, port, 4, false);

    gpio_set_mode(qts, port, 8, GPIO_MODE_INPUT);
    gpio_set_external(qts, port, 8, -1);
    gpio_set_pull(qts, port, 8, 3);
    g_assert_cmphex(extract32(gpio_readl(qts, port, GPIO_PUPDR), 8 * 2, 2),
                    ==, 3);
    assert_pin_level(qts, port, 8, false);

    gpio_set_external(qts, port, 5, 1);
    gpio_set_mode(qts, port, 5, GPIO_MODE_OUTPUT);
    gpio_set_type(qts, port, 5, GPIO_PUSH_PULL);
    odr = gpio_readl(qts, port, GPIO_ODR) & ~(1U << 5);
    gpio_writel(qts, port, GPIO_ODR, odr);
    assert_pin_level(qts, port, 5, false);
    gpio_writel(qts, port, GPIO_ODR, odr | (1U << 5));
    assert_pin_level(qts, port, 5, true);
    gpio_set_external(qts, port, 5, 0);
    assert_pin_level(qts, port, 5, true);
    gpio_set_mode(qts, port, 5, GPIO_MODE_INPUT);
    assert_pin_level(qts, port, 5, false);

    gpio_set_mode(qts, port, 6, GPIO_MODE_OUTPUT);
    gpio_set_type(qts, port, 6, GPIO_OPEN_DRAIN);
    gpio_set_external(qts, port, 6, 1);
    odr = gpio_readl(qts, port, GPIO_ODR) & ~(1U << 6);
    gpio_writel(qts, port, GPIO_ODR, odr);
    assert_pin_level(qts, port, 6, false);
    gpio_writel(qts, port, GPIO_ODR, odr | (1U << 6));
    assert_pin_level(qts, port, 6, true);
    gpio_set_external(qts, port, 6, 0);
    assert_pin_level(qts, port, 6, false);
    gpio_set_type(qts, port, 6, GPIO_PUSH_PULL);
    assert_pin_level(qts, port, 6, true);
    gpio_set_type(qts, port, 6, GPIO_OPEN_DRAIN);
    assert_pin_level(qts, port, 6, false);
    gpio_set_external(qts, port, 6, -1);
    gpio_set_pull(qts, port, 6, GPIO_PULL_UP);
    assert_pin_level(qts, port, 6, true);
    gpio_set_pull(qts, port, 6, GPIO_PULL_DOWN);
    assert_pin_level(qts, port, 6, false);
    gpio_set_pull(qts, port, 6, GPIO_PULL_NONE);
    assert_pin_level(qts, port, 6, false);

    gpio_writel(qts, port, GPIO_ODR,
                gpio_readl(qts, port, GPIO_ODR) & ~(1U << 7));
    gpio_set_external(qts, port, 7, 1);
    gpio_set_mode(qts, port, 7, GPIO_MODE_AF);
    assert_pin_level(qts, port, 7, true);
    gpio_set_mode(qts, port, 7, GPIO_MODE_ANALOG);
    assert_pin_level(qts, port, 7, true);
    gpio_set_external(qts, port, 7, 0);
    assert_pin_level(qts, port, 7, false);

    qtest_quit(qts);
}

static void assert_locked_register(QTestState *qts, const GpioPort *port,
                                   uint32_t offset, uint32_t locked_fields,
                                   uint32_t old_value, uint32_t new_value)
{
    uint32_t expected = (new_value & ~locked_fields) |
                        (old_value & locked_fields);

    gpio_writel(qts, port, offset, new_value);
    g_assert_cmphex(gpio_readl(qts, port, offset), ==, expected);
    g_assert_cmphex(gpio_readl(qts, port, offset) & ~locked_fields, ==,
                    new_value & ~locked_fields);
}

static void test_complete_lock(void)
{
    const GpioPort *port = &gpio_ports[3];
    const uint16_t locked_pins = (1U << 2) | (1U << 10);
    const uint32_t two_bit_fields =
        expand_pin_mask(locked_pins, 2, 0, GPIO_PIN_COUNT);
    const uint32_t one_bit_fields =
        expand_pin_mask(locked_pins, 1, 0, GPIO_PIN_COUNT);
    const uint32_t afrl_fields = expand_pin_mask(locked_pins, 4, 0, 8);
    const uint32_t afrh_fields = expand_pin_mask(locked_pins, 4, 8, 8);
    const uint32_t moder = 0x55555555;
    const uint32_t otyper = 0x00005555;
    const uint32_t ospeedr = 0xa5a5a5a5;
    const uint32_t pupdr = 0x5a5a5a5a;
    const uint32_t afrl = 0x12345678;
    const uint32_t afrh = 0x89abcdef;
    QTestState *qts = stm32g474_qtest_start();

    gpio_writel(qts, port, GPIO_MODER, moder);
    gpio_writel(qts, port, GPIO_OTYPER, otyper);
    gpio_writel(qts, port, GPIO_OSPEEDR, ospeedr);
    gpio_writel(qts, port, GPIO_PUPDR, pupdr);
    gpio_writel(qts, port, GPIO_AFRL, afrl);
    gpio_writel(qts, port, GPIO_AFRH, afrh);
    complete_lock(qts, port, locked_pins);

    assert_locked_register(qts, port, GPIO_MODER, two_bit_fields,
                           moder, 0xaaaaaaaa);
    assert_locked_register(qts, port, GPIO_OTYPER, one_bit_fields,
                           otyper, 0x0000aaaa);
    assert_locked_register(qts, port, GPIO_OSPEEDR, two_bit_fields,
                           ospeedr, 0x3c3c3c3c);
    assert_locked_register(qts, port, GPIO_PUPDR, two_bit_fields,
                           pupdr, 0xc3c3c3c3);
    assert_locked_register(qts, port, GPIO_AFRL, afrl_fields,
                           afrl, 0xfedcba98);
    assert_locked_register(qts, port, GPIO_AFRH, afrh_fields,
                           afrh, 0x76543210);

    gpio_writel(qts, port, GPIO_ODR, locked_pins);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR) & locked_pins, ==,
                    locked_pins);
    gpio_writel(qts, port, GPIO_BSRR,
                (uint32_t)locked_pins << 16);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR) & locked_pins, ==, 0);
    gpio_writel(qts, port, GPIO_BSRR, locked_pins);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR) & locked_pins, ==,
                    locked_pins);
    gpio_writel(qts, port, GPIO_BRR, locked_pins);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR) & locked_pins, ==, 0);

    gpio_writel(qts, port, GPIO_LCKR, 0);
    qtest_writeb(qts, port->base + GPIO_LCKR, 0);
    qtest_writew(qts, port->base + GPIO_LCKR, 0);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_LCKR), ==,
                    locked_pins | GPIO_LCKR_LCKK);
    assert_locked_register(qts, port, GPIO_MODER, two_bit_fields,
                           gpio_readl(qts, port, GPIO_MODER), UINT32_MAX);

    qtest_quit(qts);
}

static void test_zero_mask_lock(void)
{
    const GpioPort *port = &gpio_ports[3];
    QTestState *qts = stm32g474_qtest_start();

    gpio_writel(qts, port, GPIO_MODER, 0x55555555);
    complete_lock(qts, port, 0);
    gpio_writel(qts, port, GPIO_MODER, 0xaaaaaaaa);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_MODER), ==, 0xaaaaaaaa);
    gpio_writel(qts, port, GPIO_LCKR, UINT32_MAX);
    qtest_writeb(qts, port->base + GPIO_LCKR, UINT8_MAX);
    qtest_writew(qts, port->base + GPIO_LCKR, UINT16_MAX);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_LCKR), ==, GPIO_LCKR_LCKK);
    gpio_writel(qts, port, GPIO_MODER, 0x13579bdf);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_MODER), ==, 0x13579bdf);

    qtest_quit(qts);
}

typedef enum MalformedLockKind {
    MALFORMED_CHANGED_MASK,
    MALFORMED_WRONG_SECOND_KEY,
    MALFORMED_WRONG_THIRD_KEY,
    MALFORMED_READ_EARLY,
    MALFORMED_EXTRA_WRITE,
    MALFORMED_SUBWORD,
} MalformedLockKind;

typedef struct MalformedLockCase {
    const char *name;
    MalformedLockKind kind;
    unsigned int phase;
    unsigned int size;
    bool is_write;
} MalformedLockCase;

static const MalformedLockCase malformed_lock_cases[] = {
    { "changed-mask", MALFORMED_CHANGED_MASK },
    { "wrong-second-key", MALFORMED_WRONG_SECOND_KEY },
    { "wrong-third-key", MALFORMED_WRONG_THIRD_KEY },
    { "read-too-early", MALFORMED_READ_EARLY },
    { "extra-write-after-arming", MALFORMED_EXTRA_WRITE },
    { "byte-read-after-key1", MALFORMED_SUBWORD, 1, 1, false },
    { "byte-write-after-key1", MALFORMED_SUBWORD, 1, 1, true },
    { "byte-read-after-key0", MALFORMED_SUBWORD, 2, 1, false },
    { "byte-write-after-key0", MALFORMED_SUBWORD, 2, 1, true },
    { "byte-read-after-armed", MALFORMED_SUBWORD, 3, 1, false },
    { "byte-write-after-armed", MALFORMED_SUBWORD, 3, 1, true },
    { "halfword-read-after-key1", MALFORMED_SUBWORD, 1, 2, false },
    { "halfword-write-after-key1", MALFORMED_SUBWORD, 1, 2, true },
    { "halfword-read-after-key0", MALFORMED_SUBWORD, 2, 2, false },
    { "halfword-write-after-key0", MALFORMED_SUBWORD, 2, 2, true },
    { "halfword-read-after-armed", MALFORMED_SUBWORD, 3, 2, false },
    { "halfword-write-after-armed", MALFORMED_SUBWORD, 3, 2, true },
};

static void write_lock_prefix(QTestState *qts, const GpioPort *port,
                              uint16_t mask, unsigned int phase)
{
    if (phase >= 1) {
        gpio_writel(qts, port, GPIO_LCKR, mask | GPIO_LCKR_LCKK);
    }
    if (phase >= 2) {
        gpio_writel(qts, port, GPIO_LCKR, mask);
    }
    if (phase >= 3) {
        gpio_writel(qts, port, GPIO_LCKR, mask | GPIO_LCKR_LCKK);
    }
}

static void test_malformed_lock(const void *opaque)
{
    const MalformedLockCase *test = opaque;
    const GpioPort *port = &gpio_ports[3];
    const uint16_t mask = 1U << 5;
    const uint32_t field = 3U << (5 * 2);
    QTestState *qts = stm32g474_qtest_start();
    uint32_t value;

    switch (test->kind) {
    case MALFORMED_CHANGED_MASK:
        gpio_writel(qts, port, GPIO_LCKR, mask | GPIO_LCKR_LCKK);
        gpio_writel(qts, port, GPIO_LCKR, mask | (1U << 6));
        break;
    case MALFORMED_WRONG_SECOND_KEY:
        gpio_writel(qts, port, GPIO_LCKR, mask | GPIO_LCKR_LCKK);
        gpio_writel(qts, port, GPIO_LCKR, mask | GPIO_LCKR_LCKK);
        break;
    case MALFORMED_WRONG_THIRD_KEY:
        gpio_writel(qts, port, GPIO_LCKR, mask | GPIO_LCKR_LCKK);
        gpio_writel(qts, port, GPIO_LCKR, mask);
        gpio_writel(qts, port, GPIO_LCKR, mask);
        break;
    case MALFORMED_READ_EARLY:
        gpio_writel(qts, port, GPIO_LCKR, mask | GPIO_LCKR_LCKK);
        gpio_readl(qts, port, GPIO_LCKR);
        break;
    case MALFORMED_EXTRA_WRITE:
        write_lock_prefix(qts, port, mask, 3);
        gpio_writel(qts, port, GPIO_LCKR, mask | GPIO_LCKR_LCKK);
        break;
    case MALFORMED_SUBWORD:
        write_lock_prefix(qts, port, mask, test->phase);
        if (test->size == 1) {
            if (test->is_write) {
                qtest_writeb(qts, port->base + GPIO_LCKR, mask);
            } else {
                qtest_readb(qts, port->base + GPIO_LCKR);
            }
        } else if (test->is_write) {
            qtest_writew(qts, port->base + GPIO_LCKR, mask);
        } else {
            qtest_readw(qts, port->base + GPIO_LCKR);
        }
        break;
    default:
        g_assert_not_reached();
    }

    value = gpio_readl(qts, port, GPIO_MODER);
    gpio_writel(qts, port, GPIO_MODER, value ^ field);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_MODER) & field, ==,
                    (value ^ field) & field);

    complete_lock(qts, port, mask);
    value = gpio_readl(qts, port, GPIO_MODER);
    gpio_writel(qts, port, GPIO_MODER, value ^ field);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_MODER) & field, ==,
                    value & field);

    qtest_quit(qts);
}

static void test_lock_reset_clear(void)
{
    const GpioPort *port = &gpio_ports[3];
    const uint16_t mask = 1U << 8;
    const uint32_t field = 3U << (8 * 2);
    QTestState *qts = stm32g474_qtest_start();
    uint32_t value;

    complete_lock(qts, port, mask);
    value = gpio_readl(qts, port, GPIO_MODER);
    gpio_writel(qts, port, GPIO_MODER, value ^ field);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_MODER) & field, ==,
                    value & field);

    rcc_writel(qts, RCC_AHB2RSTR, 1U << port->index);
    rcc_writel(qts, RCC_AHB2RSTR, 0);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_LCKR), ==, 0);
    value = gpio_readl(qts, port, GPIO_MODER);
    gpio_writel(qts, port, GPIO_MODER, value ^ field);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_MODER) & field, ==,
                    (value ^ field) & field);

    complete_lock(qts, port, mask);
    qtest_system_reset(qts);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_LCKR), ==, 0);
    value = gpio_readl(qts, port, GPIO_MODER);
    gpio_writel(qts, port, GPIO_MODER, value ^ field);
    g_assert_cmphex(gpio_readl(qts, port, GPIO_MODER) & field, ==,
                    (value ^ field) & field);

    qtest_quit(qts);
}

static void test_rcc_clock_reset_coupling(void)
{
    QTestState *qts = stm32g474_qtest_start();

    for (unsigned int i = 0; i < GPIO_PORT_COUNT; i++) {
        assert_gpio_clock_hz(qts, &gpio_ports[i], 0);
    }

    for (unsigned int selected = 0; selected < GPIO_PORT_COUNT; selected++) {
        rcc_writel(qts, RCC_AHB2ENR, 1U << selected);
        for (unsigned int i = 0; i < GPIO_PORT_COUNT; i++) {
            assert_gpio_clock_hz(qts, &gpio_ports[i],
                                 i == selected ? 16000000 : 0);
        }
        gpio_writel(qts, &gpio_ports[selected], GPIO_AFRL,
                    0x12340000 | selected);
        g_assert_cmphex(gpio_readl(qts, &gpio_ports[selected], GPIO_AFRL),
                        ==, 0x12340000 | selected);
        rcc_writel(qts, RCC_AHB2ENR, 0);
        assert_gpio_clock_hz(qts, &gpio_ports[selected], 0);
        gpio_writel(qts, &gpio_ports[selected], GPIO_AFRL,
                    0x56780000 | selected);
        g_assert_cmphex(gpio_readl(qts, &gpio_ports[selected], GPIO_AFRL),
                        ==, 0x56780000 | selected);
    }

    for (unsigned int selected = 0; selected < GPIO_PORT_COUNT; selected++) {
        const GpioPort *port = &gpio_ports[selected];
        const GpioPort *other = &gpio_ports[(selected + 1) % GPIO_PORT_COUNT];
        uint32_t other_afrl = 0xa5000000 | selected;

        rcc_writel(qts, RCC_AHB2RSTR, 0);
        gpio_writel(qts, other, GPIO_AFRL, other_afrl);
        gpio_writel(qts, other, GPIO_ODR, 0x8000 | selected);

        gpio_writel(qts, port, GPIO_MODER, 0x55555555);
        gpio_writel(qts, port, GPIO_OTYPER, 0x00005a5a);
        gpio_writel(qts, port, GPIO_OSPEEDR, 0xaaaaaaaa);
        gpio_writel(qts, port, GPIO_PUPDR, 0x55555555);
        gpio_writel(qts, port, GPIO_AFRL, 0xdead0000 | selected);
        gpio_writel(qts, port, GPIO_AFRH, 0xbeef0000 | selected);
        gpio_writel(qts, port, GPIO_ODR, 0xffff);
        g_assert_cmphex(gpio_readl(qts, port, GPIO_MODER), ==, 0x55555555);
        g_assert_cmphex(gpio_readl(qts, port, GPIO_OTYPER), ==, 0x00005a5a);
        g_assert_cmphex(gpio_readl(qts, port, GPIO_OSPEEDR), ==, 0xaaaaaaaa);
        g_assert_cmphex(gpio_readl(qts, port, GPIO_PUPDR), ==, 0x55555555);
        g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRL), ==,
                        0xdead0000 | selected);
        g_assert_cmphex(gpio_readl(qts, port, GPIO_AFRH), ==,
                        0xbeef0000 | selected);
        g_assert_cmphex(gpio_readl(qts, port, GPIO_ODR), ==, 0xffff);
        complete_lock(qts, port, 1U << 3);
        gpio_set_external(qts, port, 0, 1);

        rcc_writel(qts, RCC_AHB2RSTR, 1U << selected);
        assert_port_reset_image(qts, port, port->idr_reset | 1U);
        g_assert_cmphex(gpio_readl(qts, other, GPIO_AFRL), ==, other_afrl);
        g_assert_cmphex(gpio_readl(qts, other, GPIO_ODR), ==,
                        0x8000 | selected);

        gpio_writel(qts, port, GPIO_MODER, 0x01234567);
        gpio_writel(qts, port, GPIO_OTYPER, 0x00001357);
        gpio_writel(qts, port, GPIO_OSPEEDR, 0x89abcdef);
        gpio_writel(qts, port, GPIO_PUPDR, 0x13579bdf);
        gpio_writel(qts, port, GPIO_AFRL, 0x2468ace0);
        gpio_writel(qts, port, GPIO_AFRH, 0x0f1e2d3c);
        gpio_writel(qts, port, GPIO_ODR, 0x0000a55a);
        gpio_writel(qts, port, GPIO_LCKR,
                    (1U << 7) | GPIO_LCKR_LCKK);
        gpio_writel(qts, port, GPIO_LCKR, 1U << 7);
        gpio_writel(qts, port, GPIO_LCKR,
                    (1U << 7) | GPIO_LCKR_LCKK);
        g_assert_cmphex(gpio_readl(qts, port, GPIO_LCKR), ==, 0);
        gpio_writel(qts, port, GPIO_BSRR, UINT32_MAX);
        assert_port_reset_image(qts, port, port->idr_reset | 1U);
        gpio_writel(qts, port, GPIO_BRR, UINT32_MAX);
        assert_port_reset_image(qts, port, port->idr_reset | 1U);

        gpio_set_external(qts, port, 0, 0);
        g_assert_cmphex(gpio_readl(qts, port, GPIO_IDR), ==,
                        port->idr_reset & ~1U);
        gpio_set_external(qts, port, 0, 1);
        g_assert_cmphex(gpio_readl(qts, port, GPIO_IDR), ==,
                        port->idr_reset | 1U);

        rcc_writel(qts, RCC_AHB2RSTR, 0);
        gpio_set_mode(qts, port, 0, GPIO_MODE_OUTPUT);
        gpio_writel(qts, port, GPIO_ODR, 1);
        g_assert_true(gpio_idr_level(qts, port, 0));
        g_assert_cmphex(gpio_readl(qts, other, GPIO_AFRL), ==, other_afrl);
        g_assert_cmphex(gpio_readl(qts, other, GPIO_ODR), ==,
                        0x8000 | selected);
    }

    qtest_quit(qts);
}

static void test_cold_reset(void)
{
    const GpioPort *port = &gpio_ports[3];
    QTestState *qts = stm32g474_qtest_start();
    uint32_t expected_idr = 1U << 5;

    qtest_irq_intercept_out_named(qts, port->path, "pin-out");
    qtest_system_reset(qts);
    gpio_set_external(qts, port, 5, 1);
    gpio_set_external(qts, port, 6, 0);
    gpio_set_mode(qts, port, 1, GPIO_MODE_OUTPUT);
    gpio_writel(qts, port, GPIO_ODR, 1U << 1);
    assert_pin_level(qts, port, 1, true);
    complete_lock(qts, port, 1U << 3);
    gpio_writel(qts, port, GPIO_AFRL, 0x12345678);

    qtest_system_reset(qts);
    assert_port_reset_image(qts, port, expected_idr);
    assert_intercepted_outputs_match_idr(qts, port);
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_true(qtest_get_irq(qts, 5));

    gpio_writel(qts, port, GPIO_MODER,
                port->moder_reset ^ (3U << (3 * 2)));
    g_assert_cmphex(gpio_readl(qts, port, GPIO_MODER), ==,
                    port->moder_reset ^ (3U << (3 * 2)));
    gpio_writel(qts, port, GPIO_BSRR, UINT32_MAX);
    gpio_writel(qts, port, GPIO_BRR, UINT32_MAX);

    qtest_system_reset(qts);
    assert_port_reset_image(qts, port, expected_idr);
    assert_intercepted_outputs_match_idr(qts, port);
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_true(qtest_get_irq(qts, 5));

    qtest_quit(qts);
}

static void configure_migration_source(QTestState *qts)
{
    const GpioPort *gpioa = &gpio_ports[0];
    const GpioPort *gpiob = &gpio_ports[1];
    const GpioPort *gpioc = &gpio_ports[2];
    const GpioPort *gpiod = &gpio_ports[3];
    const GpioPort *gpioe = &gpio_ports[4];
    const GpioPort *gpiof = &gpio_ports[5];
    const uint16_t d_lock_mask = 1U << 4;
    const uint16_t f_lock_mask = 1U << 5;

    rcc_writel(qts, RCC_AHB2ENR, (1U << 0) | (1U << 2) | (1U << 6));

    gpio_writel(qts, gpioa, GPIO_AFRL, 0x12345678);
    gpio_writel(qts, gpioa, GPIO_OSPEEDR, 0x89abcdef);
    gpio_set_mode(qts, gpioa, 0, GPIO_MODE_OUTPUT);
    gpio_writel(qts, gpioa, GPIO_ODR, 1);

    gpio_set_mode(qts, gpiob, 0, GPIO_MODE_INPUT);
    gpio_set_mode(qts, gpiob, 1, GPIO_MODE_INPUT);
    gpio_set_mode(qts, gpiob, 2, GPIO_MODE_INPUT);
    gpio_set_pull(qts, gpiob, 0, GPIO_PULL_NONE);
    gpio_set_pull(qts, gpiob, 1, GPIO_PULL_NONE);
    gpio_set_pull(qts, gpiob, 2, GPIO_PULL_UP);
    gpio_set_external(qts, gpiob, 0, 1);
    gpio_set_external(qts, gpiob, 1, 0);
    gpio_set_external(qts, gpiob, 2, -1);

    gpio_writel(qts, gpioc, GPIO_MODER, 0x55555555);
    complete_lock(qts, gpioc, 1U << 3);

    gpio_writel(qts, gpiod, GPIO_AFRL, 0x0d0d0d0d);
    gpio_writel(qts, gpiod, GPIO_LCKR,
                d_lock_mask | GPIO_LCKR_LCKK);
    gpio_writel(qts, gpiod, GPIO_LCKR, d_lock_mask);

    gpio_writel(qts, gpioe, GPIO_AFRL, 0xe5e5e5e5);
    gpio_writel(qts, gpioe, GPIO_ODR, 0xe5e5);
    rcc_writel(qts, RCC_AHB2RSTR, 1U << gpioe->index);
    assert_port_reset_image(qts, gpioe, gpioe->idr_reset);

    gpio_writel(qts, gpiof, GPIO_LCKR,
                f_lock_mask | GPIO_LCKR_LCKK);
}

static void assert_migrated_state(QTestState *qts)
{
    const GpioPort *gpioa = &gpio_ports[0];
    const GpioPort *gpiob = &gpio_ports[1];
    const GpioPort *gpioc = &gpio_ports[2];
    const GpioPort *gpiod = &gpio_ports[3];
    const GpioPort *gpioe = &gpio_ports[4];
    const GpioPort *gpiof = &gpio_ports[5];
    const uint16_t c_lock_mask = 1U << 3;
    const uint16_t d_lock_mask = 1U << 4;
    const uint16_t f_lock_mask = 1U << 5;
    const uint32_t d_lock_field = 3U << (4 * 2);
    const uint32_t f_lock_field = 3U << (5 * 2);
    uint32_t value;

    g_assert_cmphex(rcc_readl(qts, RCC_AHB2ENR) & 0x7f, ==, 0x45);
    g_assert_cmphex(rcc_readl(qts, RCC_AHB2RSTR) & 0x7f, ==,
                    1U << gpioe->index);
    for (unsigned int i = 0; i < GPIO_PORT_COUNT; i++) {
        bool enabled = i == 0 || i == 2 || i == 6;

        assert_gpio_clock_hz(qts, &gpio_ports[i],
                             enabled ? 16000000 : 0);
    }

    g_assert_cmphex(gpio_readl(qts, gpioa, GPIO_AFRL), ==, 0x12345678);
    g_assert_cmphex(gpio_readl(qts, gpioa, GPIO_OSPEEDR), ==, 0x89abcdef);
    g_assert_cmphex(gpio_readl(qts, gpioa, GPIO_ODR), ==, 1);
    g_assert_cmphex(gpio_readl(qts, gpioa, GPIO_IDR), ==, 0x0000a001);

    g_assert_cmphex(gpio_readl(qts, gpiob, GPIO_IDR), ==, 0x00000015);
    assert_intercepted_outputs_match_idr(qts, gpiob);
    gpio_set_pull(qts, gpiob, 0, GPIO_PULL_DOWN);
    gpio_set_pull(qts, gpiob, 1, GPIO_PULL_UP);
    gpio_set_pull(qts, gpiob, 2, GPIO_PULL_DOWN);
    g_assert_true(gpio_idr_level(qts, gpiob, 0));
    g_assert_false(gpio_idr_level(qts, gpiob, 1));
    g_assert_false(gpio_idr_level(qts, gpiob, 2));
    g_assert_cmphex(gpio_readl(qts, gpiob, GPIO_BSRR), ==, 0);
    g_assert_cmphex(gpio_readl(qts, gpiob, GPIO_BRR), ==, 0);

    value = gpio_readl(qts, gpioc, GPIO_MODER);
    gpio_writel(qts, gpioc, GPIO_MODER, value ^ (3U << (3 * 2)));
    g_assert_cmphex(gpio_readl(qts, gpioc, GPIO_MODER) &
                    (3U << (3 * 2)), ==, value & (3U << (3 * 2)));
    g_assert_cmphex(gpio_readl(qts, gpioc, GPIO_LCKR), ==,
                    c_lock_mask | GPIO_LCKR_LCKK);

    value = gpio_readl(qts, gpiod, GPIO_MODER);
    gpio_writel(qts, gpiod, GPIO_MODER, value ^ d_lock_field);
    g_assert_cmphex(gpio_readl(qts, gpiod, GPIO_MODER) & d_lock_field, ==,
                    (value ^ d_lock_field) & d_lock_field);
    gpio_writel(qts, gpiod, GPIO_LCKR,
                d_lock_mask | GPIO_LCKR_LCKK);
    g_assert_cmphex(gpio_readl(qts, gpiod, GPIO_LCKR), ==,
                    d_lock_mask | GPIO_LCKR_LCKK);
    value = gpio_readl(qts, gpiod, GPIO_MODER);
    gpio_writel(qts, gpiod, GPIO_MODER, value ^ d_lock_field);
    g_assert_cmphex(gpio_readl(qts, gpiod, GPIO_MODER) & d_lock_field, ==,
                    value & d_lock_field);

    gpio_writel(qts, gpiof, GPIO_LCKR,
                f_lock_mask | GPIO_LCKR_LCKK);
    value = gpio_readl(qts, gpiof, GPIO_MODER);
    gpio_writel(qts, gpiof, GPIO_MODER, value ^ f_lock_field);
    g_assert_cmphex(gpio_readl(qts, gpiof, GPIO_MODER) & f_lock_field, ==,
                    (value ^ f_lock_field) & f_lock_field);
    complete_lock(qts, gpiof, f_lock_mask);
    value = gpio_readl(qts, gpiof, GPIO_MODER);
    gpio_writel(qts, gpiof, GPIO_MODER, value ^ f_lock_field);
    g_assert_cmphex(gpio_readl(qts, gpiof, GPIO_MODER) & f_lock_field, ==,
                    value & f_lock_field);

    assert_port_reset_image(qts, gpioe, gpioe->idr_reset);
    gpio_writel(qts, gpioe, GPIO_AFRL, UINT32_MAX);
    gpio_writel(qts, gpioe, GPIO_BSRR, UINT32_MAX);
    assert_port_reset_image(qts, gpioe, gpioe->idr_reset);
    rcc_writel(qts, RCC_AHB2RSTR, 0);
    gpio_writel(qts, gpioe, GPIO_AFRL, 0x87654321);
    g_assert_cmphex(gpio_readl(qts, gpioe, GPIO_AFRL), ==, 0x87654321);
}

static void test_idle_migration(void)
{
    QTestState *src;
    QTestState *dst;
    g_autofree char *tmpdir =
        g_dir_make_tmp("stm32g474-gpio-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    g_assert_nonnull(tmpdir);
    src = stm32g474_qtest_start();
    dst = qtest_init("-M " STM32G474_MACHINE
                     " -serial null -serial null -serial null"
                     " -incoming defer");
    qtest_irq_intercept_out_named(dst, gpio_ports[1].path, "pin-out");

    configure_migration_source(src);
    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    assert_migrated_state(dst);

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    for (unsigned int i = 0; i < GPIO_PORT_COUNT; i++) {
        g_autofree char *path =
            g_strdup_printf("/stm32g474/gpio/1-topology-reset-masks/%c",
                            'a' + i);

        qtest_add_data_func(path, &gpio_ports[i], test_topology_reset_masks);
    }
    qtest_add_func("/stm32g474/gpio/2-lane-adapter",
                   test_lane_adapter);
    qtest_add_func("/stm32g474/gpio/3-pad-resolution",
                   test_pad_resolution);
    qtest_add_func("/stm32g474/gpio/4-lock/complete",
                   test_complete_lock);
    qtest_add_func("/stm32g474/gpio/4-lock/zero-mask",
                   test_zero_mask_lock);
    for (unsigned int i = 0; i < ARRAY_SIZE(malformed_lock_cases); i++) {
        g_autofree char *path =
            g_strdup_printf("/stm32g474/gpio/4-lock/malformed/%s",
                            malformed_lock_cases[i].name);

        qtest_add_data_func(path, &malformed_lock_cases[i],
                            test_malformed_lock);
    }
    qtest_add_func("/stm32g474/gpio/4-lock/reset-clear",
                   test_lock_reset_clear);
    qtest_add_func("/stm32g474/gpio/5-rcc-clock-reset-coupling",
                   test_rcc_clock_reset_coupling);
    qtest_add_func("/stm32g474/gpio/6-cold-reset",
                   test_cold_reset);
    qtest_add_func("/stm32g474/gpio/7-idle-migration",
                   test_idle_migration);

    return g_test_run();
}
