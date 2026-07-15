/*
 * QTest for the M5Stack AI Pyramid (Axera AX650X)
 *
 * Copyright (c) 2026 Zevorn
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qnum.h"
#include "libqtest.h"

#define AX650X_NUM_CPUS              8

#define AX650X_RAM_BASE              0x100000000ULL
#define AX650X_RESET_RAM_TEST_ADDR   (AX650X_RAM_BASE + 256 * MiB)

#define AX650X_GIC_DIST_BASE         0x04901000
#define AX650X_GIC_HYP_BASE          0x04904000
#define AX650X_GICD_CTLR             0x000
#define AX650X_GICD_TYPER            0x004
#define AX650X_GICD_ISPENDR          0x200
#define AX650X_GICH_VTR              0x004

#define AX650X_UART0_BASE            0x02016000
#define AX650X_UART_REGSHIFT         2
#define AX650X_UART_IER              (1 << AX650X_UART_REGSHIFT)
#define AX650X_UART_LSR              (5 << AX650X_UART_REGSHIFT)
#define AX650X_UART_IER_THRI         0x02
#define AX650X_UART_LSR_THRE         0x20
#define AX650X_UART_LSR_TEMT         0x40
#define AX650X_UART0_INTID           (32 + 135)

static QTestState *ax650x_pyramid_start(void)
{
    return qtest_init("-machine ax650x-pyramid -accel qtest -display none");
}

static uint64_t qom_get_uint(QTestState *qts, const char *path,
                             const char *property)
{
    QDict *response;
    QNum *number;
    uint64_t value;

    response = qtest_qmp(qts,
                         "{ 'execute': 'qom-get',"
                         "  'arguments': { 'path': %s, 'property': %s } }",
                         path, property);
    g_assert(qdict_haskey(response, "return"));
    number = qobject_to(QNum, qdict_get(response, "return"));
    g_assert_nonnull(number);
    g_assert_true(qnum_get_try_uint(number, &value));
    qobject_unref(response);

    return value;
}

static void test_cpu_topology(void)
{
    QTestState *qts = ax650x_pyramid_start();
    QDict *response;
    QList *cpus;
    QListEntry *entry;

    response = qtest_qmp(qts, "{ 'execute': 'query-cpus-fast' }");
    g_assert(qdict_haskey(response, "return"));
    cpus = qdict_get_qlist(response, "return");
    g_assert_cmpuint(qlist_size(cpus), ==, AX650X_NUM_CPUS);

    QLIST_FOREACH_ENTRY(cpus, entry) {
        QDict *cpu = qobject_to(QDict, qlist_entry_obj(entry));
        unsigned int index = qdict_get_int(cpu, "cpu-index");
        const char *path = qdict_get_str(cpu, "qom-path");

        g_assert_cmpuint(index, <, AX650X_NUM_CPUS);
        g_assert_cmphex(qom_get_uint(qts, path, "mp-affinity"), ==,
                        (uint64_t)index << 8);
    }

    qobject_unref(response);
    qtest_quit(qts);
}

static void test_memory_and_gic(void)
{
    QTestState *qts = ax650x_pyramid_start();
    uint64_t pattern = 0x0123456789abcdefULL;
    uint32_t typer;
    uint32_t vtr;

    qtest_writeq(qts, AX650X_RAM_BASE + 0x1000, pattern);
    g_assert_cmphex(qtest_readq(qts, AX650X_RAM_BASE + 0x1000), ==,
                    pattern);

    g_assert_cmphex(qtest_readl(qts, AX650X_GIC_DIST_BASE + AX650X_GICD_CTLR),
                    ==, 0);
    typer = qtest_readl(qts, AX650X_GIC_DIST_BASE + AX650X_GICD_TYPER);
    g_assert_cmpuint(typer & 0x1f, ==, 7);
    g_assert_cmpuint((typer >> 5) & 0x7, ==, AX650X_NUM_CPUS - 1);

    vtr = qtest_readl(qts, AX650X_GIC_HYP_BASE + AX650X_GICH_VTR);
    g_assert_cmpuint(vtr & 0x3f, ==, 3);

    qtest_quit(qts);
}

static void test_uart_irq_and_reset(void)
{
    QTestState *qts = ax650x_pyramid_start();
    uint64_t ram_pattern = 0xfedcba9876543210ULL;
    uint64_t pending_addr;
    uint32_t pending_mask;
    uint32_t lsr;

    lsr = qtest_readl(qts, AX650X_UART0_BASE + AX650X_UART_LSR);
    g_assert_cmphex(lsr & (AX650X_UART_LSR_THRE | AX650X_UART_LSR_TEMT), ==,
                    AX650X_UART_LSR_THRE | AX650X_UART_LSR_TEMT);
    g_assert_cmphex(qtest_readl(qts, AX650X_UART0_BASE + AX650X_UART_IER),
                    ==, 0);

    qtest_writel(qts, AX650X_UART0_BASE + AX650X_UART_IER,
                 AX650X_UART_IER_THRI);
    pending_addr = AX650X_GIC_DIST_BASE + AX650X_GICD_ISPENDR +
                   (AX650X_UART0_INTID / 32) * sizeof(uint32_t);
    pending_mask = 1U << (AX650X_UART0_INTID % 32);
    g_assert_cmphex(qtest_readl(qts, pending_addr) & pending_mask, ==,
                    pending_mask);

    qtest_writeq(qts, AX650X_RESET_RAM_TEST_ADDR, ram_pattern);
    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, AX650X_UART0_BASE + AX650X_UART_IER),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, AX650X_GIC_DIST_BASE + AX650X_GICD_CTLR),
                    ==, 0);
    g_assert_cmphex(qtest_readq(qts, AX650X_RESET_RAM_TEST_ADDR), ==,
                    ram_pattern);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("ax650x-pyramid/cpu-topology", test_cpu_topology);
    qtest_add_func("ax650x-pyramid/memory-and-gic", test_memory_and_gic);
    qtest_add_func("ax650x-pyramid/uart-irq-and-reset",
                   test_uart_irq_and_reset);

    return g_test_run();
}
