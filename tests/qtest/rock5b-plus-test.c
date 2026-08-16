/*
 * QTest for the Radxa ROCK 5B+ machine
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <libfdt.h>
#include "hw/core/uboot_image.h"
#include "qemu/bitops.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "libqtest.h"

#define ROCK_5B_PLUS_MACHINE "rock-5b-plus"

#define RK3588_ATAGS_BASE 0x001fe000ULL
#define RK3588_RAM_BASE 0x00200000ULL
#define RK3588_ZEPHYR_RAM_BASE 0x10000000ULL
#define RK3588_DWC3_BASE 0xfc000000ULL
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
#define RK3588_SFC_BASE 0xfe2b0000ULL

/* Rockchip SFC registers (see hw/ssi/rockchip_sfc.c). */
#define SFC_CTRL        0x000
#define SFC_IMR         0x004
#define SFC_ICLR        0x008
#define SFC_RCVR        0x010
#define SFC_FSR         0x020
#define SFC_SR          0x024
#define SFC_RISR        0x028
#define SFC_VER         0x02c
#define SFC_DMA_TRIGGER 0x080
#define SFC_DMA_ADDR    0x084
#define SFC_LEN_CTRL    0x088
#define SFC_LEN_EXT     0x08c
#define SFC_CMD         0x100
#define SFC_ADDR        0x104
#define SFC_DATA        0x108

#define SFC_FSR_RXLV_SHIFT 16
#define SFC_FSR_RXLV_MASK  (0x1f << SFC_FSR_RXLV_SHIFT)
#define SFC_RISR_DMA       (1u << 7)
#define SFC_CMD_DIR_WR     (1u << 12)
#define SFC_CMD_ADDR_24    (1u << 14)

#define SFC_OP_JEDEC_ID   0x9f
#define SFC_OP_READ       0x03
#define SFC_OP_WREN       0x06
#define SFC_OP_PAGE_PROG  0x02
#define SFC_OP_ERASE_4K   0x20

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
#define DWC3_GCOREID 0xc120
#define DWC3_GEVNTADR_LO 0xc400
#define DWC3_GEVNTADR_HI 0xc404
#define DWC3_GEVNTSIZ 0xc408
#define DWC3_GEVNTCOUNT 0xc40c
#define DWC3_DCTL 0xc704
#define DWC3_DCTL_RUNSTOP BIT(31)
#define DWC3_DCTL_CSFTRST BIT(30)
#define DWC3_DEPCMDPAR1(ep) (0xc804 + 16 * (ep))
#define DWC3_DEPCMDPAR0(ep) (0xc808 + 16 * (ep))
#define DWC3_DEPCMD(ep) (0xc80c + 16 * (ep))
#define DWC3_DEPCMD_CMDACT BIT(10)
#define DWC3_DEPCMD_DEPSTRTXFER 6
#define DWC3_TRB_CTRL_HWO BIT(0)
#define DWC3_TRB_CTRL_LST BIT(1)
#define DWC3_TRBCTL_CONTROL_SETUP (2 << 4)
#define DWC3_TRBCTL_CONTROL_STATUS_2 (3 << 4)
#define DWC3_DEVT_USBRST 0x101
#define DWC3_DEVT_CONNECTDONE 0x201
#define DWC3_DEPEVT_XFERCOMPLETE(ep) (((ep) << 1) | (1 << 6))
#define DWC3_ENUM_SETUP_DELAY_NS 1000000

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

static void test_rock_5b_plus_zephyr_ram(void)
{
    QTestState *qts = qtest_initf("-machine " ROCK_5B_PLUS_MACHINE
                                  ",zephyr-ram=on -smp 1 -m 128M");
    const uint32_t value = 0x5b3585b3;

    qtest_writel(qts, RK3588_ZEPHYR_RAM_BASE, value);
    g_assert_cmphex(qtest_readl(qts, RK3588_ZEPHYR_RAM_BASE), ==, value);
    g_assert_cmphex(qtest_readl(qts, RK3588_RAM_BASE), ==, 0);

    qtest_quit(qts);
}

