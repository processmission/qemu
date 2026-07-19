/*
 * QTest for the Radxa ROCK 5B+ machine
 *
 * Copyright (c) 2026 Chao Liu
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <libfdt.h>
#include "qemu/bitops.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "libqtest.h"

#define ROCK_5B_PLUS_MACHINE "rock-5b-plus"

#define RK3588_ATAGS_BASE 0x001fe000ULL
#define RK3588_RAM_BASE 0x00200000ULL
#define RK3588_PMU1_GRF_BASE 0xfd58a000ULL
#define RK3588_CRYPTO_BASE 0xfe370000ULL
#define RK3588_PCIE3X4_APB_BASE 0xfe150000ULL
#define RK3588_PCIE3X4_DBI_BASE 0xa40000000ULL
#define RK3588_PCIE3X2_APB_BASE 0xfe160000ULL
#define RK3588_PCIE3X2_DBI_BASE 0xa40400000ULL
#define RK3588_PCIE3X2_CFG_BASE 0xf1000000ULL
#define RK3588_GMAC0_BASE 0xfe1b0000ULL
#define RK3588_GMAC1_BASE 0xfe1c0000ULL
#define RK3588_SDMMC_BASE 0xfe2c0000ULL
#define RK3588_SDHCI_BASE 0xfe2e0000ULL
#define RK3588_GICD_BASE 0xfe600000ULL
#define RK3588_SECURE_OTP_BASE 0xfe3a0000ULL
#define RK3588_UART2_BASE 0xfeb50000ULL

#define PMU1_GRF_OS_REG2 0x0208
#define PMU1_GRF_OS_REG3 0x020c
#define RK3588_DDRTYPE_LOW_SHIFT 13
#define RK3588_DDRTYPE_HIGH_SHIFT 12
#define RK3588_DDRTYPE_LOW_MASK 0x7
#define RK3588_DDRTYPE_HIGH_MASK 0x1
#define RK3588_LPDDR5 9

#define RK3588_ATAG_CORE 0x54410001
#define RK3588_ATAG_CORE_WORDS 5
#define RK3588_ATAG_DDR_MEM 0x54410052
#define RK3588_ATAG_DDR_MEM_WORDS 48

#define DWC_PCIE_VENDOR_DEVICE 0x0000
#define DWC_PCIE_LTSSM_STATUS 0x0300
#define DWC_PCIE_ATU_VIEWPORT 0x0900
#define DWC_PCIE_ATU_CR1 0x0904
#define DWC_PCIE_ATU_CR2 0x0908
#define DWC_PCIE_ATU_LOWER_BASE 0x090c
#define DWC_PCIE_ATU_UPPER_BASE 0x0910
#define DWC_PCIE_ATU_LIMIT 0x0914
#define DWC_PCIE_ATU_LOWER_TARGET 0x0918
#define DWC_PCIE_ATU_UPPER_TARGET 0x091c
#define DWC_PCIE_ATU_TYPE_CFG0 0x4
#define DWC_PCIE_ATU_ENABLE BIT(31)
#define DWMAC4_MAC_VERSION 0x0110
#define DWMAC4_SNPSVER_0x51 0x00000051
#define DW_MMC_VERID 0x006c
#define DW_MMC_VERID_270A 0x0000270a
#define SDHCI_CAPABILITIES 0x0040
#define GICD_TYPER 0x0004
#define UART_LSR (5 << 2)
#define UART_LSR_THRE 0x20
#define UART_LSR_TEMT 0x40
#define SECURE_OTP_DOUT 0x20
#define SECURE_OTP_INT_STATUS 0x84
#define SECURE_OTP_UNIMPLEMENTED 0x100
#define SECURE_OTP_READ_DONE 0x2
#define CRYPTO_RST_CTL 0x004
#define CRYPTO_DMA_INT_ST 0x00c
#define CRYPTO_DMA_CTL 0x010
#define CRYPTO_DMA_LLI_ADDR 0x014
#define CRYPTO_FIFO_CTL 0x040
#define CRYPTO_HASH_CTL 0x048
#define CRYPTO_HASH_DOUT_0 0x3a0
#define CRYPTO_HASH_VALID 0x3e4
#define CRYPTO_WRITE_MASK(value) ((uint32_t)(value) << 16)
#define CRYPTO_DMA_SRC_ITEM_DONE 0x4
#define CRYPTO_DMA_LIST_ERR 0x20
#define CRYPTO_HASH_VALID_BIT 0x1
#define CRYPTO_FIFO_BYTESWAP 0x3
#define CRYPTO_HASH_SHA256_PAD_ENABLE 0x25
#define CRYPTO_LLI_USER_HASH_START_LAST 0x7
#define CRYPTO_LLI_DMA_LAST_SRC_DONE 0x401

static QTestState *rock_5b_plus_qtest_start(unsigned int cpus)
{
    return qtest_initf("-machine " ROCK_5B_PLUS_MACHINE
                       " -smp %u -m 512M", cpus);
}

static void assert_fdt_cells(const void *fdt, int node, const char *property,
                             const uint32_t *expected, size_t count)
{
    const fdt32_t *cells;
    int length;

    cells = fdt_getprop(fdt, node, property, &length);
    g_assert_nonnull(cells);
    g_assert_cmpint(length, ==, (int)(count * sizeof(*cells)));

    for (size_t i = 0; i < count; i++) {
        g_assert_cmphex(fdt32_to_cpu(cells[i]), ==, expected[i]);
    }
}

static void test_rock_5b_plus_pcie3x2_fdt(void)
{
    static const uint32_t kernel_insn = GUINT32_TO_LE(0x14000000);
    static const uint32_t expected_reg[] = {
        0x0000000a, 0x40400000, 0x00000000, 0x00400000,
        0x00000000, 0xfe160000, 0x00000000, 0x00010000,
        0x00000000, 0xf1000000, 0x00000000, 0x00100000,
    };
    static const uint32_t expected_interrupts[] = {
        0, 258, 4, 0,
        0, 257, 4, 0,
        0, 256, 4, 0,
        0, 255, 4, 0,
        0, 254, 4, 0,
    };
    static const uint32_t expected_bus_range[] = { 0x10, 0x1f };
    static const uint32_t expected_ranges[] = {
        0x01000000, 0x00000000, 0xf1100000,
                    0x00000000, 0xf1100000, 0x00000000, 0x00100000,
        0x02000000, 0x00000000, 0xf1200000,
                    0x00000000, 0xf1200000, 0x00000000, 0x00e00000,
        0x03000000, 0x00000009, 0x40000000,
                    0x00000009, 0x40000000, 0x00000000, 0x40000000,
    };
    g_autofree char *kernel_path = NULL;
    g_autofree char *dtb_path = NULL;
    g_autofree char *machine_arg = NULL;
    g_autofree char *dtb = NULL;
    g_autofree char *stderr_buf = NULL;
    g_autoptr(GError) error = NULL;
    gsize dtb_size;
    int kernel_fd, dtb_fd, exit_status;
    int pcie, its;
    const fdt32_t *cells;
    int length;
    bool spawned;

    kernel_fd = g_file_open_tmp("rock5b-plus-kernel-XXXXXX", &kernel_path,
                                &error);
    g_assert_no_error(error);
    g_assert_cmpint(kernel_fd, >=, 0);
    g_assert_cmpint(close(kernel_fd), ==, 0);
    g_assert_true(g_file_set_contents(kernel_path,
                                      (const char *)&kernel_insn,
                                      sizeof(kernel_insn), &error));
    g_assert_no_error(error);

    dtb_fd = g_file_open_tmp("rock5b-plus-dtb-XXXXXX", &dtb_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(dtb_fd, >=, 0);
    g_assert_cmpint(close(dtb_fd), ==, 0);

    machine_arg = g_strdup_printf(ROCK_5B_PLUS_MACHINE ",dumpdtb=%s",
                                  dtb_path);
    const char *argv[] = {
        qtest_qemu_binary(NULL),
        "-machine", machine_arg,
        "-cpu", "cortex-a76",
        "-smp", "1",
        "-m", "512M",
        "-kernel", kernel_path,
        "-display", "none",
        "-serial", "none",
        "-nodefaults",
        NULL,
    };

    spawned = g_spawn_sync(NULL, (char **)argv, NULL,
                           G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL,
                           &stderr_buf, &exit_status, &error);
    g_assert_true(spawned);
    g_assert_no_error(error);
    if (!g_spawn_check_exit_status(exit_status, &error)) {
        g_error("QEMU failed to dump the ROCK 5B+ DTB: %s\n%s",
                error->message, stderr_buf ? stderr_buf : "");
    }

    g_assert_true(g_file_get_contents(dtb_path, &dtb, &dtb_size, &error));
    g_assert_no_error(error);
    g_assert_cmpint(fdt_check_header(dtb), ==, 0);
    g_assert_cmpuint(fdt_totalsize(dtb), <=, dtb_size);

    pcie = fdt_path_offset(dtb, "/pcie@fe160000");
    g_assert_cmpint(pcie, >=, 0);
    g_assert_cmpint(fdt_node_check_compatible(dtb, pcie,
                                              "rockchip,rk3588-pcie"), ==,
                    0);
    assert_fdt_cells(dtb, pcie, "reg", expected_reg,
                     ARRAY_SIZE(expected_reg));
    assert_fdt_cells(dtb, pcie, "interrupts", expected_interrupts,
                     ARRAY_SIZE(expected_interrupts));
    assert_fdt_cells(dtb, pcie, "bus-range", expected_bus_range,
                     ARRAY_SIZE(expected_bus_range));
    assert_fdt_cells(dtb, pcie, "ranges", expected_ranges,
                     ARRAY_SIZE(expected_ranges));

    cells = fdt_getprop(dtb, pcie, "num-lanes", &length);
    g_assert_nonnull(cells);
    g_assert_cmpint(length, ==, (int)sizeof(*cells));
    g_assert_cmphex(fdt32_to_cpu(cells[0]), ==, 2);

    cells = fdt_getprop(dtb, pcie, "linux,pci-domain", &length);
    g_assert_nonnull(cells);
    g_assert_cmpint(length, ==, (int)sizeof(*cells));
    g_assert_cmphex(fdt32_to_cpu(cells[0]), ==, 1);

    cells = fdt_getprop(dtb, pcie, "resets", &length);
    g_assert_nonnull(cells);
    g_assert_cmpint(length, ==, 4 * (int)sizeof(*cells));
    g_assert_cmphex(fdt32_to_cpu(cells[0]), !=, 0);
    g_assert_cmphex(fdt32_to_cpu(cells[0]), ==, fdt32_to_cpu(cells[2]));
    g_assert_cmphex(fdt32_to_cpu(cells[1]), ==, 526);
    g_assert_cmphex(fdt32_to_cpu(cells[3]), ==, 541);

    its = fdt_path_offset(dtb,
                          "/interrupt-controller@fe600000/"
                          "msi-controller@fe660000");
    g_assert_cmpint(its, >=, 0);
    cells = fdt_getprop(dtb, pcie, "msi-map", &length);
    g_assert_nonnull(cells);
    g_assert_cmpint(length, ==, 4 * (int)sizeof(*cells));
    g_assert_cmphex(fdt32_to_cpu(cells[0]), ==, 0x1000);
    g_assert_cmphex(fdt32_to_cpu(cells[1]), ==,
                    fdt_get_phandle(dtb, its));
    g_assert_cmphex(fdt32_to_cpu(cells[2]), ==, 0x1000);
    g_assert_cmphex(fdt32_to_cpu(cells[3]), ==, 0x1000);

    g_assert_cmpint(g_unlink(kernel_path), ==, 0);
    g_assert_cmpint(g_unlink(dtb_path), ==, 0);
}

static void test_rock_5b_plus_pcie3x2_bus_number(void)
{
    QTestState *qts = rock_5b_plus_qtest_start(1);
    QDict *response;
    QList *buses;
    QListEntry *entry;
    uint32_t dbi_id;
    bool bus_10_found = false;

    response = qtest_qmp(qts, "{ 'execute': 'query-pci' }");
    g_assert(qdict_haskey(response, "return"));
    buses = qdict_get_qlist(response, "return");

    QLIST_FOREACH_ENTRY(buses, entry) {
        QDict *bus = qobject_to(QDict, qlist_entry_obj(entry));

        if (qdict_get_int(bus, "bus") == 0x10) {
            bus_10_found = true;
            break;
        }
    }
    g_assert_true(bus_10_found);
    qobject_unref(response);

    qtest_writel(qts, RK3588_PCIE3X2_DBI_BASE + DWC_PCIE_ATU_VIEWPORT, 0);
    qtest_writel(qts, RK3588_PCIE3X2_DBI_BASE + DWC_PCIE_ATU_CR1,
                 DWC_PCIE_ATU_TYPE_CFG0);
    qtest_writel(qts, RK3588_PCIE3X2_DBI_BASE + DWC_PCIE_ATU_LOWER_BASE,
                 RK3588_PCIE3X2_CFG_BASE);
    qtest_writel(qts, RK3588_PCIE3X2_DBI_BASE + DWC_PCIE_ATU_UPPER_BASE, 0);
    qtest_writel(qts, RK3588_PCIE3X2_DBI_BASE + DWC_PCIE_ATU_LIMIT,
                 RK3588_PCIE3X2_CFG_BASE + 0xfffff);
    qtest_writel(qts, RK3588_PCIE3X2_DBI_BASE + DWC_PCIE_ATU_LOWER_TARGET,
                 0x10 << 24);
    qtest_writel(qts, RK3588_PCIE3X2_DBI_BASE + DWC_PCIE_ATU_UPPER_TARGET, 0);
    qtest_writel(qts, RK3588_PCIE3X2_DBI_BASE + DWC_PCIE_ATU_CR2,
                 DWC_PCIE_ATU_ENABLE);

    dbi_id = qtest_readl(qts, RK3588_PCIE3X2_DBI_BASE +
                         DWC_PCIE_VENDOR_DEVICE);
    g_assert_cmphex(dbi_id, !=, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RK3588_PCIE3X2_CFG_BASE), ==, dbi_id);

    qtest_quit(qts);
}

static void test_rock_5b_plus_machine_creation(void)
{
    QTestState *qts = rock_5b_plus_qtest_start(1);
    uint64_t sdhci_caps;
    uint32_t pcie_id;
    uint32_t sys_reg2;
    uint32_t sys_reg3;

    qtest_writel(qts, RK3588_RAM_BASE, 0x5b5b3588);
    g_assert_cmphex(qtest_readl(qts, RK3588_RAM_BASE), ==, 0x5b5b3588);

    g_assert_cmphex(qtest_readb(qts, RK3588_UART2_BASE + UART_LSR) &
                    (UART_LSR_THRE | UART_LSR_TEMT), ==,
                    UART_LSR_THRE | UART_LSR_TEMT);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_VERID), ==,
                    DW_MMC_VERID_270A);

    sdhci_caps = qtest_readl(qts, RK3588_SDHCI_BASE + SDHCI_CAPABILITIES);
    sdhci_caps |= (uint64_t)qtest_readl(qts, RK3588_SDHCI_BASE +
                                        SDHCI_CAPABILITIES + 4) << 32;
    g_assert_cmphex(sdhci_caps, !=, 0);
    g_assert_cmphex(sdhci_caps, !=, UINT64_MAX);

    pcie_id = qtest_readl(qts, RK3588_PCIE3X4_DBI_BASE +
                          DWC_PCIE_VENDOR_DEVICE);
    g_assert_cmphex(pcie_id, !=, 0);
    g_assert_cmphex(pcie_id, !=, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RK3588_PCIE3X4_APB_BASE +
                                DWC_PCIE_LTSSM_STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_PCIE3X4_DBI_BASE +
                                0x3ffffc), ==, 0);

    pcie_id = qtest_readl(qts, RK3588_PCIE3X2_DBI_BASE +
                          DWC_PCIE_VENDOR_DEVICE);
    g_assert_cmphex(pcie_id, !=, 0);
    g_assert_cmphex(pcie_id, !=, UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RK3588_PCIE3X2_APB_BASE +
                                DWC_PCIE_LTSSM_STATUS), ==, 0);
    qtest_writel(qts, RK3588_PCIE3X2_DBI_BASE + 0x100010,
                 UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RK3588_PCIE3X2_DBI_BASE +
                                0x100010), ==, 0);

    g_assert_cmphex(qtest_readl(qts, RK3588_GICD_BASE + GICD_TYPER), !=, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_GICD_BASE + GICD_TYPER), !=,
                    UINT32_MAX);

    sys_reg2 = qtest_readl(qts, RK3588_PMU1_GRF_BASE + PMU1_GRF_OS_REG2);
    sys_reg3 = qtest_readl(qts, RK3588_PMU1_GRF_BASE + PMU1_GRF_OS_REG3);
    g_assert_cmphex((sys_reg2 >> RK3588_DDRTYPE_LOW_SHIFT) &
                    RK3588_DDRTYPE_LOW_MASK, ==,
                    RK3588_LPDDR5 & RK3588_DDRTYPE_LOW_MASK);
    g_assert_cmphex((sys_reg3 >> RK3588_DDRTYPE_HIGH_SHIFT) &
                    RK3588_DDRTYPE_HIGH_MASK, ==,
                    (RK3588_LPDDR5 >> 3) & RK3588_DDRTYPE_HIGH_MASK);

    g_assert_cmphex(qtest_readl(qts, RK3588_ATAGS_BASE), ==,
                    RK3588_ATAG_CORE_WORDS);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATAGS_BASE + 4), ==,
                    RK3588_ATAG_CORE);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATAGS_BASE + 20), ==,
                    RK3588_ATAG_DDR_MEM_WORDS);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATAGS_BASE + 24), ==,
                    RK3588_ATAG_DDR_MEM);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATAGS_BASE + 28), ==, 1);
    g_assert_cmphex(qtest_readq(qts, RK3588_ATAGS_BASE + 36), ==, 0);
    g_assert_cmphex(qtest_readq(qts, RK3588_ATAGS_BASE + 44), ==,
                    RK3588_RAM_BASE + 512ULL * 1024 * 1024);
    g_assert_cmphex(qtest_readl(qts, RK3588_ATAGS_BASE + 20 +
                                RK3588_ATAG_DDR_MEM_WORDS *
                                sizeof(uint32_t)), ==, 0);

    /* ROCK 5B+ uses a PCIe RTL8125 NIC, not either RK3588 DWMAC. */
    g_assert_cmphex(qtest_readl(qts, RK3588_GMAC0_BASE +
                                DWMAC4_MAC_VERSION), !=,
                    DWMAC4_SNPSVER_0x51);
    g_assert_cmphex(qtest_readl(qts, RK3588_GMAC1_BASE +
                                DWMAC4_MAC_VERSION), !=,
                    DWMAC4_SNPSVER_0x51);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_SDMMC_BASE + DW_MMC_VERID), ==,
                    DW_MMC_VERID_270A);

    qtest_quit(qts);
}

