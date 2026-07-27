/*
 * QTest testcase for the STM32G474 machine
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu/bswap.h"
#include "qemu/units.h"
#include "libqtest.h"

#define STM32G474_MACHINE          "stm32g474"
#define FLASH_BASE                 0x08000000
#define FLASH_SIZE                 (512 * KiB)
#define SRAM1_BASE                 0x20000000
#define SRAM1_SIZE                 (80 * KiB)
#define SRAM2_BASE                 0x20014000
#define SRAM2_SIZE                 (16 * KiB)
#define CCM_BASE                   0x10000000
#define CCM_ALIAS_BASE             0x20018000
#define CCM_SIZE                   (32 * KiB)

#define INITIAL_MSP                0x20020000
#define RESET_VECTOR               0x08000101
#define FLASH_MARKER               0x4d41524b
#define FLASH_END_MARKER           0x454e444d

static QTestState *stm32g474_qtest_init(void)
{
    return qtest_init("-machine " STM32G474_MACHINE " -serial null");
}

static void cleanup_temp_file(void *path)
{
    qtest_remove_abrt_handler(path);
    g_unlink(path);
    g_free(path);
}

static void assert_reset_registers(QTestState *qts)
{
    g_autofree char *registers = qtest_hmp(qts, "info registers");

    g_assert_nonnull(strstr(registers, "R13=20020000"));
    g_assert_nonnull(strstr(registers, "R15=08000100"));
}

static void test_machine_constructs(void)
{
    QTestState *qts;

    g_assert_true(qtest_has_machine(STM32G474_MACHINE));
    qts = stm32g474_qtest_init();

    qtest_quit(qts);
}

static void test_flash_boot_alias_reset(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree uint8_t *image = g_malloc0(FLASH_SIZE);
    /* A relative safe-character path needs no platform-specific quoting. */
    char *image_path = g_strdup("stm32g474-flash-XXXXXX");
    QTestState *qts;
    int fd;

    stl_le_p(image, INITIAL_MSP);
    stl_le_p(image + 4, RESET_VECTOR);
    stl_le_p(image + 8, FLASH_MARKER);
    stw_le_p(image + 0x100, 0xe7fe);
    stl_le_p(image + FLASH_SIZE - sizeof(uint32_t), FLASH_END_MARKER);

    fd = g_mkstemp(image_path);
    g_assert_cmpint(fd, >=, 0);
    qtest_add_abrt_handler(cleanup_temp_file, image_path);
    g_test_queue_destroy(cleanup_temp_file, image_path);
    g_assert_cmpint(close(fd), ==, 0);
    g_assert_true(g_file_set_contents(image_path, (const char *)image,
                                      FLASH_SIZE, &error));
    g_assert_no_error(error);

    qts = qtest_initf("-machine " STM32G474_MACHINE
                      " -serial null -kernel %s", image_path);

    g_assert_cmphex(qtest_readl(qts, 0), ==, INITIAL_MSP);
    g_assert_cmphex(qtest_readl(qts, 4), ==, RESET_VECTOR);
    g_assert_cmphex(qtest_readl(qts, 8), ==, FLASH_MARKER);
    g_assert_cmphex(qtest_readl(qts, FLASH_SIZE - sizeof(uint32_t)),
                    ==, FLASH_END_MARKER);
    g_assert_cmphex(qtest_readl(qts, FLASH_BASE), ==, INITIAL_MSP);
    g_assert_cmphex(qtest_readl(qts, FLASH_BASE + 4), ==, RESET_VECTOR);
    g_assert_cmphex(qtest_readl(qts, FLASH_BASE + 8), ==, FLASH_MARKER);
    g_assert_cmphex(qtest_readl(qts, FLASH_BASE + FLASH_SIZE -
                                sizeof(uint32_t)), ==, FLASH_END_MARKER);
    assert_reset_registers(qts);

    qtest_system_reset(qts);
    assert_reset_registers(qts);
    qtest_system_reset(qts);
    assert_reset_registers(qts);

    qtest_quit(qts);
}