static void test_rock_5b_plus_zephyr_uimage(void)
{
    static const uint32_t kernel_insn = GUINT32_TO_LE(0x14000000);
    uboot_image_header_t header = {
        .ih_magic = GUINT32_TO_BE(IH_MAGIC),
        .ih_size = GUINT32_TO_BE(sizeof(kernel_insn)),
        .ih_load = GUINT32_TO_BE(RK3588_ZEPHYR_RAM_BASE),
        .ih_ep = GUINT32_TO_BE(RK3588_ZEPHYR_RAM_BASE),
        .ih_os = IH_OS_LINUX,
        .ih_arch = IH_ARCH_ARM64,
        .ih_type = IH_TYPE_KERNEL,
        .ih_comp = IH_COMP_NONE,
        .ih_name = "Zephyr ROCK 5B+",
    };
    g_autofree uint8_t *image = g_malloc(sizeof(header) +
                                         sizeof(kernel_insn));
    g_autofree char *kernel_path = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    int kernel_fd;

    memcpy(image, &header, sizeof(header));
    memcpy(image + sizeof(header), &kernel_insn, sizeof(kernel_insn));

    kernel_fd = g_file_open_tmp("rock5b-plus-zephyr-XXXXXX", &kernel_path,
                                &error);
    g_assert_no_error(error);
    g_assert_cmpint(kernel_fd, >=, 0);
    g_assert_cmpint(close(kernel_fd), ==, 0);
    g_assert_true(g_file_set_contents(kernel_path, (const char *)image,
                                      sizeof(header) + sizeof(kernel_insn),
                                      &error));
    g_assert_no_error(error);

    qts = qtest_initf("-machine " ROCK_5B_PLUS_MACHINE
                      ",zephyr-ram=on -smp 1 -m 128M -kernel %s",
                      kernel_path);
    g_assert_cmphex(qtest_readl(qts, RK3588_ZEPHYR_RAM_BASE), ==,
                    GUINT32_FROM_LE(kernel_insn));

    qtest_quit(qts);
    g_assert_cmpint(g_unlink(kernel_path), ==, 0);
}

