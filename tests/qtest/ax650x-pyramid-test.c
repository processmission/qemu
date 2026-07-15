/*
 * QTest for the M5Stack AI Pyramid (Axera AX650X)
 *
 * Copyright (c) 2026 Zevorn
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/sd/ax650x-sdhci.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qnum.h"
#include "libqtest.h"
#include "libqos/sdhci-cmd.h"

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
#define AX650X_UART_USR              0x7c
#define AX650X_UART_UCV              0xf8
#define AX650X_UART_IER_THRI         0x02
#define AX650X_UART_LSR_THRE         0x20
#define AX650X_UART_LSR_TEMT         0x40
#define AX650X_UART_USR_TFNF         BIT(1)
#define AX650X_UART_USR_TFE          BIT(2)
#define AX650X_UART0_INTID           (32 + 135)

#define AX650X_EMMC_BASE             0x28000000
#define AX650X_EMMC_INTID            (32 + 93)
#define AX650X_EMMC_TEST_IMAGE_SIZE  (1 * MiB)
#define AX650X_EMMC_BLOCK_SIZE       512

#define SDHC_NORINTSTS               0x30
#define SDHC_NORINTSTSEN             0x34
#define SDHC_NORINTSIGEN             0x38
#define SDHC_NIS_CMDCMP              BIT(0)
#define SDHC_AX650X_HCVER            0x2402
#define SDHC_EMMC_SEND_OP_COND       (1 << 8)
#define SDHC_EMMC_SET_RELATIVE_ADDR  (3 << 8)

static char *emmc_path;

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

static void test_uart_extension_registers(void)
{
    QTestState *qts = ax650x_pyramid_start();

    g_assert_cmphex(qtest_readl(qts, AX650X_UART0_BASE + AX650X_UART_USR),
                    ==, AX650X_UART_USR_TFNF | AX650X_UART_USR_TFE);
    g_assert_cmphex(qtest_readl(qts, AX650X_UART0_BASE + AX650X_UART_UCV),
                    ==, 0);

    qtest_quit(qts);
}

static void test_emmc_registers_and_reset(void)
{
    QTestState *qts = ax650x_pyramid_start();
    uint64_t base = AX650X_EMMC_BASE;
    uint64_t capabilities;

    g_assert_cmphex(qtest_readw(qts, base + AX650X_SDHCI_VENDOR_PTR), ==,
                    AX650X_SDHCI_VENDOR_PTR_VALUE);
    capabilities = qtest_readq(qts, base + SDHC_CAPAB);
    g_assert_cmphex(capabilities & BIT_ULL(18), ==, BIT_ULL(18));
    g_assert_cmphex(qtest_readw(qts, base + SDHC_HCVER), ==,
                    SDHC_AX650X_HCVER);

    g_assert_cmphex(qtest_readl(qts, base + AX650X_SDHCI_PHY_CNFG), ==,
                    AX650X_SDHCI_PHY_PWRGOOD);
    qtest_writel(qts, base + AX650X_SDHCI_PHY_CNFG, 0x00cc0001);
    g_assert_cmphex(qtest_readl(qts, base + AX650X_SDHCI_PHY_CNFG), ==,
                    0x00cc0003);

    qtest_writew(qts, base + AX650X_SDHCI_PHY_CMDPAD_CNFG, 0x0449);
    g_assert_cmphex(qtest_readw(qts,
                               base + AX650X_SDHCI_PHY_CMDPAD_CNFG), ==,
                    0x0449);
    qtest_writeb(qts, base + AX650X_SDHCI_PHY_DLL_CTRL,
                 AX650X_SDHCI_DLL_EN);
    g_assert_cmphex(qtest_readb(qts,
                               base + AX650X_SDHCI_PHY_DLL_STATUS), ==,
                    AX650X_SDHCI_DLL_LOCKED);
    qtest_writeb(qts, base + AX650X_SDHCI_PHY_DLL_CTRL, 0);
    g_assert_cmphex(qtest_readb(qts,
                               base + AX650X_SDHCI_PHY_DLL_STATUS), ==, 0);

    qtest_writeb(qts, base + AX650X_SDHCI_PHY_DLLDBG_MLKDC, 0xff);
    qtest_writeb(qts, base + AX650X_SDHCI_PHY_DLLDBG_SLKDC, 0xff);
    g_assert_cmphex(qtest_readb(qts,
                               base + AX650X_SDHCI_PHY_DLLDBG_MLKDC), ==,
                    98);
    g_assert_cmphex(qtest_readb(qts,
                               base + AX650X_SDHCI_PHY_DLLDBG_SLKDC), ==,
                    24);

    qtest_writew(qts, base + AX650X_SDHCI_EMMC_CTRL, 0xffff);
    g_assert_cmphex(qtest_readw(qts, base + AX650X_SDHCI_EMMC_CTRL), ==,
                    AX650X_SDHCI_CARD_IS_EMMC |
                    AX650X_SDHCI_EMMC_RST_N |
                    AX650X_SDHCI_EMMC_RST_N_OE |
                    AX650X_SDHCI_ENH_STROBE_EN);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, base + AX650X_SDHCI_PHY_CNFG), ==,
                    AX650X_SDHCI_PHY_PWRGOOD);
    g_assert_cmphex(qtest_readw(qts,
                               base + AX650X_SDHCI_PHY_CMDPAD_CNFG), ==, 0);
    g_assert_cmphex(qtest_readb(qts,
                               base + AX650X_SDHCI_PHY_DLL_STATUS), ==, 0);
    g_assert_cmphex(qtest_readw(qts, base + AX650X_SDHCI_EMMC_CTRL), ==, 0);
    g_assert_cmphex(qtest_readw(qts, base + AX650X_SDHCI_VENDOR_PTR), ==,
                    AX650X_SDHCI_VENDOR_PTR_VALUE);

    qtest_quit(qts);
}

static QTestState *ax650x_pyramid_start_with_emmc(void)
{
    QTestState *qts;
    uint64_t base = AX650X_EMMC_BASE;

    qts = qtest_initf("-machine ax650x-pyramid -accel qtest -display none "
                      "-drive file=%s,if=sd,format=raw,auto-read-only=off",
                      emmc_path);
    qtest_writeb(qts, base + SDHC_SWRST, SDHC_RESET_ALL);
    qtest_writew(qts, base + SDHC_CLKCON,
                 SDHC_CLOCK_SDCLK_EN | SDHC_CLOCK_INT_EN);

    qtest_writel(qts, base + SDHC_NORINTSTSEN, SDHC_NIS_CMDCMP);
    qtest_writel(qts, base + SDHC_NORINTSIGEN, SDHC_NIS_CMDCMP);
    sdhci_cmd_regs(qts, base, 0, 0, 0, 0, 0);

    sdhci_cmd_regs(qts, base, 0, 0, 0x40ff8000, 0,
                   SDHC_EMMC_SEND_OP_COND);
    sdhci_cmd_regs(qts, base, 0, 0, 0, 0, SDHC_ALL_SEND_CID);
    sdhci_cmd_regs(qts, base, 0, 0, 1 << 16, 0,
                   SDHC_EMMC_SET_RELATIVE_ADDR | SDHC_CMD_RESPONSE);
    sdhci_cmd_regs(qts, base, 0, 0, 1 << 16, 0,
                   SDHC_SELECT_DESELECT_CARD);

    return qts;
}

static void test_emmc_block_io_and_irq(void)
{
    uint8_t source[AX650X_EMMC_BLOCK_SIZE];
    uint8_t written[AX650X_EMMC_BLOCK_SIZE];
    uint8_t readback[AX650X_EMMC_BLOCK_SIZE];
    QTestState *qts;
    uint64_t pending_addr;
    uint32_t pending_mask;
    int fd;
    ssize_t ret;

    for (unsigned int i = 0; i < sizeof(source); i++) {
        source[i] = i % 127 + 1;
        written[i] = (i * 3) % 127 + 1;
    }
    fd = open(emmc_path, O_WRONLY);
    g_assert_cmpint(fd, >=, 0);
    ret = pwrite(fd, source, sizeof(source), 0);
    g_assert_cmpint(ret, ==, sizeof(source));
    close(fd);

    qts = ax650x_pyramid_start_with_emmc();
    g_assert_cmphex(qtest_readw(qts,
                               AX650X_EMMC_BASE + SDHC_NORINTSTS) &
                    SDHC_NIS_CMDCMP, ==, SDHC_NIS_CMDCMP);
    pending_addr = AX650X_GIC_DIST_BASE + AX650X_GICD_ISPENDR +
                   (AX650X_EMMC_INTID / 32) * sizeof(uint32_t);
    pending_mask = 1U << (AX650X_EMMC_INTID % 32);
    g_assert_cmphex(qtest_readl(qts, pending_addr) & pending_mask, ==,
                    pending_mask);

    ret = sdhci_read_cmd(qts, AX650X_EMMC_BASE, (char *)readback,
                         sizeof(readback));
    g_assert_cmpint(ret, ==, sizeof(readback));
    g_assert_cmpmem(readback, sizeof(readback), source, sizeof(source));

    sdhci_write_cmd(qts, AX650X_EMMC_BASE, (char *)written,
                    sizeof(written), AX650X_EMMC_BLOCK_SIZE);
    qtest_quit(qts);

    fd = open(emmc_path, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    ret = pread(fd, readback, sizeof(readback), 0);
    g_assert_cmpint(ret, ==, sizeof(readback));
    close(fd);
    g_assert_cmpmem(readback, sizeof(readback), written, sizeof(written));
}

static void emmc_drive_create(void)
{
    GError *error = NULL;
    int fd;

    fd = g_file_open_tmp("ax650x-emmc-XXXXXX", &emmc_path, &error);
    if (fd < 0) {
        g_error("unable to create eMMC image: %s", error->message);
    }
    g_assert_nonnull(emmc_path);
    g_assert_cmpint(ftruncate(fd, AX650X_EMMC_TEST_IMAGE_SIZE), ==, 0);
    close(fd);
}

static void emmc_drive_destroy(void)
{
    unlink(emmc_path);
    g_free(emmc_path);
}

int main(int argc, char **argv)
{
    int ret;

    emmc_drive_create();
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("ax650x-pyramid/cpu-topology", test_cpu_topology);
    qtest_add_func("ax650x-pyramid/memory-and-gic", test_memory_and_gic);
    qtest_add_func("ax650x-pyramid/uart-irq-and-reset",
                   test_uart_irq_and_reset);
    qtest_add_func("ax650x-pyramid/uart-extension-registers",
                   test_uart_extension_registers);
    qtest_add_func("ax650x-pyramid/emmc-registers-and-reset",
                   test_emmc_registers_and_reset);
    qtest_add_func("ax650x-pyramid/emmc-block-io-and-irq",
                   test_emmc_block_io_and_irq);

    ret = g_test_run();
    emmc_drive_destroy();
    return ret;
}
