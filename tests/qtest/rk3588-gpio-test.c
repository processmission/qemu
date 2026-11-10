/*
 * QEMU RK3588 GPIO bank tests
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "libqtest.h"

#define ROCK5B_MACHINE_NAME     "rock-5b"
#define ROCK5BPLUS_MACHINE_NAME "rock-5b-plus"
#define ROCK5B_GIC_DIST_BASE    0xfe600000ULL
#define ROCK5B_GPIO0_SPI        277

#define ROCK5B_GPIO_VERSION_V2  0x01000c2b

enum {
    GPIO_SWPORT_DR_L = 0x00,
    GPIO_SWPORT_DR_H = 0x04,
    GPIO_SWPORT_DDR_L = 0x08,
    GPIO_SWPORT_DDR_H = 0x0c,
    GPIO_INT_EN_L = 0x10,
    GPIO_INT_EN_H = 0x14,
    GPIO_INT_MASK_L = 0x18,
    GPIO_INT_MASK_H = 0x1c,
    GPIO_INT_TYPE_L = 0x20,
    GPIO_INT_TYPE_H = 0x24,
    GPIO_INT_POLARITY_L = 0x28,
    GPIO_INT_POLARITY_H = 0x2c,
    GPIO_INT_BOTHEDGE_L = 0x30,
    GPIO_INT_BOTHEDGE_H = 0x34,
    GPIO_INT_STATUS_L = 0x50,
    GPIO_INT_STATUS_H = 0x54,
    GPIO_INT_RAWSTATUS_L = 0x58,
    GPIO_INT_RAWSTATUS_H = 0x5c,
    GPIO_PORT_EOI_L = 0x60,
    GPIO_PORT_EOI_H = 0x64,
    GPIO_EXT_PORT_L = 0x70,
    GPIO_EXT_PORT_H = 0x74,
    GPIO_VERSION_ID = 0x78,
};

typedef struct Rock5BGPIOBank {
    uint64_t base;
    unsigned int spi;
} Rock5BGPIOBank;

static const Rock5BGPIOBank rock5b_gpio_banks[] = {
    { 0xfd8a0000, ROCK5B_GPIO0_SPI + 0 },
    { 0xfec20000, ROCK5B_GPIO0_SPI + 1 },
    { 0xfec30000, ROCK5B_GPIO0_SPI + 2 },
    { 0xfec40000, ROCK5B_GPIO0_SPI + 3 },
    { 0xfec50000, ROCK5B_GPIO0_SPI + 4 },
};

/* GIC SPI N is INTID 32+N on the distributor. */
#define ROCK5B_GIC_SPI_INTID(spi)   ((spi) + 32)
#define ROCK5B_GIC_ISPENDR(intid) \
    (ROCK5B_GIC_DIST_BASE + 0x200 + 4 * ((intid) / 32))

static QTestState *rock5b_gpio_qtest_start(void)
{
    return qtest_init("-machine " ROCK5B_MACHINE_NAME
                      " -m 256M -serial null -display none");
}

static uint32_t gpio_we16(uint32_t mask, uint32_t value)
{
    return ((mask & 0xffff) << 16) | (value & 0xffff);
}

static void gpio_write16(QTestState *qts, uint64_t base, uint32_t reg,
                         uint32_t mask, uint32_t value)
{
    qtest_writel(qts, base + reg, gpio_we16(mask, value));
}

static void gpio_set_input(QTestState *qts, unsigned int bank,
                           unsigned int pin, int level)
{
    g_autofree char *path =
        g_strdup_printf("/machine/soc/gpio%u", bank);

    qtest_set_irq_in(qts, path, "unnamed-gpio-in", pin, level);
}

static void assert_gpio_bit(QTestState *qts, unsigned int bank,
                            uint32_t reg, unsigned int bit, bool set)
{
    uint32_t value = qtest_readl(qts, rock5b_gpio_banks[bank].base + reg);

    if (set) {
        g_assert_cmphex(value & BIT(bit), ==, BIT(bit));
    } else {
        g_assert_cmphex(value & BIT(bit), ==, 0);
    }
}

static void assert_gic_spi_pending(QTestState *qts, unsigned int spi,
                                   bool pending)
{
    unsigned int intid = ROCK5B_GIC_SPI_INTID(spi);
    uint32_t mask = BIT(intid % 32);
    uint32_t value = qtest_readl(qts, ROCK5B_GIC_ISPENDR(intid));

    g_assert_cmpuint(!!(value & mask), ==, pending);
}

