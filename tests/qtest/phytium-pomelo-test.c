/*
 * QTest coverage for the Phytium D2000 Pomelo machine.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"

#define POMELO_MACHINE        "phytium-pomelo"
#define POMELO_RAM            0x80000000ULL
#define POMELO_HIGH_RAM       0x2000000000ULL
#define POMELO_UART           0x28001000ULL
#define POMELO_GIC_DIST       0x29a00000ULL
#define POMELO_PCIE_ECAM      0x40000000ULL
#define POMELO_GMAC0          0x2820c000ULL

static QTestState *pomelo_start(const char *machine, const char *memory)
{
    return qtest_initf("-machine %s -smp 8 -m %s -display none",
                       machine, memory);
}

static void test_pomelo_machine(void)
{
    QTestState *qts = pomelo_start(POMELO_MACHINE, "1G");

    qtest_writeq(qts, POMELO_RAM, UINT64_C(0x1122334455667788));
    g_assert_cmphex(qtest_readq(qts, POMELO_RAM), ==,
                    UINT64_C(0x1122334455667788));
    g_assert_cmpuint(qtest_readl(qts, POMELO_GIC_DIST + 0x4), !=, 0);
    g_assert_cmphex(qtest_readl(qts, POMELO_UART + 0x18) & 0x80, ==, 0x80);
    qtest_readl(qts, POMELO_PCIE_ECAM);
    qtest_readl(qts, POMELO_GMAC0 + 0x110);
    qtest_quit(qts);
}

static void test_pomelo_highmem(void)
{
    QTestState *qts = pomelo_start("phytium-d2000", "3G");

    qtest_writeq(qts, POMELO_RAM, UINT64_C(0x0123456789abcdef));
    qtest_writeq(qts, POMELO_HIGH_RAM, UINT64_C(0xfedcba9876543210));
    g_assert_cmphex(qtest_readq(qts, POMELO_RAM), ==,
                    UINT64_C(0x0123456789abcdef));
    g_assert_cmphex(qtest_readq(qts, POMELO_HIGH_RAM), ==,
                    UINT64_C(0xfedcba9876543210));
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/phytium-pomelo/machine", test_pomelo_machine);
    qtest_add_func("/phytium-pomelo/highmem", test_pomelo_highmem);
    return g_test_run();
}