static void test_rock_5b_plus_dwc3_device(void)
{
    const uint64_t event_buffer = 0;
    const uint64_t setup_trb = RK3588_ZEPHYR_RAM_BASE + 0x2000;
    const uint64_t setup_packet = RK3588_ZEPHYR_RAM_BASE + 0x2100;
    const uint64_t status_trb = RK3588_ZEPHYR_RAM_BASE + 0x2200;
    QTestState *qts = qtest_initf("-machine " ROCK_5B_PLUS_MACHINE
                                  ",zephyr-ram=on -smp 1 -m 128M");

    g_assert_cmphex(qtest_readl(qts, RK3588_DWC3_BASE + DWC3_GCOREID) >> 16,
                    ==, 0x5533);

    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DCTL, DWC3_DCTL_CSFTRST);
    g_assert_cmphex(qtest_readl(qts, RK3588_DWC3_BASE + DWC3_DCTL) &
                    DWC3_DCTL_CSFTRST, ==, 0);

    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_GEVNTADR_LO,
                 event_buffer);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_GEVNTADR_HI, 0);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_GEVNTSIZ, 64);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DCTL,
                 DWC3_DCTL_RUNSTOP);

    g_assert_cmphex(qtest_readl(qts, RK3588_DWC3_BASE + DWC3_GEVNTCOUNT),
                    ==, 2 * sizeof(uint32_t));
    g_assert_cmphex(qtest_readl(qts, event_buffer), ==, DWC3_DEVT_USBRST);
    g_assert_cmphex(qtest_readl(qts, event_buffer + sizeof(uint32_t)), ==,
                    DWC3_DEVT_CONNECTDONE);

    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_GEVNTCOUNT,
                 2 * sizeof(uint32_t));
    qtest_writel(qts, setup_trb, setup_packet);
    qtest_writel(qts, setup_trb + 4, 0);
    qtest_writel(qts, setup_trb + 8, 8);
    qtest_writel(qts, setup_trb + 12,
                 DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_LST |
                 DWC3_TRBCTL_CONTROL_SETUP);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DEPCMDPAR1(0), setup_trb);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DEPCMDPAR0(0), 0);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DEPCMD(0),
                 DWC3_DEPCMD_CMDACT | DWC3_DEPCMD_DEPSTRTXFER);

    g_assert_cmphex(qtest_readl(qts, RK3588_DWC3_BASE + DWC3_GEVNTCOUNT),
                    ==, 0);
    qtest_clock_step(qts, DWC3_ENUM_SETUP_DELAY_NS);
    g_assert_cmphex(qtest_readl(qts, setup_packet), ==, 0x00010500);
    g_assert_cmphex(qtest_readl(qts, setup_packet + 4), ==, 0);
    g_assert_cmphex(qtest_readl(qts, setup_trb + 12) & DWC3_TRB_CTRL_HWO,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, event_buffer + 8), ==,
                    DWC3_DEPEVT_XFERCOMPLETE(0));

    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_GEVNTCOUNT,
                 sizeof(uint32_t));
    qtest_writel(qts, status_trb, 0);
    qtest_writel(qts, status_trb + 4, 0);
    qtest_writel(qts, status_trb + 8, 0);
    qtest_writel(qts, status_trb + 12,
                 DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_LST |
                 DWC3_TRBCTL_CONTROL_STATUS_2);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DEPCMDPAR1(1), status_trb);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DEPCMDPAR0(1), 0);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DEPCMD(1),
                 DWC3_DEPCMD_CMDACT | DWC3_DEPCMD_DEPSTRTXFER);
    g_assert_cmphex(qtest_readl(qts, event_buffer + 12), ==,
                    DWC3_DEPEVT_XFERCOMPLETE(1));

    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_GEVNTCOUNT,
                 sizeof(uint32_t));
    qtest_writel(qts, setup_trb + 8, 8);
    qtest_writel(qts, setup_trb + 12,
                 DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_LST |
                 DWC3_TRBCTL_CONTROL_SETUP);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DEPCMD(0),
                 DWC3_DEPCMD_CMDACT | DWC3_DEPCMD_DEPSTRTXFER);
    qtest_clock_step(qts, DWC3_ENUM_SETUP_DELAY_NS);
    g_assert_cmphex(qtest_readl(qts, setup_packet), ==, 0x00010900);
    g_assert_cmphex(qtest_readl(qts, event_buffer + 16), ==,
                    DWC3_DEPEVT_XFERCOMPLETE(0));

    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DCTL,
                 DWC3_DCTL_CSFTRST);
    g_assert_cmphex(qtest_readl(qts, RK3588_DWC3_BASE + DWC3_DCTL), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_DWC3_BASE + DWC3_GEVNTCOUNT),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_DWC3_BASE + DWC3_GEVNTADR_LO),
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_DWC3_BASE + DWC3_GEVNTSIZ),
                    ==, 0);

    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_GEVNTADR_LO,
                 event_buffer);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_GEVNTADR_HI, 0);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_GEVNTSIZ, 64);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DCTL,
                 DWC3_DCTL_RUNSTOP);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_GEVNTCOUNT,
                 2 * sizeof(uint32_t));

    qtest_writel(qts, setup_trb + 8, 8);
    qtest_writel(qts, setup_trb + 12,
                 DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_LST |
                 DWC3_TRBCTL_CONTROL_SETUP);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DEPCMDPAR1(0), setup_trb);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DEPCMDPAR0(0), 0);
    qtest_writel(qts, RK3588_DWC3_BASE + DWC3_DEPCMD(0),
                 DWC3_DEPCMD_CMDACT | DWC3_DEPCMD_DEPSTRTXFER);
    qtest_clock_step(qts, DWC3_ENUM_SETUP_DELAY_NS);
    g_assert_cmphex(qtest_readl(qts, setup_packet), ==, 0x00010500);
    g_assert_cmphex(qtest_readl(qts, event_buffer + 8), ==,
                    DWC3_DEPEVT_XFERCOMPLETE(0));

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