static void assert_gpio_gic_routing(QTestState *qts, int active_bank)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_gpio_banks); i++) {
        assert_gic_spi_pending(qts, rock5b_gpio_banks[i].spi,
                               i == active_bank);
    }
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

static void gpio_configure_irq(QTestState *qts, unsigned int bank,
                               unsigned int pin, bool edge, bool polarity,
                               bool bothedge, bool masked)
{
    const Rock5BGPIOBank *gpio = &rock5b_gpio_banks[bank];
    uint32_t bit = BIT(pin);

    gpio_write16(qts, gpio->base, GPIO_INT_TYPE_L, bit,
                 edge ? bit : 0);
    gpio_write16(qts, gpio->base, GPIO_INT_POLARITY_L, bit,
                 polarity ? bit : 0);
    gpio_write16(qts, gpio->base, GPIO_INT_BOTHEDGE_L, bit,
                 bothedge ? bit : 0);
    gpio_write16(qts, gpio->base, GPIO_INT_MASK_L, bit,
                 masked ? bit : 0);
    gpio_write16(qts, gpio->base, GPIO_INT_EN_L, bit, bit);
}

static void gpio_eoi(QTestState *qts, unsigned int bank, unsigned int pin)
{
    uint32_t bit = BIT(pin);

    gpio_write16(qts, rock5b_gpio_banks[bank].base, GPIO_PORT_EOI_L,
                 bit, bit);
}

static void assert_gpio_irq_state(QTestState *qts, unsigned int bank,
                                  unsigned int pin, bool raw, bool status)
{
    assert_gpio_bit(qts, bank, GPIO_INT_RAWSTATUS_L, pin, raw);
    assert_gpio_bit(qts, bank, GPIO_INT_STATUS_L, pin, status);
}

static void test_gpio_qom_and_mapping(void)
{
    QTestState *qts = rock5b_gpio_qtest_start();

    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_gpio_banks); i++) {
        g_autofree char *name = g_strdup_printf("gpio%u", i);

        assert_qom_child(qts, "/machine/soc", name, "rockchip-gpio");
        g_assert_cmphex(qtest_readl(qts, rock5b_gpio_banks[i].base +
                                    GPIO_VERSION_ID), ==,
                        ROCK5B_GPIO_VERSION_V2);
    }

    /*
     * Give each bank a distinct output bit.  The same bit value is used in
     * every bank, so an address alias would make the asserted bits accumulate.
     */
    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_gpio_banks); i++) {
        uint32_t bit = BIT(i);

        gpio_write16(qts, rock5b_gpio_banks[i].base, GPIO_SWPORT_DDR_L,
                     bit, bit);
        gpio_write16(qts, rock5b_gpio_banks[i].base, GPIO_SWPORT_DR_L,
                     bit, bit);
        gpio_write16(qts, rock5b_gpio_banks[i].base, GPIO_SWPORT_DDR_H,
                     bit, bit);
        gpio_write16(qts, rock5b_gpio_banks[i].base, GPIO_SWPORT_DR_H,
                     bit, bit);
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_gpio_banks); i++) {
        uint32_t expected = BIT(i);

        g_assert_cmphex(qtest_readl(qts, rock5b_gpio_banks[i].base +
                                    GPIO_EXT_PORT_L), ==, expected);
        g_assert_cmphex(qtest_readl(qts, rock5b_gpio_banks[i].base +
                                    GPIO_EXT_PORT_H), ==, expected);
    }

    qtest_quit(qts);
}