static void test_linear_sram(void)
{
    static const struct {
        uint64_t address;
        uint32_t value;
    } bank_samples[] = {
        { SRAM1_BASE, 0x11111111 },
        { SRAM1_BASE + SRAM1_SIZE - sizeof(uint32_t), 0x1111eeee },
        { SRAM2_BASE, 0x22222222 },
        { SRAM2_BASE + SRAM2_SIZE - sizeof(uint32_t), 0x2222dddd },
        { CCM_BASE, 0x33333333 },
        { CCM_BASE + CCM_SIZE - sizeof(uint32_t), 0x3333cccc },
    };
    static const uint8_t sram1_to_sram2[] = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
    };
    static const uint8_t sram2_to_ccm[] = {
        0x89, 0x7a, 0x6b, 0x5c, 0x4d, 0x3e, 0x2f, 0x10,
    };
    uint8_t readback[sizeof(sram1_to_sram2)];
    QTestState *qts;

    qts = stm32g474_qtest_init();

    for (size_t i = 0; i < ARRAY_SIZE(bank_samples); i++) {
        qtest_writel(qts, bank_samples[i].address, bank_samples[i].value);
    }
    /* Read only after every bank is seeded so shared backing cannot hide. */
    for (size_t i = 0; i < ARRAY_SIZE(bank_samples); i++) {
        g_assert_cmphex(qtest_readl(qts, bank_samples[i].address),
                        ==, bank_samples[i].value);
    }

    qtest_memwrite(qts, SRAM1_BASE + SRAM1_SIZE - sizeof(uint32_t),
                   sram1_to_sram2, sizeof(sram1_to_sram2));
    qtest_memread(qts, SRAM1_BASE + SRAM1_SIZE - sizeof(uint32_t), readback,
                  sizeof(readback));
    g_assert_cmpmem(readback, sizeof(readback), sram1_to_sram2,
                    sizeof(sram1_to_sram2));

    qtest_memwrite(qts, SRAM2_BASE + SRAM2_SIZE - sizeof(uint32_t),
                   sram2_to_ccm, sizeof(sram2_to_ccm));
    qtest_memread(qts, SRAM2_BASE + SRAM2_SIZE - sizeof(uint32_t), readback,
                  sizeof(readback));
    g_assert_cmpmem(readback, sizeof(readback), sram2_to_ccm,
                    sizeof(sram2_to_ccm));

    qtest_quit(qts);
}

static void test_ccm_alias(void)
{
    QTestState *qts;

    qts = stm32g474_qtest_init();

    qtest_writel(qts, CCM_BASE, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, CCM_ALIAS_BASE), ==, 0x12345678);
    qtest_writel(qts, CCM_ALIAS_BASE, 0x87654321);
    g_assert_cmphex(qtest_readl(qts, CCM_BASE), ==, 0x87654321);

    qtest_writel(qts, CCM_BASE + CCM_SIZE - sizeof(uint32_t), 0x0badc0de);
    g_assert_cmphex(qtest_readl(qts, CCM_ALIAS_BASE + CCM_SIZE -
                                sizeof(uint32_t)), ==, 0x0badc0de);
    qtest_writel(qts, CCM_ALIAS_BASE + CCM_SIZE - sizeof(uint32_t),
                 0xc001d00d);
    g_assert_cmphex(qtest_readl(qts, CCM_BASE + CCM_SIZE -
                                sizeof(uint32_t)), ==, 0xc001d00d);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("stm32g474/machine/constructs", test_machine_constructs);
    qtest_add_func("stm32g474/memory/flash-boot-alias-reset",
                   test_flash_boot_alias_reset);
    qtest_add_func("stm32g474/memory/linear-sram", test_linear_sram);
    qtest_add_func("stm32g474/memory/ccm-alias", test_ccm_alias);

    return g_test_run();
}
