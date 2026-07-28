/*
 * QTest testcase for the Mercedes-Benz ARDEP V2 machine
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qapi/error.h"
#include "libqtest.h"
#include "migration/migration-qmp.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"

#define ARDEP_V2_MACHINE "ardep-v2"
#define STM32G474_MACHINE "stm32g474"

#define MCU_QOM_PATH "/machine/mcu"
#define RCC_QOM_PATH MCU_QOM_PATH "/rcc"
#define FDCAN_QOM_PATH MCU_QOM_PATH "/fdcan"

#define FLASH_BASE 0x08000000ULL
#define FLASH_SIZE (512 * KiB)

#define RCC_BASE 0x40021000ULL
#define RCC_CR 0x00
#define RCC_CFGR 0x08
#define RCC_PLLCFGR 0x0c
#define RCC_AHB2ENR 0x4c
#define RCC_APB1ENR1 0x58
#define RCC_APB2ENR 0x60

#define RCC_CR_HSION BIT(8)
#define RCC_CR_HSIRDY BIT(10)
#define RCC_CR_HSEON BIT(16)
#define RCC_CR_HSERDY BIT(17)
#define RCC_CR_HSEBYP BIT(18)
#define RCC_CR_PLLON BIT(24)
#define RCC_CR_PLLRDY BIT(25)

#define RCC_CFGR_SW_MASK (3U << 0)
#define RCC_CFGR_SW_PLL (3U << 0)
#define RCC_CFGR_SWS_MASK (3U << 2)
#define RCC_CFGR_SWS_HSI16 (1U << 2)
#define RCC_CFGR_SWS_PLL (3U << 2)

#define RCC_PLLCFGR_ARDEP 0x01105533U
#define RCC_AHB2_GPIOEN(port) BIT(port)
#define RCC_APB1_UART4EN BIT(19)
#define RCC_APB2_SYSCFGEN BIT(0)

#define GPIO_BASE 0x48000000ULL
#define GPIO_STRIDE 0x400
#define GPIO_MODER 0x00
#define GPIO_IDR 0x10
#define GPIO_ODR 0x14
#define GPIO_BSRR 0x18
#define GPIO_MODE_OUTPUT 1U

#define SYSCFG_BASE 0x40010000ULL
#define SYSCFG_EXTICR1 0x08

#define EXTI_BASE 0x40010400ULL
#define EXTI_IMR1 0x00
#define EXTI_FTSR1 0x0c
#define EXTI_PR1 0x14

#define NVIC_ISER0 0xe000e100ULL
#define NVIC_ISPR0 0xe000e200ULL
#define NVIC_ICPR0 0xe000e280ULL
#define EXTI3_IRQ 9

#define UART4_BASE 0x40004c00ULL
#define USART_CR1 0x00
#define USART_CR2 0x04
#define USART_BRR 0x0c
#define USART_ISR 0x1c
#define USART_TDR 0x28
#define USART_PRESC 0x2c
#define USART_CR1_UE BIT(0)
#define USART_CR1_TE BIT(3)

#define INITIAL_MSP 0x20020000U
#define RESET_VECTOR 0x08000101U
#define SLOT0_MSP 0x20018000U
#define SLOT0_VECTOR 0x08018301U
#define CLOCK_PERIOD_1SEC (1000000000ULL << 32)
#define WAIT_TIMEOUT_US (5 * G_USEC_PER_SEC)
#define OVERSIZE_IMAGE_ENV "QTEST_ARDEP_V2_OVERSIZE_IMAGE"

typedef struct LedCase {
    const char *name;
    const char *color;
    const char *led_path;
    const char *splitter_path;
    uint64_t gpio_base;
    unsigned int port;
} LedCase;

static const LedCase led_cases[] = {
    {
        .name = "green",
        .color = "green",
        .led_path = "/machine/green-led",
        .splitter_path = "/machine/green-led-splitter",
        .gpio_base = GPIO_BASE,
        .port = 0,
    }, {
        .name = "red",
        .color = "red",
        .led_path = "/machine/red-led",
        .splitter_path = "/machine/red-led-splitter",
        .gpio_base = GPIO_BASE + 2 * GPIO_STRIDE,
        .port = 2,
    },
};

typedef struct FlashMarker {
    uint32_t offset;
    uint32_t value;
} FlashMarker;

static const FlashMarker flash_markers[] = {
    { 0x00000, INITIAL_MSP },
    { 0x17ffc, 0x17ffc001 },
    { 0x18000, 0x01800001 },
    { 0x18200, SLOT0_MSP },
    { 0x47ffc, 0x047ffc01 },
    { 0x48000, 0x04800001 },
    { 0x77ffc, 0x077ffc01 },
    { 0x78000, 0x07800001 },
    { 0x7fffc, 0x07fffc01 },
};

static QTestState *ardep_v2_start(void)
{
    return qtest_init("-M " ARDEP_V2_MACHINE
                      " -serial null -serial null -serial null");
}

static QTestState *ardep_v2_start_incoming(void)
{
    return qtest_init("-M " ARDEP_V2_MACHINE
                      " -serial null -serial null -serial null"
                      " -incoming defer");
}

static QTestState *stm32g474_start(void)
{
    return qtest_init("-M " STM32G474_MACHINE
                      " -serial null -serial null -serial null");
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

    g_error("QOM child %s/%s is missing", parent, name);
}

static char *qom_get_string(QTestState *qts, const char *path,
                            const char *property)
{
    QDict *response;
    char *value;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-get', 'arguments': {"
                         "  'path': %s, 'property': %s } }",
                         path, property);
    g_assert_false(qdict_haskey(response, "error"));
    value = g_strdup(qdict_get_str(response, "return"));
    qobject_unref(response);

    return value;
}

static void assert_qom_property_missing(QTestState *qts, const char *path,
                                        const char *property)
{
    QDict *response;
    QDict *error;
    const char *description;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-get', 'arguments': {"
                         "  'path': %s, 'property': %s } }",
                         path, property);
    g_assert_false(qdict_haskey(response, "return"));
    g_assert_true(qdict_haskey(response, "error"));
    error = qdict_get_qdict(response, "error");
    g_assert_cmpstr(qdict_get_str(error, "class"), ==, "GenericError");
    description = qdict_get_str(error, "desc");
    g_assert_nonnull(strstr(description, property));
    g_assert_nonnull(strstr(description, "not found"));
    qobject_unref(response);
}

static void assert_qom_link(QTestState *qts, const char *path,
                            const char *property, const char *expected)
{
    g_autofree char *value = qom_get_string(qts, path, property);

    g_assert_cmpstr(value, ==, expected);
}

static void assert_board_can_links(QTestState *qts, const char *canbus0,
                                   const char *canbus1)
{
    static const char *const paths[] = {
        "/machine",
        MCU_QOM_PATH,
        FDCAN_QOM_PATH,
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(paths); i++) {
        assert_qom_link(qts, paths[i], "canbus0", canbus0);
        assert_qom_link(qts, paths[i], "canbus1", canbus1);
    }
}

static uint64_t qom_get_uint(QTestState *qts, const char *path,
                             const char *property)
{
    QDict *response;
    uint64_t value;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-get', 'arguments': {"
                         "  'path': %s, 'property': %s } }",
                         path, property);
    g_assert_false(qdict_haskey(response, "error"));
    value = qdict_get_int(response, "return");
    qobject_unref(response);

    return value;
}

static uint64_t clock_period_from_hz(uint64_t hz)
{
    return hz ? CLOCK_PERIOD_1SEC / hz : 0;
}

static void assert_clock_hz(QTestState *qts, const char *path, uint64_t hz)
{
    g_assert_cmphex(qom_get_uint(qts, path, "qtest-clock-period"), ==,
                    clock_period_from_hz(hz));
}

static uint32_t rcc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, RCC_BASE + offset);
}

static void rcc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, RCC_BASE + offset, value);
}

static void assert_core_clocks(QTestState *qts, uint64_t hz,
                               uint64_t refclk_hz)
{
    static const char *const clocks[] = {
        "sysclk", "hclk", "pclk1", "pclk2",
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(clocks); i++) {
        g_autofree char *path =
            g_strdup_printf(RCC_QOM_PATH "/%s", clocks[i]);

        assert_clock_hz(qts, path, hz);
    }
    assert_clock_hz(qts, RCC_QOM_PATH "/cortex-refclk", refclk_hz);
}

static void assert_hsi16_reset_clocks(QTestState *qts)
{
    g_assert_cmphex(rcc_readl(qts, RCC_CR) &
                    (RCC_CR_HSION | RCC_CR_HSIRDY),
                    ==, RCC_CR_HSION | RCC_CR_HSIRDY);
    g_assert_cmphex(rcc_readl(qts, RCC_CFGR) &
                    (RCC_CFGR_SW_MASK | RCC_CFGR_SWS_MASK),
                    ==, 1U | RCC_CFGR_SWS_HSI16);
    assert_core_clocks(qts, 16000000, 2000000);
}

static void configure_ardep_pll(QTestState *qts)
{
    uint32_t cr;
    uint32_t cfgr;

    cr = rcc_readl(qts, RCC_CR);
    rcc_writel(qts, RCC_CR, cr | RCC_CR_HSEBYP);
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) | RCC_CR_HSEON);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) &
                    (RCC_CR_HSEBYP | RCC_CR_HSEON | RCC_CR_HSERDY),
                    ==, RCC_CR_HSEBYP | RCC_CR_HSEON | RCC_CR_HSERDY);

    rcc_writel(qts, RCC_PLLCFGR, RCC_PLLCFGR_ARDEP);
    g_assert_cmphex(rcc_readl(qts, RCC_PLLCFGR), ==,
                    RCC_PLLCFGR_ARDEP);
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) | RCC_CR_PLLON);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) &
                    (RCC_CR_PLLON | RCC_CR_PLLRDY),
                    ==, RCC_CR_PLLON | RCC_CR_PLLRDY);
    assert_clock_hz(qts, RCC_QOM_PATH "/pll-r", 170000000);

    cfgr = (rcc_readl(qts, RCC_CFGR) & ~RCC_CFGR_SW_MASK) |
           RCC_CFGR_SW_PLL;
    rcc_writel(qts, RCC_CFGR, cfgr);
    g_assert_cmphex(rcc_readl(qts, RCC_CFGR) &
                    (RCC_CFGR_SW_MASK | RCC_CFGR_SWS_MASK),
                    ==, RCC_CFGR_SW_PLL | RCC_CFGR_SWS_PLL);
    assert_core_clocks(qts, 170000000, 21250000);
}

static uint32_t gpio_readl(QTestState *qts, const LedCase *led,
                           uint32_t offset)
{
    return qtest_readl(qts, led->gpio_base + offset);
}

static void gpio_writel(QTestState *qts, const LedCase *led,
                        uint32_t offset, uint32_t value)
{
    qtest_writel(qts, led->gpio_base + offset, value);
}

static void gpio_set_output_high(QTestState *qts, const LedCase *led)
{
    const unsigned int pin = 3;
    uint32_t moder;

    rcc_writel(qts, RCC_AHB2ENR,
               rcc_readl(qts, RCC_AHB2ENR) |
               RCC_AHB2_GPIOEN(led->port));
    gpio_writel(qts, led, GPIO_BSRR, BIT(pin));
    g_assert_cmphex(gpio_readl(qts, led, GPIO_ODR) & BIT(pin), ==,
                    BIT(pin));
    moder = gpio_readl(qts, led, GPIO_MODER);
    moder = deposit32(moder, pin * 2, 2, GPIO_MODE_OUTPUT);
    gpio_writel(qts, led, GPIO_MODER, moder);
    g_assert_cmphex(extract32(gpio_readl(qts, led, GPIO_MODER),
                              pin * 2, 2),
                    ==, GPIO_MODE_OUTPUT);
    g_assert_cmphex(gpio_readl(qts, led, GPIO_IDR) & BIT(pin), ==,
                    BIT(pin));
}

static void gpio_set_level(QTestState *qts, const LedCase *led, bool high)
{
    gpio_writel(qts, led, GPIO_BSRR, BIT(high ? 3 : 19));
    g_assert_cmpint(!!(gpio_readl(qts, led, GPIO_IDR) & BIT(3)), ==,
                    high);
}

static void syscfg_select_exti3(QTestState *qts, unsigned int port)
{
    uint32_t value = qtest_readl(qts, SYSCFG_BASE + SYSCFG_EXTICR1);

    value = deposit32(value, 12, 3, port);
    qtest_writel(qts, SYSCFG_BASE + SYSCFG_EXTICR1, value);
}

static bool nvic_exti3_pending(QTestState *qts)
{
    return qtest_readl(qts, NVIC_ISPR0) & BIT(EXTI3_IRQ);
}

static void configure_led_exti3(QTestState *qts, const LedCase *led)
{
    const uint32_t bit = BIT(3);

    rcc_writel(qts, RCC_APB2ENR,
               rcc_readl(qts, RCC_APB2ENR) | RCC_APB2_SYSCFGEN);
    syscfg_select_exti3(qts, led->port);
    gpio_set_output_high(qts, led);
    qtest_writel(qts, EXTI_BASE + EXTI_FTSR1, bit);
    qtest_writel(qts, EXTI_BASE + EXTI_IMR1, bit);
    qtest_writel(qts, EXTI_BASE + EXTI_PR1, bit);
    qtest_writel(qts, NVIC_ISER0, BIT(EXTI3_IRQ));
    qtest_writel(qts, NVIC_ICPR0, BIT(EXTI3_IRQ));
    g_assert_cmphex(qtest_readl(qts, EXTI_BASE + EXTI_PR1), ==, 0);
    g_assert_false(nvic_exti3_pending(qts));
}

static void assert_led_properties(QTestState *qts, const LedCase *led)
{
    g_autofree char *color =
        qom_get_string(qts, led->led_path, "color");

    g_assert_false(qtest_qom_get_bool(qts, led->led_path,
                                      "gpio-active-high"));
    g_assert_cmpstr(color, ==, led->color);
}

static void test_machine_topology(void)
{
    QTestState *qts;

    g_assert_true(qtest_has_machine(ARDEP_V2_MACHINE));
    g_assert_true(qtest_has_machine(STM32G474_MACHINE));
    qts = ardep_v2_start();

    assert_qom_child(qts, "/machine", "mcu", "stm32g474");
    assert_qom_child(qts, "/machine", "hse", "clock");
    assert_qom_child(qts, "/machine", "red-led", "led");
    assert_qom_child(qts, "/machine", "green-led", "led");
    assert_qom_child(qts, "/machine", "red-led-splitter", "split-irq");
    assert_qom_child(qts, "/machine", "green-led-splitter", "split-irq");

    assert_qom_child(qts, MCU_QOM_PATH, "flash", "stm32g474-flash");
    assert_qom_child(qts, MCU_QOM_PATH, "stm32g474.sram1[0]",
                     "memory-region");
    assert_qom_child(qts, MCU_QOM_PATH, "rcc", "stm32g474-rcc");
    assert_qom_child(qts, MCU_QOM_PATH, "gpioa", "stm32g474-gpio-a");
    assert_qom_child(qts, MCU_QOM_PATH, "syscfg", "stm32g474-syscfg");
    assert_qom_child(qts, MCU_QOM_PATH, "exti", "stm32g474-exti");
    assert_qom_child(qts, MCU_QOM_PATH, "uart4", "stm32g474-uart");

    g_assert_cmpuint(qom_get_uint(qts, "/machine/red-led-splitter",
                                  "num-lines"), ==, 2);
    g_assert_cmpuint(qom_get_uint(qts, "/machine/green-led-splitter",
                                  "num-lines"), ==, 2);
    for (unsigned int i = 0; i < ARRAY_SIZE(led_cases); i++) {
        assert_led_properties(qts, &led_cases[i]);
    }

    qtest_quit(qts);
}

static void test_can_links_no_backend(void)
{
    QTestState *qts = ardep_v2_start();

    assert_board_can_links(qts, "", "");
    assert_qom_property_missing(qts, "/machine", "canbus2");
    assert_qom_link(qts, MCU_QOM_PATH, "canbus2", "");
    assert_qom_link(qts, FDCAN_QOM_PATH, "canbus2", "");

    qtest_quit(qts);
}

static void test_can_links_shared(void)
{
    QTestState *qts =
        qtest_init("-M " ARDEP_V2_MACHINE
                   " -object can-bus,id=qcan"
                   " -machine canbus0=qcan,canbus1=qcan"
                   " -serial null -serial null -serial null");

    assert_board_can_links(qts, "/objects/qcan", "/objects/qcan");
    assert_qom_link(qts, MCU_QOM_PATH, "canbus2", "");
    assert_qom_link(qts, FDCAN_QOM_PATH, "canbus2", "");

    qtest_quit(qts);
}

static void test_can_links_independent(void)
{
    QTestState *qts =
        qtest_init("-M " ARDEP_V2_MACHINE
                   " -object can-bus,id=qcan0"
                   " -object can-bus,id=qcan1"
                   " -machine canbus0=qcan0,canbus1=qcan1"
                   " -serial null -serial null -serial null");

    assert_board_can_links(qts, "/objects/qcan0", "/objects/qcan1");
    assert_qom_link(qts, MCU_QOM_PATH, "canbus2", "");
    assert_qom_link(qts, FDCAN_QOM_PATH, "canbus2", "");

    qtest_quit(qts);
}

static void test_hse_pll_and_reset(void)
{
    QTestState *qts = ardep_v2_start();

    assert_clock_hz(qts, "/machine/hse", 16000000);
    assert_hsi16_reset_clocks(qts);
    configure_ardep_pll(qts);

    for (unsigned int reset = 0; reset < 2; reset++) {
        qtest_system_reset(qts);
        assert_clock_hz(qts, "/machine/hse", 16000000);
        g_assert_cmphex(rcc_readl(qts, RCC_CR) &
                        (RCC_CR_HSEBYP | RCC_CR_HSEON |
                         RCC_CR_HSERDY | RCC_CR_PLLON | RCC_CR_PLLRDY),
                        ==, RCC_CR_HSEBYP);
        assert_hsi16_reset_clocks(qts);
        configure_ardep_pll(qts);
    }
    qtest_quit(qts);

    qts = stm32g474_start();
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) | RCC_CR_HSEBYP);
    rcc_writel(qts, RCC_CR, rcc_readl(qts, RCC_CR) | RCC_CR_HSEON);
    g_assert_cmphex(rcc_readl(qts, RCC_CR) &
                    (RCC_CR_HSEBYP | RCC_CR_HSEON | RCC_CR_HSERDY),
                    ==, RCC_CR_HSEBYP | RCC_CR_HSEON);
    qtest_quit(qts);
}

static void test_led_levels(const void *opaque)
{
    const LedCase *led = opaque;
    QTestState *qts = ardep_v2_start();

    assert_led_properties(qts, led);
    qtest_irq_intercept_out(qts, led->splitter_path);
    qtest_system_reset(qts);

    gpio_set_output_high(qts, led);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    gpio_set_level(qts, led, false);
    g_assert_false(qtest_get_irq(qts, 0));
    g_assert_false(qtest_get_irq(qts, 1));

    gpio_set_level(qts, led, true);
    g_assert_true(qtest_get_irq(qts, 0));
    g_assert_true(qtest_get_irq(qts, 1));

    qtest_quit(qts);
}

static void test_led_exti_fanout(const void *opaque)
{
    const LedCase *led = opaque;
    const uint32_t bit = BIT(3);
    QTestState *qts = ardep_v2_start();

    qtest_system_reset(qts);
    configure_led_exti3(qts, led);

    gpio_set_level(qts, led, false);
    g_assert_cmphex(qtest_readl(qts, EXTI_BASE + EXTI_PR1), ==, bit);
    g_assert_true(nvic_exti3_pending(qts));

    qtest_writel(qts, EXTI_BASE + EXTI_PR1, bit);
    qtest_writel(qts, NVIC_ICPR0, BIT(EXTI3_IRQ));
    g_assert_cmphex(qtest_readl(qts, EXTI_BASE + EXTI_PR1), ==, 0);
    g_assert_false(nvic_exti3_pending(qts));

    gpio_set_level(qts, led, true);
    g_assert_cmphex(qtest_readl(qts, EXTI_BASE + EXTI_PR1), ==, 0);
    g_assert_false(nvic_exti3_pending(qts));

    qtest_quit(qts);
}

static uint8_t receive_serial_byte(QTestState *qts, int sock_fd)
{
    gint64 deadline = g_get_monotonic_time() + WAIT_TIMEOUT_US;
    uint8_t byte;

    do {
        ssize_t ret = recv(sock_fd, &byte, sizeof(byte), 0);

        if (ret == 1) {
            return byte;
        }
        if (ret == 0 || (errno != EAGAIN && errno != EWOULDBLOCK &&
                         errno != EINTR)) {
            g_error("serial0 receive failed: %s",
                    ret == 0 ? "closed" : g_strerror(errno));
        }
        qtest_readl(qts, UART4_BASE + USART_ISR);
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    g_error("serial0 receive timed out");
}

static void test_serial0_ordering(void)
{
    g_autofree char *uart4_chardev = NULL;
    g_autofree char *usart2_chardev = NULL;
    g_autofree char *usart1_chardev = NULL;
    QTestState *qts;
    int sock_fd;

    qts = qtest_init_with_serial("-M " ARDEP_V2_MACHINE, &sock_fd);
    qemu_set_blocking(sock_fd, false, &error_abort);
    uart4_chardev = qom_get_string(qts, MCU_QOM_PATH "/uart4", "chardev");
    usart2_chardev = qom_get_string(qts, MCU_QOM_PATH "/usart2", "chardev");
    usart1_chardev = qom_get_string(qts, MCU_QOM_PATH "/usart1", "chardev");
    g_assert_cmpstr(uart4_chardev, ==, "s0");
    g_assert_cmpstr(usart2_chardev, ==, "");
    g_assert_cmpstr(usart1_chardev, ==, "");

    rcc_writel(qts, RCC_APB1ENR1,
               rcc_readl(qts, RCC_APB1ENR1) | RCC_APB1_UART4EN);
    qtest_writel(qts, UART4_BASE + USART_CR2, 0);
    qtest_writel(qts, UART4_BASE + USART_BRR, 0x8b);
    qtest_writel(qts, UART4_BASE + USART_PRESC, 0);
    qtest_writel(qts, UART4_BASE + USART_CR1,
                 USART_CR1_UE | USART_CR1_TE);
    qtest_writel(qts, UART4_BASE + USART_TDR, 0xa6);
    g_assert_cmphex(receive_serial_byte(qts, sock_fd), ==, 0xa6);

    close(sock_fd);
    qtest_quit(qts);
}

static void cleanup_temp_file(void *opaque)
{
    char *path = opaque;

    qtest_remove_abrt_handler(path);
    g_unlink(path);
    g_free(path);
}

static char *write_temp_image(const uint8_t *image, size_t size,
                              const char *template)
{
    g_autoptr(GError) error = NULL;
    char *path = g_strdup(template);
    int fd;

    fd = g_mkstemp(path);
    g_assert_cmpint(fd, >=, 0);
    qtest_add_abrt_handler(cleanup_temp_file, path);
    g_test_queue_destroy(cleanup_temp_file, path);
    g_assert_cmpint(close(fd), ==, 0);
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      size, &error));
    g_assert_no_error(error);

    return path;
}

static void assert_boot_registers(QTestState *qts)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers");

    g_assert_nonnull(strstr(registers, "R13=20020000"));
    g_assert_nonnull(strstr(registers, "R15=08000100"));
}

static void test_raw_flash_layout(void)
{
    g_autofree uint8_t *image = g_malloc0(FLASH_SIZE);
    char *image_path;
    QTestState *qts;

    for (unsigned int i = 0; i < ARRAY_SIZE(flash_markers); i++) {
        stl_le_p(image + flash_markers[i].offset,
                 flash_markers[i].value);
    }
    stl_le_p(image + 4, RESET_VECTOR);
    stw_le_p(image + 0x100, 0xe7fe);
    stl_le_p(image + 0x18204, SLOT0_VECTOR);

    image_path = write_temp_image(image, FLASH_SIZE,
                                  "ardep-v2-flash-XXXXXX");
    qts = qtest_initf("-M " ARDEP_V2_MACHINE
                      " -serial null -serial null -serial null"
                      " -kernel %s", image_path);

    for (unsigned int i = 0; i < ARRAY_SIZE(flash_markers); i++) {
        g_assert_cmphex(qtest_readl(qts, flash_markers[i].offset),
                        ==, flash_markers[i].value);
        g_assert_cmphex(qtest_readl(qts, FLASH_BASE +
                                    flash_markers[i].offset),
                        ==, flash_markers[i].value);
    }
    g_assert_cmphex(qtest_readl(qts, 4), ==, RESET_VECTOR);
    g_assert_cmphex(qtest_readl(qts, FLASH_BASE + 4), ==, RESET_VECTOR);
    g_assert_cmphex(qtest_readl(qts, 0x18204), ==, SLOT0_VECTOR);
    g_assert_cmphex(qtest_readl(qts, FLASH_BASE + 0x18204), ==,
                    SLOT0_VECTOR);
    assert_boot_registers(qts);

    for (unsigned int reset = 0; reset < 2; reset++) {
        qtest_system_reset(qts);
        assert_boot_registers(qts);
    }

    qtest_quit(qts);
}

static void test_raw_oversize_rejected(void)
{
    g_autofree uint8_t *image = NULL;
    char *image_path;

    if (g_test_subprocess()) {
        const char *child_image_path = g_getenv(OVERSIZE_IMAGE_ENV);
        QTestState *qts;

        g_assert_nonnull(child_image_path);
        qts = qtest_initf("-M " ARDEP_V2_MACHINE
                          " -serial null -serial null -serial null"
                          " -kernel %s", child_image_path);
        qtest_quit(qts);
        g_error("oversized raw image was accepted");
    }

    image = g_malloc0(FLASH_SIZE + 1);
    image_path = write_temp_image(image, FLASH_SIZE + 1,
                                  "ardep-v2-oversize-XXXXXX");
    g_assert_true(g_setenv(OVERSIZE_IMAGE_ENV, image_path, true));
    g_test_trap_subprocess(NULL, WAIT_TIMEOUT_US, 0);
    g_unsetenv(OVERSIZE_IMAGE_ENV);
    g_assert_false(g_test_trap_reached_timeout());
    g_test_trap_assert_failed();
    g_test_trap_assert_stderr("*Could not load kernel*");
}

static void test_active_migration(void)
{
    const LedCase *led = &led_cases[0];
    const uint32_t exti3 = BIT(3);
    QTestState *src;
    QTestState *dst;
    g_autofree char *tmpdir =
        g_dir_make_tmp("ardep-v2-migration-XXXXXX", NULL);
    g_autofree char *sock = g_strdup_printf("%s/migration.sock", tmpdir);
    g_autofree char *uri = g_strdup_printf("unix:%s", sock);

    g_assert_nonnull(tmpdir);
    src = ardep_v2_start();
    dst = ardep_v2_start_incoming();
    qtest_irq_intercept_in(dst, led->led_path);

    configure_ardep_pll(src);
    configure_led_exti3(src, led);
    migrate_incoming_qmp(dst, uri, NULL, "{}");
    migrate_qmp(src, dst, uri, NULL, "{}");
    wait_for_migration_complete(src);
    qtest_qmp_eventwait(dst, "RESUME");

    g_assert_cmphex(rcc_readl(dst, RCC_CR) &
                    (RCC_CR_HSEBYP | RCC_CR_HSEON | RCC_CR_HSERDY |
                     RCC_CR_PLLON | RCC_CR_PLLRDY),
                    ==, RCC_CR_HSEBYP | RCC_CR_HSEON | RCC_CR_HSERDY |
                        RCC_CR_PLLON | RCC_CR_PLLRDY);
    g_assert_cmphex(rcc_readl(dst, RCC_PLLCFGR), ==,
                    RCC_PLLCFGR_ARDEP);
    g_assert_cmphex(rcc_readl(dst, RCC_CFGR) & RCC_CFGR_SWS_MASK, ==,
                    RCC_CFGR_SWS_PLL);
    assert_clock_hz(dst, "/machine/hse", 16000000);
    assert_core_clocks(dst, 170000000, 21250000);
    g_assert_true(qtest_get_irq(dst, 0));

    gpio_set_level(dst, led, false);
    g_assert_false(qtest_get_irq(dst, 0));
    g_assert_cmphex(qtest_readl(dst, EXTI_BASE + EXTI_PR1), ==, exti3);
    g_assert_true(nvic_exti3_pending(dst));
    qtest_writel(dst, EXTI_BASE + EXTI_PR1, exti3);
    qtest_writel(dst, NVIC_ICPR0, BIT(EXTI3_IRQ));
    g_assert_false(nvic_exti3_pending(dst));
    gpio_set_level(dst, led, true);
    g_assert_true(qtest_get_irq(dst, 0));

    qtest_system_reset(dst);
    assert_clock_hz(dst, "/machine/hse", 16000000);
    g_assert_cmphex(rcc_readl(dst, RCC_CR) &
                    (RCC_CR_HSEBYP | RCC_CR_HSEON |
                     RCC_CR_HSERDY | RCC_CR_PLLON | RCC_CR_PLLRDY),
                    ==, RCC_CR_HSEBYP);
    assert_hsi16_reset_clocks(dst);
    configure_ardep_pll(dst);

    qtest_quit(src);
    qtest_quit(dst);
    g_unlink(sock);
    g_rmdir(tmpdir);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/ardep-v2/1-machine-topology",
                   test_machine_topology);
    qtest_add_func("/ardep-v2/2-hse-pll-reset",
                   test_hse_pll_and_reset);
    for (unsigned int i = 0; i < ARRAY_SIZE(led_cases); i++) {
        g_autofree char *level_path =
            g_strdup_printf("/ardep-v2/3-led/%s/levels",
                            led_cases[i].name);
        g_autofree char *fanout_path =
            g_strdup_printf("/ardep-v2/3-led/%s/exti-fanout",
                            led_cases[i].name);

        qtest_add_data_func(level_path, &led_cases[i], test_led_levels);
        qtest_add_data_func(fanout_path, &led_cases[i],
                            test_led_exti_fanout);
    }
    qtest_add_func("/ardep-v2/4-serial0-ordering",
                   test_serial0_ordering);
    qtest_add_func("/ardep-v2/5-loader/raw-flash-layout",
                   test_raw_flash_layout);
    qtest_add_func("/ardep-v2/5-loader/oversize-rejected",
                   test_raw_oversize_rejected);
    qtest_add_func("/ardep-v2/6-active-migration",
                   test_active_migration);
    qtest_add_func("/ardep-v2/7-can-links/no-backend",
                   test_can_links_no_backend);
    qtest_add_func("/ardep-v2/7-can-links/shared",
                   test_can_links_shared);
    qtest_add_func("/ardep-v2/7-can-links/independent",
                   test_can_links_independent);

    return g_test_run();
}