static void test_rock_5b_plus_smp_creation(void)
{
    QTestState *qts = rock_5b_plus_qtest_start(8);

    qtest_quit(qts);
}

static void test_rock_5b_plus_unfused_secure_otp(void)
{
    QTestState *qts = rock_5b_plus_qtest_start(1);

    g_assert_cmphex(qtest_readl(qts, RK3588_SECURE_OTP_BASE +
                                SECURE_OTP_INT_STATUS), ==,
                    SECURE_OTP_READ_DONE);
    g_assert_cmphex(qtest_readl(qts, RK3588_SECURE_OTP_BASE +
                                SECURE_OTP_DOUT), ==, 0);

    qtest_writel(qts, RK3588_SECURE_OTP_BASE + SECURE_OTP_DOUT,
                 UINT32_MAX);
    qtest_writel(qts, RK3588_SECURE_OTP_BASE + SECURE_OTP_INT_STATUS, 0);
    qtest_writel(qts, RK3588_SECURE_OTP_BASE + SECURE_OTP_UNIMPLEMENTED,
                 UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RK3588_SECURE_OTP_BASE +
                                SECURE_OTP_DOUT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_SECURE_OTP_BASE +
                                SECURE_OTP_INT_STATUS), ==,
                    SECURE_OTP_READ_DONE);
    g_assert_cmphex(qtest_readl(qts, RK3588_SECURE_OTP_BASE +
                                SECURE_OTP_UNIMPLEMENTED), ==, 0);

    qtest_system_reset(qts);
    g_assert_cmphex(qtest_readl(qts, RK3588_SECURE_OTP_BASE +
                                SECURE_OTP_DOUT), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_SECURE_OTP_BASE +
                                SECURE_OTP_INT_STATUS), ==,
                    SECURE_OTP_READ_DONE);
    g_assert_cmphex(qtest_readl(qts, RK3588_SECURE_OTP_BASE +
                                SECURE_OTP_UNIMPLEMENTED), ==, 0);

    qtest_quit(qts);
}