static uint8_t sfc_pattern_byte(uint32_t addr)
{
    return (uint8_t)(addr * 7 + 3);
}

static void test_rock_5b_plus_sfc_flash(void)
{
    /* The w25q128 model requires a full-size backing file. */
    static const size_t flash_size = 16 * 1024 * 1024;
    g_autofree uint8_t *pattern = g_malloc(flash_size);
    g_autofree char *flash_path = NULL;
    g_autoptr(GError) error = NULL;
    QTestState *qts;
    int flash_fd;
    gsize readback_size = 0;

    for (size_t i = 0; i < flash_size; i++) {
        pattern[i] = sfc_pattern_byte(i);
    }

    flash_fd = g_file_open_tmp("rock5b-plus-sfc-XXXXXX", &flash_path,
                                &error);
    g_assert_no_error(error);
    g_assert_cmpint(flash_fd, >=, 0);
    g_assert_cmpint(close(flash_fd), ==, 0);
    g_assert_true(g_file_set_contents(flash_path, (const char *)pattern,
                                      flash_size, &error));
    g_assert_no_error(error);

    qts = qtest_initf("-machine " ROCK_5B_PLUS_MACHINE
                      " -smp 1 -m 512M"
                      " -drive if=mtd,index=0,file=%s,format=raw",
                      flash_path);

    /* Controller reset and version. */
    qtest_writel(qts, RK3588_SFC_BASE + SFC_RCVR, 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_RCVR), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_VER), ==, 4);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CTRL, 0);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_LEN_CTRL, 1);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ICLR, 0xffffffff);

    /* JEDEC ID through the PIO FIFO: w25q128 = ef 40 18. */
    qtest_writel(qts, RK3588_SFC_BASE + SFC_LEN_EXT, 3);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD, SFC_OP_JEDEC_ID);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_FSR) &
                    SFC_FSR_RXLV_MASK, !=, 0);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_DATA) &
                    0xffffff, ==, 0x1840ef);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);

    /* PIO read of the flash contents at 0x100. */
    qtest_writel(qts, RK3588_SFC_BASE + SFC_LEN_EXT, 4);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD,
                 SFC_OP_READ | SFC_CMD_ADDR_24);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ADDR, 0x100);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_DATA), ==,
                    (uint32_t)sfc_pattern_byte(0x100) |
                    ((uint32_t)sfc_pattern_byte(0x101) << 8) |
                    ((uint32_t)sfc_pattern_byte(0x102) << 16) |
                    ((uint32_t)sfc_pattern_byte(0x103) << 24));
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);

    /* DMA read of 4 bytes from 0x300 into RAM. */
    qtest_writel(qts, RK3588_SFC_BASE + SFC_LEN_EXT, 4);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD,
                 SFC_OP_READ | SFC_CMD_ADDR_24);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ADDR, 0x300);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_DMA_ADDR, RK3588_RAM_BASE);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ICLR, 0xffffffff);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_DMA_TRIGGER, 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_RISR) &
                    SFC_RISR_DMA, ==, SFC_RISR_DMA);
    g_assert_cmphex(qtest_readl(qts, RK3588_RAM_BASE), ==,
                    (uint32_t)sfc_pattern_byte(0x300) |
                    ((uint32_t)sfc_pattern_byte(0x301) << 8) |
                    ((uint32_t)sfc_pattern_byte(0x302) << 16) |
                    ((uint32_t)sfc_pattern_byte(0x303) << 24));
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ICLR, SFC_RISR_DMA);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_RISR) &
                    SFC_RISR_DMA, ==, 0);

    /*
     * 4K erase at 0x200 then page program 0x0a0b0c0d and read it back.
     * NOR flash programming can only clear bits, so the sector must be
     * erased (all 0xff) before the pattern is written.
     */
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD, SFC_OP_WREN);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD,
                 SFC_OP_ERASE_4K | SFC_CMD_DIR_WR | SFC_CMD_ADDR_24);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ADDR, 0x200);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);

    qtest_writel(qts, RK3588_SFC_BASE + SFC_LEN_EXT, 4);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD,
                 SFC_OP_READ | SFC_CMD_ADDR_24);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ADDR, 0x200);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_DATA), ==,
                    UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);

    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD, SFC_OP_WREN);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_LEN_EXT, 4);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD,
                 SFC_OP_PAGE_PROG | SFC_CMD_DIR_WR | SFC_CMD_ADDR_24);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ADDR, 0x200);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_DATA, 0x0a0b0c0d);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);

    qtest_writel(qts, RK3588_SFC_BASE + SFC_LEN_EXT, 4);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD,
                 SFC_OP_READ | SFC_CMD_ADDR_24);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ADDR, 0x200);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_DATA), ==,
                    0x0a0b0c0d);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);

    /* DMA write of RAM contents to flash at 0x1400 (erase first). */
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD, SFC_OP_WREN);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD,
                 SFC_OP_ERASE_4K | SFC_CMD_DIR_WR | SFC_CMD_ADDR_24);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ADDR, 0x1400);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);
    qtest_writel(qts, RK3588_RAM_BASE, 0x11223344);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD, SFC_OP_WREN);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_LEN_EXT, 4);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD,
                 SFC_OP_PAGE_PROG | SFC_CMD_DIR_WR | SFC_CMD_ADDR_24);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ADDR, 0x1400);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_DMA_ADDR, RK3588_RAM_BASE);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_DMA_TRIGGER, 1);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_RISR) &
                    SFC_RISR_DMA, ==, SFC_RISR_DMA);

    qtest_writel(qts, RK3588_SFC_BASE + SFC_LEN_EXT, 4);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD,
                 SFC_OP_READ | SFC_CMD_ADDR_24);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ADDR, 0x1400);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_DATA), ==,
                    0x11223344);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);

    /* Re-erase 0x200: contents return to 0xff. */
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD, SFC_OP_WREN);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD,
                 SFC_OP_ERASE_4K | SFC_CMD_DIR_WR | SFC_CMD_ADDR_24);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ADDR, 0x200);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);

    qtest_writel(qts, RK3588_SFC_BASE + SFC_LEN_EXT, 4);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_CMD,
                 SFC_OP_READ | SFC_CMD_ADDR_24);
    qtest_writel(qts, RK3588_SFC_BASE + SFC_ADDR, 0x200);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_DATA), ==,
                    UINT32_MAX);
    g_assert_cmphex(qtest_readl(qts, RK3588_SFC_BASE + SFC_SR), ==, 0);

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
    qtest_add_func("/rock-5b-plus/zephyr-ram",
                   test_rock_5b_plus_zephyr_ram);
    qtest_add_func("/rock-5b-plus/zephyr-uimage",
                   test_rock_5b_plus_zephyr_uimage);
    qtest_add_func("/rock-5b-plus/dwc3-device",
                   test_rock_5b_plus_dwc3_device);
    qtest_add_func("/rock-5b-plus/pcie3x2-fdt",
                   test_rock_5b_plus_pcie3x2_fdt);
    qtest_add_func("/rock-5b-plus/pcie3x2-bus-number",
                   test_rock_5b_plus_pcie3x2_bus_number);
    qtest_add_func("/rock-5b-plus/unfused-secure-otp",
                   test_rock_5b_plus_unfused_secure_otp);
    qtest_add_func("/rock-5b-plus/crypto-sha256",
                   test_rock_5b_plus_crypto_sha256);
    qtest_add_func("/rock-5b-plus/sfc-flash",
                   test_rock_5b_plus_sfc_flash);

    return g_test_run();
}