static void test_gpio_data_direction_and_mask(void)
{
    const Rock5BGPIOBank *gpio0 = &rock5b_gpio_banks[0];
    QTestState *qts = rock5b_gpio_qtest_start();

    qtest_irq_intercept_out_named(qts, "/machine/soc/gpio0", "gpio-out");

    /* An input pin follows its external line, not a software DR write. */
    gpio_set_input(qts, 0, 0, 1);
    assert_gpio_bit(qts, 0, GPIO_EXT_PORT_L, 0, true);
    gpio_write16(qts, gpio0->base, GPIO_SWPORT_DR_L, BIT(0), BIT(0));
    assert_gpio_bit(qts, 0, GPIO_EXT_PORT_L, 0, true);
    gpio_set_input(qts, 0, 0, 0);
    assert_gpio_bit(qts, 0, GPIO_EXT_PORT_L, 0, false);

    /* Once configured as output, DR drives EXT_PORT and gpio-out. */
    gpio_write16(qts, gpio0->base, GPIO_SWPORT_DDR_L, BIT(0), BIT(0));
    assert_gpio_bit(qts, 0, GPIO_EXT_PORT_L, 0, true);
    g_assert_true(qtest_get_irq(qts, 0));
    gpio_set_input(qts, 0, 0, 0);
    assert_gpio_bit(qts, 0, GPIO_EXT_PORT_L, 0, true);
    g_assert_true(qtest_get_irq(qts, 0));

    /* A zero high-word mask must not modify DR. */
    gpio_write16(qts, gpio0->base, GPIO_SWPORT_DR_L, 0, 0);
    assert_gpio_bit(qts, 0, GPIO_EXT_PORT_L, 0, true);
    gpio_write16(qts, gpio0->base, GPIO_SWPORT_DR_L, BIT(0), 0);
    assert_gpio_bit(qts, 0, GPIO_EXT_PORT_L, 0, false);
    g_assert_false(qtest_get_irq(qts, 0));
    gpio_write16(qts, gpio0->base, GPIO_SWPORT_DR_L, BIT(0), BIT(0));
    g_assert_true(qtest_get_irq(qts, 0));

    /* Partial writes only touch the selected data bits. */
    gpio_write16(qts, gpio0->base, GPIO_SWPORT_DDR_L,
                 BIT(1) | BIT(2) | BIT(3),
                 BIT(1) | BIT(2) | BIT(3));
    gpio_write16(qts, gpio0->base, GPIO_SWPORT_DR_L, BIT(1), BIT(1));
    g_assert_cmphex(qtest_readl(qts, gpio0->base + GPIO_SWPORT_DR_L),
                    ==, BIT(0) | BIT(1));
    gpio_write16(qts, gpio0->base, GPIO_SWPORT_DR_L, 0, BIT(2));
    g_assert_cmphex(qtest_readl(qts, gpio0->base + GPIO_SWPORT_DR_L),
                    ==, BIT(0) | BIT(1));
    gpio_write16(qts, gpio0->base, GPIO_SWPORT_DR_L,
                 BIT(2) | BIT(3), BIT(2));
    g_assert_cmphex(qtest_readl(qts, gpio0->base + GPIO_SWPORT_DR_L),
                    ==, BIT(0) | BIT(1) | BIT(2));

    /* The high-half window controls pins 16-31 independently. */
    gpio_write16(qts, gpio0->base, GPIO_SWPORT_DDR_H, BIT(0), BIT(0));
    gpio_write16(qts, gpio0->base, GPIO_SWPORT_DR_H, BIT(0), BIT(0));
    g_assert_cmphex(qtest_readl(qts, gpio0->base + GPIO_SWPORT_DDR_H),
                    ==, BIT(0));
    g_assert_cmphex(qtest_readl(qts, gpio0->base + GPIO_SWPORT_DR_H),
                    ==, BIT(0));
    g_assert_cmphex(qtest_readl(qts, gpio0->base + GPIO_SWPORT_DR_L),
                    ==, BIT(0) | BIT(1) | BIT(2));
    assert_gpio_bit(qts, 0, GPIO_EXT_PORT_H, 0, true);
    g_assert_true(qtest_get_irq(qts, 16));

    /* Pin and bank state remain isolated. */
    gpio_set_input(qts, 0, 4, 1);
    assert_gpio_bit(qts, 0, GPIO_EXT_PORT_L, 4, true);
    gpio_set_input(qts, 1, 4, 1);
    assert_gpio_bit(qts, 0, GPIO_EXT_PORT_L, 4, true);
    g_assert_cmphex(qtest_readl(qts, rock5b_gpio_banks[1].base +
                                GPIO_EXT_PORT_L), ==, BIT(4));

    qtest_quit(qts);
}

static void test_gpio_rising_edge(void)
{
    QTestState *qts = rock5b_gpio_qtest_start();

    gpio_configure_irq(qts, 0, 0, true, true, false, false);
    gpio_set_input(qts, 0, 0, 1);
    assert_gpio_irq_state(qts, 0, 0, true, true);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, true);

    gpio_eoi(qts, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, false, false);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, false);

    /* A falling edge does not retrigger a rising-edge input. */
    gpio_set_input(qts, 0, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, false, false);

    qtest_quit(qts);
}

