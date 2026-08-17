/*
 * QTest for the Radxa ROCK 5B+ machine
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
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
#define DWC_PCIE_ATU_TYPE_MEM 0x0
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

/* ---------------- pcie2x1l0 virtio-net ---------------- */

#define RK3588_PCIE2X1L0_DBI_BASE 0xa40800000ULL
#define RK3588_PCIE2X1L0_CFG_BASE 0xf2000000ULL
#define PCIE2X1L0_NET_BDF (0x21 << 24)
#define PCIE2X1L0_MEM_WIN 0xf3000000ULL
#define PCIE2X1L0_BAR_BASE 0x10000000ULL
#define NET_RING_BASE 0x01000000ULL
#define NET_BUF_BASE 0x01100000ULL
#define VIRTIO_NET_HDR_SIZE 10
#define VRING_DESC_F_WRITE BIT(1)
#define VIRTIO_PCI_CAP_COMMON_CFG 1
#define VIRTIO_PCI_CAP_NOTIFY_CFG 2
#define VIRTIO_PCI_CAP_ISR_CFG 3
#define VPC_COMMON_DEVICE_STATUS 0x14
#define VPC_COMMON_DEVICE_FEATURE_SELECT 0x00
#define VPC_COMMON_DEVICE_FEATURE 0x04
#define VPC_COMMON_GUEST_FEATURE_SELECT 0x08
#define VPC_COMMON_GUEST_FEATURE 0x0c
#define VPC_COMMON_QUEUE_SELECT 0x16
#define VPC_COMMON_QUEUE_SIZE 0x18
#define VPC_COMMON_QUEUE_ENABLE 0x1c
#define VPC_COMMON_QUEUE_NOTIFY_OFF 0x1e
#define VPC_COMMON_QUEUE_DESC_LO 0x20
#define VPC_COMMON_QUEUE_AVL_LO 0x28
#define VPC_COMMON_QUEUE_USED_LO 0x30

static void pcie2x1l0_atu_cfg0(QTestState *qts, uint32_t target_bdf)
{
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_VIEWPORT, 0);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_CR1,
                 DWC_PCIE_ATU_TYPE_CFG0);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_LOWER_BASE,
                 RK3588_PCIE2X1L0_CFG_BASE);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_UPPER_BASE, 0);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_LIMIT,
                 RK3588_PCIE2X1L0_CFG_BASE + 0xfffff);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_LOWER_TARGET,
                 target_bdf);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_UPPER_TARGET, 0);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_CR2,
                 DWC_PCIE_ATU_ENABLE);
}

static void pcie2x1l0_atu_mem(QTestState *qts, uint32_t win_base,
                              uint32_t target)
{
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_VIEWPORT, 1);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_CR1,
                 DWC_PCIE_ATU_TYPE_MEM);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_LOWER_BASE,
                 win_base);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_UPPER_BASE, 0);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_LIMIT,
                 win_base + 0xfffff);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_LOWER_TARGET,
                 target);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_UPPER_TARGET, 0);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_CR2,
                 DWC_PCIE_ATU_ENABLE);
}

/* Program an inbound (PCI -> system memory) ATU viewport for DMA. */
static void pcie2x1l0_atu_inbound(QTestState *qts, uint32_t pci_base,
                                  uint32_t pci_limit, uint32_t target)
{
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_VIEWPORT,
                 0x80000000U);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_CR1,
                 DWC_PCIE_ATU_TYPE_MEM);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_LOWER_BASE,
                 pci_base);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_UPPER_BASE, 0);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_LIMIT,
                 pci_limit);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_LOWER_TARGET,
                 target);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_UPPER_TARGET, 0);
    qtest_writel(qts, RK3588_PCIE2X1L0_DBI_BASE + DWC_PCIE_ATU_CR2,
                 DWC_PCIE_ATU_ENABLE);
}

static void pcie2x1l0_write_cfg(QTestState *qts, uint32_t offset,
                                uint32_t value, unsigned size)
{
    if (size == 4) {
        qtest_writel(qts, RK3588_PCIE2X1L0_CFG_BASE + offset, value);
    } else if (size == 2) {
        qtest_writew(qts, RK3588_PCIE2X1L0_CFG_BASE + offset, value);
    } else {
        qtest_writeb(qts, RK3588_PCIE2X1L0_CFG_BASE + offset, value);
    }
}

