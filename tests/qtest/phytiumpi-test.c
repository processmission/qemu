/*
 * QTest for the local-only Phytium Pi board model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qemu/bswap.h"
#include "libqtest.h"

#define PHYTIUMPI_MACHINE "phytium-pi"
#define PHYTIUMPI_RAM_BASE 0x80000000ULL
#define PHYTIUMPI_HIGH_RAM_BASE 0x2000000000ULL
#define PHYTIUMPI_MCI_BASE 0x28000000ULL
#define PHYTIUMPI_MCI1_BASE 0x28001000ULL
#define PHYTIUMPI_UART_BASE 0x2802a000ULL
#define PHYTIUMPI_GICD_BASE 0x30800000ULL
#define PHYTIUMPI_GICR_BASE 0x30880000ULL
#define PHYTIUMPI_GIC_ITS_BASE 0x30820000ULL
#define PHYTIUMPI_GIC_CPU_BASE 0x30840000ULL
#define PHYTIUMPI_SCP_MAILBOX_BASE 0x32a10400ULL
#define PHYTIUMPI_FW_USB2_CLUSTER_BASE 0x31800000ULL
#define PHYTIUMPI_FW_PHY_CFG_BASE 0x31b00000ULL
#define PHYTIUMPI_FW_PHY_CFG0_BASE 0x32000000ULL
#define PHYTIUMPI_FW_PHY_CFG1_BASE 0x32100000ULL
#define PHYTIUMPI_FW_USB2_LOW_BASE 0x32800000ULL
#define PHYTIUMPI_FW_USB2_HIGH_BASE 0x32880000ULL
#define PHYTIUMPI_DDR_CTRL_BASE 0x32b33000ULL
#define PHYTIUMPI_XMAC_BASE 0x3200c000ULL
#define PHYTIUMPI_RNG_BASE 0x32a36000ULL
#define PHYTIUMPI_PCIE_ECAM_BASE 0x40000000ULL

#define GICD_TYPER 0x0004
#define GICR_TYPER 0x0008
#define PL011_FR 0x0018
#define PL011_FR_TXFE 0x80
#define TEST_SD_SIZE (1 << 20)

#define MCI_CNTRL 0x000
#define MCI_CNTRL_CONTROLLER_RESET 0x00000001
#define MCI_CNTRL_FIFO_RESET 0x00000002
#define MCI_CNTRL_DMA_RESET 0x00000004
#define MCI_CNTRL_INT_ENABLE 0x00000010
#define MCI_CNTRL_DMA_ENABLE 0x00000020
#define MCI_CNTRL_USE_INTERNAL_DMAC 0x02000000
#define MCI_BLKSIZ 0x01c
#define MCI_BYTCNT 0x020
#define MCI_INT_MASK 0x024
#define MCI_CMDARG 0x028
#define MCI_CMD 0x02c
#define MCI_CMD_START 0x80000000
#define MCI_CMD_DAT_EXP 0x00000200
#define MCI_CMD_RESP_LONG 0x00000080
#define MCI_CMD_RESP_EXP 0x00000040
#define MCI_RESP0 0x030
#define MCI_MASKED_INTS 0x040
#define MCI_RAW_INTS 0x044
#define MCI_STATUS 0x048
#define MCI_STATUS_FIFO_EMPTY 0x00000004
#define MCI_CARD_DETECT 0x050
#define MCI_CCLK_RDY 0x058
#define MCI_CARD_RESET 0x078
#define MCI_BUS_MODE 0x080
#define MCI_BUS_MODE_DE 0x00000080
#define MCI_DESC_LIST_ADDRL 0x088
#define MCI_DESC_LIST_ADDRH 0x08c
#define MCI_DMAC_STATUS 0x090
#define MCI_DMAC_INT_ENA 0x094
#define MCI_DATA 0x200

#define MCI_INT_CMD 0x00000004
#define MCI_INT_DTO 0x00000008
#define MCI_INT_RTO 0x00000100
#define MCI_INT_DRTO 0x00000200
#define MCI_DMAC_STATUS_RI 0x00000002
#define MCI_DMAC_STATUS_NIS 0x00000100

#define ADMA_ATTR_LD 0x00000004
#define ADMA_ATTR_FD 0x00000008
#define ADMA_ATTR_OWN 0x80000000

#define SCP_MAILBOX_STATUS 0x004
#define SCP_MAILBOX_CONTROL 0x010
#define SCP_MAILBOX_LENGTH 0x014
#define SCP_MAILBOX_COMMAND 0x018
#define SCP_MAILBOX_RESPONSE 0x01c
#define SCP_MAILBOX_DONE 0x1

#define DDR_CTRL_INDEX 0x080
#define DDR_CTRL_DATA 0x084
#define DDR_CTRL_POLL_INDEX ((0x76 + 0x800) << 2)
#define DDR_CTRL_SAMPLE_INDEX 0x5de0
#define DDR_CTRL_OTHER_INDEX 0x5dd0
#define DDR_CTRL_READY_INDEX ((0xdc + 0x800) << 2)
#define DDR_CTRL_DONE (1u << 27)
#define DDR_CTRL_READY_BITS (DDR_CTRL_DONE | (1u << 25) | 1u)

#define XMAC_NETWORK_STATUS 0x008
#define XMAC_PHY_MAINTENANCE 0x034
#define XMAC_PCS_STATUS 0x214
#define XMAC_MDIO_IDLE 0x00000004
#define XMAC_PCS_LINK_UP 0x00008000
#define XMAC_MDIO_READ(phy, reg) \
    (0x60020000 | ((phy) << 23) | ((reg) << 18))
#define XMAC_MDIO_WRITE(phy, reg, data) \
    (0x50020000 | ((phy) << 23) | ((reg) << 18) | ((data) & 0xffff))

#define PHY_BMCR 0
#define PHY_BMSR 1
#define PHY_PHYID1 2
#define PHY_PHYID2 3
#define PHY_BMCR_RESET 0x8000
#define PHY_BMSR_LINK_STATUS 0x0004
#define PHY_BMSR_AUTONEG_COMPLETE 0x0020

static QTestState *phytiumpi_qtest_start(unsigned int cpus)
{
    return qtest_initf("-machine " PHYTIUMPI_MACHINE " -smp %u -m 512M",
                       cpus);
}

static QTestState *phytiumpi_qtest_start_extra(unsigned int cpus,
                                               const char *extra)
{
    return qtest_initf("-machine " PHYTIUMPI_MACHINE " -smp %u -m 512M %s",
                       cpus, extra ? extra : "");
}

static QTestState *phytiumpi_qtest_start_ram(unsigned int cpus,
                                             const char *ram_size)
{
    return qtest_initf("-machine " PHYTIUMPI_MACHINE " -smp %u -m %s",
                       cpus, ram_size);
}

static char *phytiumpi_create_sd_image(void)
{
    g_autofree uint8_t *image = g_malloc0(TEST_SD_SIZE);
    g_autoptr(GError) error = NULL;
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("phytium-pi-sd-XXXXXX.img", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    close(fd);

    for (unsigned int i = 0; i < 512; i++) {
        image[i] = i & 0xff;
    }
    g_assert_true(g_file_set_contents(path, (const char *)image,
                                      TEST_SD_SIZE, &error));
    g_assert_no_error(error);

    return path;
}

static void phytiumpi_remove_sd_image(char *path)
{
    if (path) {
        g_unlink(path);
        g_free(path);
    }
}

static uint32_t phytiumpi_mci_cmd(QTestState *qts, uint32_t arg,
                                  uint32_t cmd)
{
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_CMDARG, arg);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_CMD, MCI_CMD_START | cmd);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_CMD) &
                    MCI_CMD_START, ==, 0);
    return qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_RESP0);
}

static uint16_t phytiumpi_xmac_phy_read(QTestState *qts, uint8_t phy,
                                        uint8_t reg)
{
    qtest_writel(qts, PHYTIUMPI_XMAC_BASE + XMAC_PHY_MAINTENANCE,
                 XMAC_MDIO_READ(phy, reg));
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_XMAC_BASE +
                                XMAC_NETWORK_STATUS) & XMAC_MDIO_IDLE,
                    ==, XMAC_MDIO_IDLE);
    return qtest_readl(qts, PHYTIUMPI_XMAC_BASE + XMAC_PHY_MAINTENANCE);
}

static void phytiumpi_xmac_phy_write(QTestState *qts, uint8_t phy,
                                     uint8_t reg, uint16_t value)
{
    qtest_writel(qts, PHYTIUMPI_XMAC_BASE + XMAC_PHY_MAINTENANCE,
                 XMAC_MDIO_WRITE(phy, reg, value));
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_XMAC_BASE +
                                XMAC_NETWORK_STATUS) & XMAC_MDIO_IDLE,
                    ==, XMAC_MDIO_IDLE);
}

static void phytiumpi_mci_clear_irqs(QTestState *qts)
{
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_RAW_INTS, 0x0001ffff);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_DMAC_STATUS, 0x00000317);
}

static void phytiumpi_mci_write_adma_desc(QTestState *qts, uint64_t desc_addr,
                                          uint64_t buf_addr, size_t len)
{
    uint8_t desc[32] = {};

    stl_le_p(desc + 0, ADMA_ATTR_OWN | ADMA_ATTR_FD | ADMA_ATTR_LD);
    stl_le_p(desc + 8, len);
    stl_le_p(desc + 16, (uint32_t)buf_addr);
    stl_le_p(desc + 20, (uint32_t)(buf_addr >> 32));
    qtest_memwrite(qts, desc_addr, desc, sizeof(desc));
}

static uint32_t phytiumpi_mci_init_card(QTestState *qts)
{
    uint32_t resp;
    uint32_t rca;

    phytiumpi_mci_cmd(qts, 0, 0);
    phytiumpi_mci_cmd(qts, 0x1aa, MCI_CMD_RESP_EXP | 8);

    for (unsigned int i = 0; i < 100; i++) {
        phytiumpi_mci_cmd(qts, 0, MCI_CMD_RESP_EXP | 55);
        resp = phytiumpi_mci_cmd(qts, 0x41200000, MCI_CMD_RESP_EXP | 41);
        if (resp & 0x80000000) {
            break;
        }
    }

    phytiumpi_mci_cmd(qts, 0, MCI_CMD_RESP_EXP | MCI_CMD_RESP_LONG | 2);
    resp = phytiumpi_mci_cmd(qts, 0, MCI_CMD_RESP_EXP | 3);
    rca = resp & 0xffff0000;
    g_assert_cmphex(rca, !=, 0);

    phytiumpi_mci_cmd(qts, rca, MCI_CMD_RESP_EXP | 7);
    phytiumpi_mci_cmd(qts, 512, MCI_CMD_RESP_EXP | 16);
    phytiumpi_mci_clear_irqs(qts);

    return rca;
}

static void test_phytiumpi_machine_creation(void)
{
    QTestState *qts = phytiumpi_qtest_start(1);
    uint64_t pattern = 0x1122334455667788ULL;
    uint32_t gicd_typer;
    uint32_t uart_fr;

    qtest_writeq(qts, PHYTIUMPI_RAM_BASE, pattern);
    g_assert_cmphex(qtest_readq(qts, PHYTIUMPI_RAM_BASE), ==, pattern);

    gicd_typer = qtest_readl(qts, PHYTIUMPI_GICD_BASE + GICD_TYPER);
    g_assert_cmpuint(gicd_typer & 0x1f, >=, 0x7);

    qtest_readq(qts, PHYTIUMPI_GICR_BASE + GICR_TYPER);

    uart_fr = qtest_readl(qts, PHYTIUMPI_UART_BASE + PL011_FR);
    g_assert_cmphex(uart_fr & PL011_FR_TXFE, ==, PL011_FR_TXFE);

    qtest_readl(qts, PHYTIUMPI_GIC_ITS_BASE);
    qtest_readl(qts, PHYTIUMPI_GIC_CPU_BASE);
    qtest_readl(qts, PHYTIUMPI_PCIE_ECAM_BASE);
    qtest_writel(qts, PHYTIUMPI_RNG_BASE + 4, 0xfeedface);
    qtest_readl(qts, PHYTIUMPI_RNG_BASE + 4);

    qtest_writel(qts, PHYTIUMPI_FW_USB2_CLUSTER_BASE + 0x1c0000,
                 0x11223344);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_FW_USB2_CLUSTER_BASE + 0x1c0000),
                    ==, 0x11223344);
    qtest_writel(qts, PHYTIUMPI_FW_PHY_CFG_BASE + 0x40254, 0xaabbccdd);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_FW_PHY_CFG_BASE + 0x40254),
                    ==, 0xaabbccdd);
    qtest_writel(qts, PHYTIUMPI_FW_PHY_CFG0_BASE + 0xe5a0, 0x05060708);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_FW_PHY_CFG0_BASE + 0xe5a0),
                    ==, 0x05060708);
    qtest_writel(qts, PHYTIUMPI_FW_PHY_CFG1_BASE + 0x40254, 0xddccbbaa);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_FW_PHY_CFG1_BASE + 0x40254),
                    ==, 0xddccbbaa);
    qtest_writel(qts, PHYTIUMPI_FW_PHY_CFG1_BASE + 0x14008c, 0x01020304);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_FW_PHY_CFG1_BASE + 0x14008c),
                    ==, 0x01020304);
    qtest_writeb(qts, PHYTIUMPI_FW_USB2_LOW_BASE + 0x18d, 0xa5);
    g_assert_cmphex(qtest_readb(qts, PHYTIUMPI_FW_USB2_LOW_BASE + 0x18d),
                    ==, 0xa5);
    qtest_writel(qts, PHYTIUMPI_FW_USB2_HIGH_BASE + 0x7fffc, 0x55667788);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_FW_USB2_HIGH_BASE + 0x7fffc),
                    ==, 0x55667788);

    qtest_writel(qts, PHYTIUMPI_SCP_MAILBOX_BASE + SCP_MAILBOX_RESPONSE, 1);
    qtest_writel(qts, PHYTIUMPI_SCP_MAILBOX_BASE + SCP_MAILBOX_STATUS, 0);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_SCP_MAILBOX_BASE +
                                SCP_MAILBOX_STATUS) & SCP_MAILBOX_DONE,
                    ==, SCP_MAILBOX_DONE);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_SCP_MAILBOX_BASE +
                                SCP_MAILBOX_RESPONSE), ==, 0);

    qtest_writel(qts, PHYTIUMPI_SCP_MAILBOX_BASE + SCP_MAILBOX_COMMAND,
                 0x5006);
    qtest_writel(qts, PHYTIUMPI_SCP_MAILBOX_BASE + SCP_MAILBOX_LENGTH, 8);
    qtest_writel(qts, PHYTIUMPI_SCP_MAILBOX_BASE + SCP_MAILBOX_RESPONSE, 1);
    qtest_writel(qts, PHYTIUMPI_SCP_MAILBOX_BASE + SCP_MAILBOX_CONTROL, 0);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_SCP_MAILBOX_BASE +
                                SCP_MAILBOX_STATUS) & SCP_MAILBOX_DONE,
                    ==, SCP_MAILBOX_DONE);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_SCP_MAILBOX_BASE +
                                SCP_MAILBOX_RESPONSE), ==, 0);

    qtest_writel(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_DATA, 0x03000110);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_DATA),
                    ==, 0x03000110);
    qtest_writel(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_INDEX,
                 DDR_CTRL_POLL_INDEX);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_DATA),
                    ==, DDR_CTRL_DONE);
    qtest_writel(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_INDEX,
                 DDR_CTRL_SAMPLE_INDEX);
    qtest_writel(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_DATA, 0x18f00108);
    qtest_writel(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_INDEX,
                 DDR_CTRL_OTHER_INDEX);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_DATA),
                    ==, DDR_CTRL_READY_BITS);
    qtest_writel(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_DATA, 0);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_DATA),
                    ==, DDR_CTRL_READY_BITS);
    qtest_writel(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_INDEX,
                 DDR_CTRL_READY_INDEX);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_DDR_CTRL_BASE + DDR_CTRL_DATA),
                    ==, DDR_CTRL_READY_BITS);

    qtest_quit(qts);
}

static void test_phytiumpi_highmem_creation(void)
{
    QTestState *qts = phytiumpi_qtest_start_ram(1, "2304M");
    uint64_t low_pattern = 0x1122334455667788ULL;
    uint64_t high_pattern = 0x8877665544332211ULL;

    qtest_writeq(qts, PHYTIUMPI_RAM_BASE, low_pattern);
    qtest_writeq(qts, PHYTIUMPI_HIGH_RAM_BASE, high_pattern);
    g_assert_cmphex(qtest_readq(qts, PHYTIUMPI_RAM_BASE), ==, low_pattern);
    g_assert_cmphex(qtest_readq(qts, PHYTIUMPI_HIGH_RAM_BASE), ==,
                    high_pattern);

    qtest_quit(qts);
}

static void test_phytiumpi_mci_registers(void)
{
    char *sd_path = phytiumpi_create_sd_image();
    g_autofree char *args = g_strdup_printf(
        "-drive if=sd,file=%s,format=raw,auto-read-only=off", sd_path);
    QTestState *qts = phytiumpi_qtest_start_extra(1, args);
    uint32_t ctrl;

    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_CCLK_RDY) & 1,
                    ==, 1);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_CARD_RESET), ==,
                    1);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_CARD_DETECT) & 1,
                    ==, 0);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_STATUS) &
                    MCI_STATUS_FIFO_EMPTY, ==, MCI_STATUS_FIFO_EMPTY);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI1_BASE + MCI_CCLK_RDY) & 1,
                    ==, 1);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI1_BASE + MCI_CARD_DETECT) &
                    1, ==, 1);

    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_CNTRL,
                 MCI_CNTRL_CONTROLLER_RESET | MCI_CNTRL_FIFO_RESET |
                 MCI_CNTRL_DMA_RESET);
    ctrl = qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_CNTRL);
    g_assert_cmphex(ctrl & (MCI_CNTRL_CONTROLLER_RESET |
                            MCI_CNTRL_FIFO_RESET |
                            MCI_CNTRL_DMA_RESET), ==, 0);
    qtest_writel(qts, PHYTIUMPI_MCI1_BASE + MCI_CNTRL,
                 MCI_CNTRL_CONTROLLER_RESET | MCI_CNTRL_FIFO_RESET |
                 MCI_CNTRL_DMA_RESET);
    ctrl = qtest_readl(qts, PHYTIUMPI_MCI1_BASE + MCI_CNTRL);
    g_assert_cmphex(ctrl & (MCI_CNTRL_CONTROLLER_RESET |
                            MCI_CNTRL_FIFO_RESET |
                            MCI_CNTRL_DMA_RESET), ==, 0);

    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_CNTRL,
                 MCI_CNTRL_INT_ENABLE);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_INT_MASK, MCI_INT_CMD);
    phytiumpi_mci_cmd(qts, 0, 0);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_RAW_INTS) &
                    MCI_INT_CMD, ==, MCI_INT_CMD);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_MASKED_INTS) &
                    MCI_INT_CMD, ==, MCI_INT_CMD);

    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_RAW_INTS, MCI_INT_CMD);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_RAW_INTS) &
                    MCI_INT_CMD, ==, 0);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_MASKED_INTS) &
                    MCI_INT_CMD, ==, 0);

    qtest_quit(qts);
    phytiumpi_remove_sd_image(sd_path);
}

static void test_phytiumpi_mci_send_scr(void)
{
    char *sd_path = phytiumpi_create_sd_image();
    g_autofree char *args = g_strdup_printf(
        "-drive if=sd,file=%s,format=raw,auto-read-only=off", sd_path);
    QTestState *qts = phytiumpi_qtest_start_extra(1, args);
    uint32_t rca;
    uint32_t raw;

    rca = phytiumpi_mci_init_card(qts);

    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_BLKSIZ, 512);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_BYTCNT, 512);
    phytiumpi_mci_cmd(qts, rca, MCI_CMD_RESP_EXP | 55);
    phytiumpi_mci_cmd(qts, 0, MCI_CMD_RESP_EXP | MCI_CMD_DAT_EXP | 51);
    qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_DATA);
    qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_DATA);

    raw = qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_RAW_INTS);
    g_assert_cmphex(raw & MCI_INT_DTO, ==, MCI_INT_DTO);
    g_assert_cmphex(raw & (MCI_INT_RTO | MCI_INT_DRTO), ==, 0);

    phytiumpi_mci_clear_irqs(qts);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_BLKSIZ, 512);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_BYTCNT, 512);
    phytiumpi_mci_cmd(qts, 0, MCI_CMD_RESP_EXP | MCI_CMD_DAT_EXP | 51);
    qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_DATA);
    qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_DATA);

    raw = qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_RAW_INTS);
    g_assert_cmphex(raw & MCI_INT_DTO, ==, MCI_INT_DTO);
    g_assert_cmphex(raw & (MCI_INT_RTO | MCI_INT_DRTO), ==, 0);

    qtest_quit(qts);
    phytiumpi_remove_sd_image(sd_path);
}

static void test_phytiumpi_mci_adma_read(void)
{
    char *sd_path = phytiumpi_create_sd_image();
    g_autofree char *args = g_strdup_printf(
        "-drive if=sd,file=%s,format=raw,auto-read-only=off", sd_path);
    QTestState *qts = phytiumpi_qtest_start_extra(1, args);
    uint64_t desc_addr = PHYTIUMPI_RAM_BASE + 0x10000;
    uint64_t buf_addr = PHYTIUMPI_RAM_BASE + 0x11000;
    uint8_t desc[32] = {};
    uint8_t data[512] = {};

    phytiumpi_mci_init_card(qts);

    phytiumpi_mci_write_adma_desc(qts, desc_addr, buf_addr, sizeof(data));
    qtest_memset(qts, buf_addr, 0, sizeof(data));

    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_CNTRL,
                 MCI_CNTRL_INT_ENABLE | MCI_CNTRL_USE_INTERNAL_DMAC);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_INT_MASK,
                 MCI_INT_CMD | MCI_INT_DTO);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_DMAC_INT_ENA,
                 MCI_DMAC_STATUS_RI | MCI_DMAC_STATUS_NIS);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_BUS_MODE, MCI_BUS_MODE_DE);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_BLKSIZ, sizeof(data));
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_BYTCNT, sizeof(data));
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_DESC_LIST_ADDRL,
                 (uint32_t)desc_addr);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_DESC_LIST_ADDRH,
                 (uint32_t)(desc_addr >> 32));

    phytiumpi_mci_cmd(qts, 0, MCI_CMD_RESP_EXP | MCI_CMD_DAT_EXP | 17);

    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_RAW_INTS) &
                    MCI_INT_DTO, ==, MCI_INT_DTO);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_DMAC_STATUS) &
                    (MCI_DMAC_STATUS_RI | MCI_DMAC_STATUS_NIS), ==,
                    MCI_DMAC_STATUS_RI | MCI_DMAC_STATUS_NIS);

    qtest_memread(qts, buf_addr, data, sizeof(data));
    for (unsigned int i = 0; i < 512; i++) {
        g_assert_cmphex(data[i], ==, (i & 0xff));
    }

    qtest_memread(qts, desc_addr, desc, sizeof(desc));
    g_assert_cmphex(ldl_le_p(desc) & ADMA_ATTR_OWN, ==, 0);

    qtest_quit(qts);
    phytiumpi_remove_sd_image(sd_path);
}

static void test_phytiumpi_mci_adma_scr(void)
{
    char *sd_path = phytiumpi_create_sd_image();
    g_autofree char *args = g_strdup_printf(
        "-drive if=sd,file=%s,format=raw,auto-read-only=off", sd_path);
    QTestState *qts = phytiumpi_qtest_start_extra(1, args);
    uint64_t desc_addr = PHYTIUMPI_RAM_BASE + 0x12000;
    uint64_t buf_addr = PHYTIUMPI_RAM_BASE + 0x13000;
    uint8_t data[8] = {};
    uint8_t desc[32] = {};
    uint32_t rca;
    uint32_t raw;

    rca = phytiumpi_mci_init_card(qts);

    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_CNTRL,
                 MCI_CNTRL_INT_ENABLE | MCI_CNTRL_USE_INTERNAL_DMAC);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_INT_MASK,
                 MCI_INT_CMD | MCI_INT_DTO);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_DMAC_INT_ENA,
                 MCI_DMAC_STATUS_RI | MCI_DMAC_STATUS_NIS);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_BUS_MODE, MCI_BUS_MODE_DE);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_BLKSIZ, 512);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_BYTCNT, 512);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_DESC_LIST_ADDRL,
                 (uint32_t)desc_addr);
    qtest_writel(qts, PHYTIUMPI_MCI_BASE + MCI_DESC_LIST_ADDRH,
                 (uint32_t)(desc_addr >> 32));

    phytiumpi_mci_write_adma_desc(qts, desc_addr, buf_addr, 512);
    qtest_memset(qts, buf_addr, 0, 512);
    phytiumpi_mci_cmd(qts, rca, MCI_CMD_RESP_EXP | 55);
    phytiumpi_mci_cmd(qts, 0, MCI_CMD_RESP_EXP | MCI_CMD_DAT_EXP | 51);

    raw = qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_RAW_INTS);
    g_assert_cmphex(raw & MCI_INT_DTO, ==, MCI_INT_DTO);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_DMAC_STATUS) &
                    (MCI_DMAC_STATUS_RI | MCI_DMAC_STATUS_NIS), ==,
                    MCI_DMAC_STATUS_RI | MCI_DMAC_STATUS_NIS);

    qtest_memread(qts, buf_addr, data, sizeof(data));
    g_assert_cmphex(data[0], ==, 0x02);
    g_assert_cmphex(data[1], ==, 0x25);
    qtest_memread(qts, desc_addr, desc, sizeof(desc));
    g_assert_cmphex(ldl_le_p(desc) & ADMA_ATTR_OWN, ==, 0);

    phytiumpi_mci_clear_irqs(qts);
    phytiumpi_mci_write_adma_desc(qts, desc_addr, buf_addr, 512);
    qtest_memset(qts, buf_addr, 0, 512);
    phytiumpi_mci_cmd(qts, 0, MCI_CMD_RESP_EXP | MCI_CMD_DAT_EXP | 51);

    raw = qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_RAW_INTS);
    g_assert_cmphex(raw & MCI_INT_DTO, ==, MCI_INT_DTO);
    g_assert_cmphex(raw & (MCI_INT_RTO | MCI_INT_DRTO), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_MCI_BASE + MCI_DMAC_STATUS) &
                    (MCI_DMAC_STATUS_RI | MCI_DMAC_STATUS_NIS), ==,
                    MCI_DMAC_STATUS_RI | MCI_DMAC_STATUS_NIS);

    qtest_memread(qts, buf_addr, data, sizeof(data));
    g_assert_cmphex(data[0], ==, 0x02);
    g_assert_cmphex(data[1], ==, 0x25);

    qtest_quit(qts);
    phytiumpi_remove_sd_image(sd_path);
}

static void test_phytiumpi_xmac_phy_link(void)
{
    QTestState *qts = phytiumpi_qtest_start(1);
    uint16_t bmsr;
    uint16_t phyid1;
    uint16_t phyid2;

    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_XMAC_BASE +
                                XMAC_NETWORK_STATUS) & XMAC_MDIO_IDLE,
                    ==, XMAC_MDIO_IDLE);
    g_assert_cmphex(qtest_readl(qts, PHYTIUMPI_XMAC_BASE + XMAC_PCS_STATUS) &
                    XMAC_PCS_LINK_UP, ==, XMAC_PCS_LINK_UP);

    bmsr = phytiumpi_xmac_phy_read(qts, 0, PHY_BMSR);
    g_assert_cmphex(bmsr & PHY_BMSR_LINK_STATUS, ==,
                    PHY_BMSR_LINK_STATUS);
    g_assert_cmphex(bmsr & PHY_BMSR_AUTONEG_COMPLETE, ==,
                    PHY_BMSR_AUTONEG_COMPLETE);

    phyid1 = phytiumpi_xmac_phy_read(qts, 0, PHY_PHYID1);
    phyid2 = phytiumpi_xmac_phy_read(qts, 0, PHY_PHYID2);
    g_assert_cmphex(phyid1, !=, 0);
    g_assert_cmphex(phyid1, !=, 0xffff);
    g_assert_cmphex(phyid2, !=, 0);
    g_assert_cmphex(phyid2, !=, 0xffff);

    phytiumpi_xmac_phy_write(qts, 0, PHY_BMCR, PHY_BMCR_RESET);
    g_assert_cmphex(phytiumpi_xmac_phy_read(qts, 0, PHY_BMCR) &
                    PHY_BMCR_RESET, ==, 0);
    g_assert_cmphex(phytiumpi_xmac_phy_read(qts, 1, PHY_BMSR), ==, 0xffff);

    qtest_quit(qts);
}

static void test_phytiumpi_smp_creation(void)
{
    QTestState *qts = phytiumpi_qtest_start(4);

    qtest_readq(qts, PHYTIUMPI_GICR_BASE + GICR_TYPER);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/phytium-pi/machine_creation",
                   test_phytiumpi_machine_creation);
    qtest_add_func("/phytium-pi/highmem_creation",
                   test_phytiumpi_highmem_creation);
    qtest_add_func("/phytium-pi/mci_registers", test_phytiumpi_mci_registers);
    qtest_add_func("/phytium-pi/mci_send_scr", test_phytiumpi_mci_send_scr);
    qtest_add_func("/phytium-pi/mci_adma_read", test_phytiumpi_mci_adma_read);
    qtest_add_func("/phytium-pi/mci_adma_scr", test_phytiumpi_mci_adma_scr);
    qtest_add_func("/phytium-pi/xmac_phy_link", test_phytiumpi_xmac_phy_link);
    qtest_add_func("/phytium-pi/smp_creation", test_phytiumpi_smp_creation);

    return g_test_run();
}