static void test_gpio_falling_edge(void)
{
    QTestState *qts = rock5b_gpio_qtest_start();

    gpio_set_input(qts, 0, 0, 1);
    gpio_configure_irq(qts, 0, 0, true, false, false, false);
    gpio_set_input(qts, 0, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, true, true);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, true);

    gpio_eoi(qts, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, false, false);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, false);

    qtest_quit(qts);
}

static void test_gpio_both_edges(void)
{
    QTestState *qts = rock5b_gpio_qtest_start();

    gpio_configure_irq(qts, 0, 0, true, true, true, false);
    gpio_set_input(qts, 0, 0, 1);
    assert_gpio_irq_state(qts, 0, 0, true, true);
    gpio_eoi(qts, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, false, false);

    gpio_set_input(qts, 0, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, true, true);
    gpio_eoi(qts, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, false, false);

    qtest_quit(qts);
}

static void test_gpio_high_level(void)
{
    QTestState *qts = rock5b_gpio_qtest_start();

    gpio_set_input(qts, 0, 0, 1);
    gpio_configure_irq(qts, 0, 0, false, true, false, false);
    assert_gpio_irq_state(qts, 0, 0, true, true);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, true);

    /* EOI cannot release a level-triggered interrupt while it is active. */
    gpio_eoi(qts, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, true, true);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, true);

    gpio_set_input(qts, 0, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, false, false);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, false);

    qtest_quit(qts);
}

static void test_gpio_low_level(void)
{
    QTestState *qts = rock5b_gpio_qtest_start();

    gpio_configure_irq(qts, 0, 0, false, false, false, false);
    assert_gpio_irq_state(qts, 0, 0, true, true);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, true);

    gpio_set_input(qts, 0, 0, 1);
    assert_gpio_irq_state(qts, 0, 0, false, false);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, false);

    gpio_set_input(qts, 0, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, true, true);
    gpio_eoi(qts, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, true, true);

    qtest_quit(qts);
}

static void test_gpio_mask_and_unmask(void)
{
    QTestState *qts = rock5b_gpio_qtest_start();

    gpio_configure_irq(qts, 0, 0, true, true, false, true);
    gpio_set_input(qts, 0, 0, 1);
    assert_gpio_irq_state(qts, 0, 0, true, false);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, false);

    gpio_write16(qts, rock5b_gpio_banks[0].base, GPIO_INT_MASK_L,
                 BIT(0), 0);
    assert_gpio_irq_state(qts, 0, 0, true, true);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, true);

    gpio_eoi(qts, 0, 0);
    assert_gpio_irq_state(qts, 0, 0, false, false);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[0].spi, false);

    qtest_quit(qts);
}

static void test_gpio_gic_routing(void)
{
    QTestState *qts = rock5b_gpio_qtest_start();

    assert_gpio_gic_routing(qts, -1);
    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_gpio_banks); i++) {
        gpio_configure_irq(qts, i, 0, true, true, false, false);
        gpio_set_input(qts, i, 0, 1);
        assert_gpio_gic_routing(qts, i);
        gpio_eoi(qts, i, 0);
        assert_gpio_gic_routing(qts, -1);
        gpio_set_input(qts, i, 0, 0);
    }

    qtest_quit(qts);
}

static void test_gpio_reset(void)
{
    QTestState *qts = rock5b_gpio_qtest_start();

    qtest_irq_intercept_out_named(qts, "/machine/soc/gpio4", "gpio-out");
    gpio_write16(qts, rock5b_gpio_banks[4].base, GPIO_SWPORT_DDR_L,
                 BIT(0), BIT(0));
    gpio_write16(qts, rock5b_gpio_banks[4].base, GPIO_SWPORT_DR_L,
                 BIT(0), BIT(0));
    gpio_configure_irq(qts, 4, 1, true, true, false, false);
    gpio_set_input(qts, 4, 1, 1);
    assert_gpio_irq_state(qts, 4, 1, true, true);
    assert_gic_spi_pending(qts, rock5b_gpio_banks[4].spi, true);
    g_assert_true(qtest_get_irq(qts, 0));

    qtest_system_reset(qts);

    g_assert_false(qtest_get_irq(qts, 0));
    assert_gpio_gic_routing(qts, -1);
    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_gpio_banks); i++) {
        static const uint32_t reset_regs[] = {
            GPIO_SWPORT_DR_L, GPIO_SWPORT_DR_H,
            GPIO_SWPORT_DDR_L, GPIO_SWPORT_DDR_H,
            GPIO_INT_EN_L, GPIO_INT_EN_H,
            GPIO_INT_MASK_L, GPIO_INT_MASK_H,
            GPIO_INT_TYPE_L, GPIO_INT_TYPE_H,
            GPIO_INT_POLARITY_L, GPIO_INT_POLARITY_H,
            GPIO_INT_BOTHEDGE_L, GPIO_INT_BOTHEDGE_H,
            GPIO_INT_STATUS_L, GPIO_INT_STATUS_H,
            GPIO_INT_RAWSTATUS_L, GPIO_INT_RAWSTATUS_H,
            GPIO_EXT_PORT_L, GPIO_EXT_PORT_H,
        };

        g_assert_cmphex(qtest_readl(qts, rock5b_gpio_banks[i].base +
                                    GPIO_VERSION_ID), ==,
                        ROCK5B_GPIO_VERSION_V2);
        for (unsigned int j = 0; j < ARRAY_SIZE(reset_regs); j++) {
            g_assert_cmphex(qtest_readl(qts, rock5b_gpio_banks[i].base +
                                        reset_regs[j]), ==, 0);
        }
    }

    qtest_quit(qts);
}