static uint32_t pcie2x1l0_read_cfg(QTestState *qts, uint32_t offset,
                                   unsigned size)
{
    if (size == 4) {
        return qtest_readl(qts, RK3588_PCIE2X1L0_CFG_BASE + offset);
    } else if (size == 2) {
        return qtest_readw(qts, RK3588_PCIE2X1L0_CFG_BASE + offset);
    }
    return qtest_readb(qts, RK3588_PCIE2X1L0_CFG_BASE + offset);
}

/* BAR-relative access through the outbound MEM viewport. */
static uint32_t win_addr(uint32_t bar_addr, uint32_t offset)
{
    return PCIE2X1L0_MEM_WIN + (bar_addr - PCIE2X1L0_BAR_BASE) + offset;
}

static void socket_send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    while (len) {
        ssize_t n = send(fd, p, len, 0);

        g_assert_cmpint(n, >, 0);
        p += n;
        len -= n;
    }
}

static ssize_t socket_recv_some(int fd, void *buf, size_t len)
{
    struct timeval tv = { .tv_sec = 30, .tv_usec = 0 };
    fd_set rfds;

    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0) {
        return -1;
    }
    return recv(fd, buf, len, 0);
}

static void test_rock_5b_plus_sdhci_cmd1(void)
{
    gchar *card = g_strdup("/tmp/rock5b-sdhci-card-XXXXXX");
    QTestState *qts;
    const uint32_t b = 0xfe2e0000ULL;
    int fd = mkstemp(card);

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    qts = qtest_initf("-machine " ROCK_5B_PLUS_MACHINE
                      " -smp 1 -m 512M"
                      " -drive if=sd,index=0,file=%s,format=raw", card);
    unlink(card);
    g_free(card);

    /* Power on the eMMC SDHCI controller, unmask the command-complete
     * interrupt, and check the card slot reports the inserted card. */
    qtest_writeb(qts, b + 0x29, 0x0f);   /* SDHCI_POWER_CONTROL */
    qtest_writel(qts, b + 0x2c, 0x0007); /* SDHCI_CLKCON: clocks on */
    qtest_writel(qts, b + 0x34, 0x0001); /* NORINTSTSEN: CMDCMP */
    g_assert_cmphex(qtest_readl(qts, b + 0x24) & 0x00010000,
                    ==, 0x00010000);     /* card present */

    /* Issue CMD1 (SEND_OP_COND) with a 48-bit response; the controller
     * completes the command and raises the command-complete status. */
    qtest_writel(qts, b + 0x08, 0x00000000);  /* argument */
    qtest_writel(qts, b + 0x0e, (1 << 8) | 1); /* CMD1 | 48-bit resp */
    g_assert_cmphex(qtest_readl(qts, b + 0x30) & 0x0001, ==, 0x0001);

    qtest_quit(qts);
}