static void test_rock_5b_plus_crypto_sha256(void)
{
    static const uint8_t input[] = { 'a', 'b', 'c' };
    static const uint32_t expected[] = {
        0xba7816bf, 0x8f01cfea, 0x414140de, 0x5dae2223,
        0xb00361a3, 0x96177a9c, 0xb410ff61, 0xf20015ad,
    };
    const uint64_t input_addr = RK3588_RAM_BASE;
    const uint64_t lli_addr = RK3588_RAM_BASE + 0x1000;
    QTestState *qts = rock_5b_plus_qtest_start(1);
    uint32_t status = 0;

    qtest_memwrite(qts, input_addr, input, sizeof(input));
    qtest_writel(qts, lli_addr + 0x00, input_addr);
    qtest_writel(qts, lli_addr + 0x04, sizeof(input));
    qtest_writel(qts, lli_addr + 0x08, 0);
    qtest_writel(qts, lli_addr + 0x0c, 0);
    qtest_writel(qts, lli_addr + 0x10, CRYPTO_LLI_USER_HASH_START_LAST);
    qtest_writel(qts, lli_addr + 0x14, 0);
    qtest_writel(qts, lli_addr + 0x18, CRYPTO_LLI_DMA_LAST_SRC_DONE);
    qtest_writel(qts, lli_addr + 0x1c, 0);

    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_RST_CTL,
                 CRYPTO_WRITE_MASK(1) | 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_RST_CTL), ==, 0);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_FIFO_CTL,
                 CRYPTO_WRITE_MASK(CRYPTO_FIFO_BYTESWAP) |
                 CRYPTO_FIFO_BYTESWAP);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_HASH_CTL,
                 CRYPTO_WRITE_MASK(0xffff) |
                 CRYPTO_HASH_SHA256_PAD_ENABLE);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_HASH_CTL, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_HASH_CTL), ==,
                    CRYPTO_HASH_SHA256_PAD_ENABLE);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_LLI_ADDR, lli_addr);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_CTL,
                 CRYPTO_WRITE_MASK(1) | 1);

    for (unsigned int i = 0; i < 1000 && !status; i++) {
        qtest_clock_step(qts, 1);
        status = qtest_readl(qts, RK3588_CRYPTO_BASE +
                             CRYPTO_DMA_INT_ST);
    }

    g_assert_cmphex(status, ==, CRYPTO_DMA_SRC_ITEM_DONE);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_HASH_VALID), ==,
                    CRYPTO_HASH_VALID_BIT);
    for (unsigned int i = 0; i < ARRAY_SIZE(expected); i++) {
        g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                    CRYPTO_HASH_DOUT_0 + i * 4), ==,
                        expected[i]);
    }

    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_INT_ST,
                 CRYPTO_DMA_SRC_ITEM_DONE);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_HASH_VALID,
                 CRYPTO_HASH_VALID_BIT);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_DMA_INT_ST), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_HASH_VALID), ==, 0);

    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_CTL,
                 CRYPTO_WRITE_MASK(2) | 2);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_DMA_INT_ST), ==,
                    CRYPTO_DMA_LIST_ERR);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_INT_ST,
                 CRYPTO_DMA_LIST_ERR);

    qtest_writel(qts, lli_addr + 0x10, 0x6);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_CTL,
                 CRYPTO_WRITE_MASK(1) | 1);
    status = 0;
    for (unsigned int i = 0; i < 1000 && !status; i++) {
        qtest_clock_step(qts, 1);
        status = qtest_readl(qts, RK3588_CRYPTO_BASE +
                             CRYPTO_DMA_INT_ST);
    }
    g_assert_cmphex(status, ==, CRYPTO_DMA_LIST_ERR);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_INT_ST,
                 CRYPTO_DMA_LIST_ERR);

    qtest_writel(qts, lli_addr + 0x10,
                 CRYPTO_LLI_USER_HASH_START_LAST | 0x8);
    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_DMA_CTL,
                 CRYPTO_WRITE_MASK(1) | 1);
    status = 0;
    for (unsigned int i = 0; i < 1000 && !status; i++) {
        qtest_clock_step(qts, 1);
        status = qtest_readl(qts, RK3588_CRYPTO_BASE +
                             CRYPTO_DMA_INT_ST);
    }
    g_assert_cmphex(status, ==, CRYPTO_DMA_LIST_ERR);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_HASH_VALID), ==, 0);

    qtest_writel(qts, RK3588_CRYPTO_BASE + CRYPTO_RST_CTL,
                 CRYPTO_WRITE_MASK(1) | 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_DMA_INT_ST), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_CRYPTO_BASE +
                                CRYPTO_HASH_DOUT_0), ==, 0);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(ROCK_5B_PLUS_MACHINE)) {
        g_test_skip(ROCK_5B_PLUS_MACHINE " machine not available");
        return 0;
    }

    qtest_add_func("/rock-5b-plus/machine-creation",
                   test_rock_5b_plus_machine_creation);
    qtest_add_func("/rock-5b-plus/smp-creation",
                   test_rock_5b_plus_smp_creation);
    qtest_add_func("/rock-5b-plus/pcie3x2-fdt",
                   test_rock_5b_plus_pcie3x2_fdt);
    qtest_add_func("/rock-5b-plus/pcie3x2-bus-number",
                   test_rock_5b_plus_pcie3x2_bus_number);
    qtest_add_func("/rock-5b-plus/unfused-secure-otp",
                   test_rock_5b_plus_unfused_secure_otp);
    qtest_add_func("/rock-5b-plus/crypto-sha256",
                   test_rock_5b_plus_crypto_sha256);

    return g_test_run();
}