static void test_gpio_not_unimplemented(void)
{
    g_autofree char *log_path =
        g_build_filename(g_get_tmp_dir(), "rk3588-gpio-unimplemented.log",
                         NULL);
    g_autofree char *log = NULL;
    QTestState *qts;

    unlink(log_path);
    qts = qtest_initf("-machine " ROCK5B_MACHINE_NAME
                      " -m 256M -serial null -display none"
                      " -d unimp -D \"%s\"", log_path);

    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_gpio_banks); i++) {
        g_assert_cmphex(qtest_readl(qts, rock5b_gpio_banks[i].base +
                                    GPIO_VERSION_ID), ==,
                        ROCK5B_GPIO_VERSION_V2);
    }

    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_gpio_banks); i++) {
        g_autofree char *unimpl_name = g_strdup_printf("rk3588.gpio%u", i);

        g_assert_null(strstr(log, unimpl_name));
    }
    unlink(log_path);
}

static void test_gpio_legacy_banks(void)
{
    QTestState *qts;

    /*
     * The legacy machines already created these banks before the default
     * path did.  Keep that path covered now that creation is shared.
     */
    if (!qtest_has_machine(ROCK5BPLUS_MACHINE_NAME)) {
        g_test_skip(ROCK5BPLUS_MACHINE_NAME " machine is not available");
        return;
    }

    qts = qtest_init("-machine " ROCK5BPLUS_MACHINE_NAME
                     " -m 256M -serial null -display none");

    for (unsigned int i = 0; i < ARRAY_SIZE(rock5b_gpio_banks); i++) {
        g_autofree char *name = g_strdup_printf("gpio%u", i);

        assert_qom_child(qts, "/machine/soc", name, "rockchip-gpio");
        g_assert_cmphex(qtest_readl(qts, rock5b_gpio_banks[i].base +
                                    GPIO_VERSION_ID), ==,
                        ROCK5B_GPIO_VERSION_V2);
    }

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(ROCK5B_MACHINE_NAME)) {
        g_test_skip(ROCK5B_MACHINE_NAME " machine is not available");
        return 0;
    }
    if (!qtest_has_device("rockchip-gpio")) {
        g_test_skip("rockchip-gpio device is not available");
        return 0;
    }

    qtest_add_func("/rk3588-gpio/qom-and-mapping",
                   test_gpio_qom_and_mapping);
    qtest_add_func("/rk3588-gpio/data-direction-and-mask",
                   test_gpio_data_direction_and_mask);
    qtest_add_func("/rk3588-gpio/rising-edge", test_gpio_rising_edge);
    qtest_add_func("/rk3588-gpio/falling-edge", test_gpio_falling_edge);
    qtest_add_func("/rk3588-gpio/both-edges", test_gpio_both_edges);
    qtest_add_func("/rk3588-gpio/high-level", test_gpio_high_level);
    qtest_add_func("/rk3588-gpio/low-level", test_gpio_low_level);
    qtest_add_func("/rk3588-gpio/mask-and-unmask",
                   test_gpio_mask_and_unmask);
    qtest_add_func("/rk3588-gpio/gic-routing", test_gpio_gic_routing);
    qtest_add_func("/rk3588-gpio/reset", test_gpio_reset);
    qtest_add_func("/rk3588-gpio/not-unimplemented",
                   test_gpio_not_unimplemented);
    qtest_add_func("/rk3588-gpio/legacy-banks", test_gpio_legacy_banks);

    return g_test_run();
}