static void test_rock_5b_plus_pcie2x1l0_virtio_net(void)
{
    static const uint8_t tx_frame[60] = {
        0x52, 0x54, 0x00, 0x12, 0x34, 0x56, /* dst: device MAC */
        0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, /* src */
        0x08, 0x00,                         /* ethertype */
        'R', 'O', 'C', 'K', '5', 'B', '+', ' ', 'n', 'e', 't', 't',
        'e', 's', 't', ' ', 'f', 'r', 'a', 'm', 'e', ' ', '0', '0',
        '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0',
        '0', '0', '0', '0', '0', '0', '0', '0', '0', '0',
    };
    uint8_t tx_buf[VIRTIO_NET_HDR_SIZE + sizeof(tx_frame)];
    uint8_t recv_buf[2048];
    uint32_t bar_addr[6] = {
        0x10000000, 0x10001000, 0x10002000, 0x10003000, 0x10004000, 0x10005000,
    };
    uint32_t common_cfg = 0, notify_cfg = 0, isr_cfg = 0;
    uint32_t notify_mult = 4, notify_off = 0;
    uint32_t msix_table_bar = 0, msix_table_off = 0;
    uint32_t net_frame_len = sizeof(tx_frame);
    uint32_t wire_len;
    uint64_t used_ring, avail_ring, desc_ring;
    QTestState *qts;
    int sv[2];
    int i, cap;

    g_assert_cmpint(socketpair(PF_UNIX, SOCK_STREAM, 0, sv), ==, 0);

    qts = qtest_initf("-machine " ROCK_5B_PLUS_MACHINE
                      " -smp 1 -m 512M"
                      " -netdev socket,fd=%d,id=hs0"
                      " -device virtio-net-pci,bus=/pcie2x1l0/pcie/"
                      "designware-pcie-root/dw-pcie,netdev=hs0",
                      sv[1]);
    close(sv[1]);

    /* Put the VM in the running state so the virtio RX path is live. */
    {
        QDict *rsp = qtest_qmp(qts, "{ 'execute': 'cont' }");

        qobject_unref(rsp);
    }

    /* Program the CFG0 ATU and configure the root bridge: secondary
     * bus number and memory window for the endpoint BARs (a guest
     * kernel does the same during enumeration).  PCI_COMMAND was
     * cleared by the machine reset; re-enable MEM|MASTER. */
    pcie2x1l0_atu_cfg0(qts, 0x20 << 24);
    pcie2x1l0_write_cfg(qts, 0x19, 0x21, 1); /* PCI_SECONDARY_BUS */
    pcie2x1l0_write_cfg(qts, 0x1a, 0x21, 1); /* PCI_SUBORDINATE_BUS */
    pcie2x1l0_write_cfg(qts, 0x04, 0x6, 2);  /* PCI_COMMAND */
    pcie2x1l0_write_cfg(qts, 0x20, 0x1000, 2); /* PCI_MEMORY_BASE */
    pcie2x1l0_write_cfg(qts, 0x22, 0x100f, 2); /* PCI_MEMORY_LIMIT */

    /* Read the endpoint config space through the ATU data window. */
    pcie2x1l0_atu_cfg0(qts, PCIE2X1L0_NET_BDF);
    g_assert_cmphex(pcie2x1l0_read_cfg(qts, 0x00, 2), ==, 0x1af4);
    g_assert_cmphex(pcie2x1l0_read_cfg(qts, 0x02, 2), ==, 0x1041);
    g_assert_cmphex(pcie2x1l0_read_cfg(qts, 0x0b, 1), ==, 0x02);

    /* Walk the capabilities: virtio transport + MSI-X. */
    cap = pcie2x1l0_read_cfg(qts, 0x34, 1);
    while (cap) {
        uint8_t id = pcie2x1l0_read_cfg(qts, cap, 1);

        if (id == 0x09) { /* vendor: virtio */
            uint8_t cfg_type = pcie2x1l0_read_cfg(qts, cap + 3, 1);
            uint8_t bar = pcie2x1l0_read_cfg(qts, cap + 4, 1);
            uint32_t offset = pcie2x1l0_read_cfg(qts, cap + 8, 4);

            switch (cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG:
                common_cfg = bar_addr[bar] + offset;
                break;
            case VIRTIO_PCI_CAP_NOTIFY_CFG:
                notify_cfg = bar_addr[bar] + offset;
                notify_mult = pcie2x1l0_read_cfg(qts, cap + 16, 4);
                break;
            case VIRTIO_PCI_CAP_ISR_CFG:
                isr_cfg = bar_addr[bar] + offset;
                break;
            default:
                break;
            }
        } else if (id == 0x11) { /* MSI-X */
            uint32_t table = pcie2x1l0_read_cfg(qts, cap + 4, 4);

            msix_table_bar = table & 0x7;
            msix_table_off = table & ~0x7;
        }
        cap = pcie2x1l0_read_cfg(qts, cap + 1, 1);
    }
    g_assert_cmpuint(common_cfg, !=, 0);
    g_assert_cmpuint(notify_cfg, !=, 0);
    g_assert_cmpuint(isr_cfg, !=, 0);
    g_assert_cmpuint(msix_table_bar, !=, 0);

    /* Assign BAR addresses (BAR4/5 is the 64-bit modern memory bar). */
    for (i = 0; i < 6; i++) {
        pcie2x1l0_write_cfg(qts, 0x10 + 4 * i, 0xffffffff, 4);
        pcie2x1l0_write_cfg(qts, 0x10 + 4 * i, bar_addr[i], 4);
    }
    pcie2x1l0_write_cfg(qts, 0x10 + 4 * 5, 0, 4); /* 64-bit high dword */

    /* Enable memory space + bus mastering so the BARs get mapped. */
    pcie2x1l0_write_cfg(qts, 0x04, 0x6, 2);

    /* Map the BARs through the outbound MEM viewport. */
    pcie2x1l0_atu_mem(qts, PCIE2X1L0_MEM_WIN, PCIE2X1L0_BAR_BASE);

    /* DMA (vrings + buffers) lives in guest RAM: map the PCI-side
     * range to system memory through inbound ATU viewport 0. */
    pcie2x1l0_atu_inbound(qts, NET_RING_BASE, NET_BUF_BASE + 0x3000 - 1,
                          NET_RING_BASE);

    /* ---- virtio driver: reset, features, DRIVER_OK ---- */
    qtest_writeb(qts, win_addr(common_cfg, VPC_COMMON_DEVICE_STATUS), 0);
    qtest_writeb(qts, win_addr(common_cfg, VPC_COMMON_DEVICE_STATUS), 0x3);
    g_assert_cmphex(qtest_readb(qts, win_addr(common_cfg,
                                              VPC_COMMON_DEVICE_STATUS)),
                    ==, 0x3);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_DEVICE_FEATURE_SELECT), 0);
    g_assert_cmphex(qtest_readl(qts, win_addr(common_cfg,
                                              VPC_COMMON_DEVICE_FEATURE)),
                    !=, 0);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_GUEST_FEATURE_SELECT), 0);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_GUEST_FEATURE), 0);
    qtest_writeb(qts, win_addr(common_cfg, VPC_COMMON_DEVICE_STATUS), 0xb);
    g_assert_cmphex(qtest_readb(qts, win_addr(common_cfg,
                                              VPC_COMMON_DEVICE_STATUS)),
                    ==, 0xb);
    qtest_writeb(qts, win_addr(common_cfg, VPC_COMMON_DEVICE_STATUS), 0xf);

    /* ---- RX queue 0 ---- */
    desc_ring = NET_RING_BASE;
    avail_ring = NET_RING_BASE + 0x1000;
    used_ring = NET_RING_BASE + 0x2000;
    qtest_writew(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_SELECT), 0);
    qtest_writew(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_SIZE), 64);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_DESC_LO),
                 desc_ring);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_DESC_LO) + 4, 0);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_AVL_LO),
                 avail_ring);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_AVL_LO) + 4, 0);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_USED_LO),
                 used_ring);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_USED_LO) + 4, 0);
    qtest_writew(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_ENABLE), 1);

    /* One RX buffer: desc 0 -> NET_BUF_BASE, 2048 bytes, WRITE. */
    qtest_writeq(qts, desc_ring, NET_BUF_BASE);
    qtest_writel(qts, desc_ring + 8, 2048);
    qtest_writew(qts, desc_ring + 12, VRING_DESC_F_WRITE);
    qtest_writew(qts, desc_ring + 14, 0);
    qtest_writew(qts, avail_ring, 0);      /* flags */
    qtest_writew(qts, avail_ring + 2, 1);  /* idx */
    qtest_writew(qts, avail_ring + 4, 0);  /* ring[0] */

    /* Kick queue 0. */
    qtest_writel(qts, win_addr(notify_cfg, 0), 0);

    /* Inject a frame from the host side. */
    wire_len = GUINT32_TO_BE(net_frame_len);
    socket_send_all(sv[0], &wire_len, sizeof(wire_len));
    socket_send_all(sv[0], tx_frame, sizeof(tx_frame));

    /* Wait for the used ring. */
    for (i = 0; i < 30000 && qtest_readw(qts, used_ring + 2) == 0; i++) {
        g_usleep(1000);
    }
    g_assert_cmpuint(qtest_readw(qts, used_ring + 2), ==, 1);
    g_assert_cmphex(qtest_readl(qts, used_ring + 4), ==, 0); /* id */
    g_assert_cmpuint(qtest_readl(qts, used_ring + 8),
                     ==, net_frame_len + VIRTIO_NET_HDR_SIZE);

    /* The frame follows the virtio header in the RX buffer. */
    for (i = 0; i < (int)net_frame_len; i++) {
        g_assert_cmphex(qtest_readb(qts, NET_BUF_BASE + VIRTIO_NET_HDR_SIZE + i),
                        ==, tx_frame[i]);
    }

    /* ---- TX queue 1 ---- */
    desc_ring = NET_RING_BASE + 0x3000;
    avail_ring = NET_RING_BASE + 0x4000;
    used_ring = NET_RING_BASE + 0x5000;
    qtest_writew(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_SELECT), 1);
    qtest_writew(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_SIZE), 64);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_DESC_LO),
                 desc_ring);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_DESC_LO) + 4, 0);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_AVL_LO),
                 avail_ring);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_AVL_LO) + 4, 0);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_USED_LO),
                 used_ring);
    qtest_writel(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_USED_LO) + 4, 0);
    qtest_writew(qts, win_addr(common_cfg, VPC_COMMON_QUEUE_ENABLE), 1);
    notify_off = qtest_readw(qts, win_addr(common_cfg,
                                           VPC_COMMON_QUEUE_NOTIFY_OFF));

    /* TX buffer: virtio header + frame. */
    memset(tx_buf, 0, sizeof(tx_buf));
    memcpy(tx_buf + VIRTIO_NET_HDR_SIZE, tx_frame, sizeof(tx_frame));
    for (i = 0; i < (int)sizeof(tx_buf); i += 4) {
        qtest_writel(qts, NET_BUF_BASE + 0x1000 + i, ldl_he_p(&tx_buf[i]));
    }
    qtest_writeq(qts, desc_ring, NET_BUF_BASE + 0x1000);
    qtest_writel(qts, desc_ring + 8, sizeof(tx_buf));
    qtest_writew(qts, desc_ring + 12, 0);  /* flags: read-only */
    qtest_writew(qts, desc_ring + 14, 0);
    qtest_writew(qts, avail_ring, 0);
    qtest_writew(qts, avail_ring + 2, 1);
    qtest_writew(qts, avail_ring + 4, 0);

    /* Kick queue 1 (notify offset from the common config). */
    qtest_writel(qts, win_addr(notify_cfg, notify_off * notify_mult), 0);

    /* The frame arrives on the host socket as [len][payload]. */
    {
        ssize_t n = socket_recv_some(sv[0], recv_buf, sizeof(recv_buf));

        g_assert_cmpint(n, >, 0);
        g_assert_cmpuint(GUINT32_FROM_BE(ldl_he_p(recv_buf)),
                         ==, net_frame_len);
        g_assert_cmpint(n, ==, 4 + (ssize_t)net_frame_len);
        g_assert_cmpmem(recv_buf + 4, net_frame_len, tx_frame,
                        sizeof(tx_frame));
    }

    /* The used ring for TX advances too. */
    for (i = 0; i < 30000 && qtest_readw(qts, used_ring + 2) == 0; i++) {
        g_usleep(1000);
    }
    g_assert_cmpuint(qtest_readw(qts, used_ring + 2), ==, 1);

    /* MSI-X table is reachable and writable (msg addr + mask control). */
    {
        uint32_t t = win_addr(bar_addr[msix_table_bar], msix_table_off);

        qtest_writel(qts, t, 0x12345678); /* vector 0 message address */
        g_assert_cmphex(qtest_readl(qts, t), ==, 0x12345678);
        qtest_writel(qts, t + 8, 1);      /* vector 0 control: masked */
        g_assert_cmphex(qtest_readl(qts, t + 8) & 1, ==, 1);
        qtest_writel(qts, t + 8, 0);
        g_assert_cmphex(qtest_readl(qts, t + 8) & 1, ==, 0);
    }

    close(sv[0]);
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
    qtest_add_func("/rock-5b-plus/sdhci-cmd1",
                   test_rock_5b_plus_sdhci_cmd1);
    qtest_add_func("/rock-5b-plus/pcie2x1l0-virtio-net",
                   test_rock_5b_plus_pcie2x1l0_virtio_net);

    return g_test_run();
}
