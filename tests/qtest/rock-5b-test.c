/*
 * QEMU ROCK 5B machine tests
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/units.h"
#include "qemu/sockets.h"
#include "libqtest.h"

#define ROCK5B_LOW_RAM_BASE     0x00000000ULL
#define ROCK5B_FIRMWARE_BASE    0x00200000ULL
#define ROCK5B_GIC_DIST_BASE    0xfe600000ULL
#define ROCK5B_GIC_ITS0_BASE    0xfe640000ULL
#define ROCK5B_SFC_BASE         0xfe2b0000ULL
#define ROCK5B_SDMMC_BASE       0xfe2c0000ULL
#define ROCK5B_UART2_BASE       0xfeb50000ULL
#define ROCK5B_UART3_BASE       0xfeb60000ULL
#define ROCK5B_USB3OTG0_BASE    0xfc000000ULL
#define ROCK5B_PCIE2X1L2_APB    0xfe190000ULL
#define ROCK5B_PCIE2X1L2_DBI    0xa41000000ULL
#define ROCK5B_PCIE2X1L2_CFG    0xf4000000ULL
#define ROCK5B_CRU_BASE         0xfd7c0000ULL

/* RK3588 pcie2x1l2 APB vendor registers (PCIE_CLIENT_*). */
#define RK_PCIE_CLIENT_GENERAL_CON      0x000
#define RK_PCIE_CLIENT_LTSSM_STATUS     0x300
#define RK_PCIE_LTSSM_LINK_UP           0x00030011U

/* DesignWare iATU viewport registers. */
#define DW_PCIE_ATU_VIEWPORT            0x900
#define DW_PCIE_ATU_CR1                 0x904
#define DW_PCIE_ATU_CR2                 0x908
#define DW_PCIE_ATU_LOWER_BASE          0x90c
#define DW_PCIE_ATU_UPPER_BASE          0x910
#define DW_PCIE_ATU_LIMIT               0x914
#define DW_PCIE_ATU_LOWER_TARGET        0x918
#define DW_PCIE_ATU_UPPER_TARGET        0x91c
#define DW_PCIE_ATU_TYPE_CFG0           0x4
#define DW_PCIE_ATU_ENABLE              0x80000000U
#define DW_PCIE_DBI_TAIL_OFFSET         0x1000

#define RTL8125_VENDOR_ID               0x10ec
#define RTL8125_DEVICE_ID               0x8125

/* DWC3 global/device registers. */
#define DWC3_GCOREID            0x0000c120
#define DWC3_GCTL               0x0000c110
#define DWC3_DCFG               0x0000c700

#define DWC3_GCOREID_VALUE      0x5533330a
#define DWC3_GCTL_RESET         0x30c13004

/* DW8250 UART status register, 32-bit register window. */
#define DW_UART_THR             0x00
#define DW_UART_RBR             0x00
#define DW_UART_IER             0x04
#define DW_UART_IIR             0x08
#define DW_UART_FCR             0x08
#define DW_UART_SCR             0x1c
#define DW_UART_LSR             0x14
#define DW_UART_USR             0x7c

#define DW_UART_IER_RDI         BIT(0)
#define DW_UART_IIR_NO_INT      BIT(0)
#define DW_UART_IIR_RDI         0x04
#define DW_UART_IIR_FIFO_MASK   0xc0
#define DW_UART_FCR_FE          BIT(0)
#define DW_UART_FCR_ITL_4       0x40
#define DW_UART_LSR_DR          BIT(0)
#define DW_UART_LSR_THRE        BIT(5)
#define DW_UART_LSR_TEMT        BIT(6)
#define DW_UART_USR_RFNE        BIT(3)
#define ROCK5B_UART2_IRQ        333

/* dw_mmc core register offsets (DesignWare Mobile Storage Host). */
#define DW_MMC_CTRL             0x000
#define DW_MMC_PWREN            0x004
#define DW_MMC_BLKSIZ           0x01c
#define DW_MMC_BYTCNT           0x020
#define DW_MMC_INTMASK          0x024
#define DW_MMC_CMDARG           0x028
#define DW_MMC_CMD              0x02c
#define DW_MMC_RESP0            0x030
#define DW_MMC_MINTSTS          0x040
#define DW_MMC_RINTSTS          0x044
#define DW_MMC_STATUS           0x048
#define DW_MMC_FIFOTH           0x04c
#define DW_MMC_CDETECT          0x050
#define DW_MMC_VERID            0x06c
#define DW_MMC_HCON             0x070
#define DW_MMC_RST_N            0x078
#define DW_MMC_BMOD             0x080
#define DW_MMC_DBADDR           0x088
#define DW_MMC_IDSTS            0x08c
#define DW_MMC_IDINTEN          0x090
#define DW_MMC_CDTHRCTL         0x100
/* VERID >= 0x240a: 0x100 is the PIO FIFO data port during a transfer. */
#define DW_MMC_FIFO             0x100
#define DW_MMC_ENABLE_SHIFT     0x110
#define DW_MMC_RK_TIMING_CON0   0x130
#define DW_MMC_LEGACY_DATA      0x200

#define DW_MMC_CTRL_RESET       BIT(0)
#define DW_MMC_CTRL_FIFO_RESET  BIT(1)
#define DW_MMC_CTRL_DMA_RESET   BIT(2)
#define DW_MMC_CTRL_INT_ENABLE  BIT(4)
#define DW_MMC_CTRL_DMA_ENABLE  BIT(5)
#define DW_MMC_CTRL_USE_IDMAC   BIT(25)
#define DW_MMC_DESC_OWN         BIT(31)
#define DW_MMC_DESC_CH          BIT(4)
#define DW_MMC_DESC_LD          BIT(2)
#define DW_MMC_CMD_RESP_EXP     BIT(6)
#define DW_MMC_CMD_RESP_LONG    BIT(7)
#define DW_MMC_CMD_DAT_EXP      BIT(9)
#define DW_MMC_CMD_START        BIT(31)

#define DW_MMC_INT_CMD_DONE     BIT(2)
#define DW_MMC_INT_DATA_OVER    BIT(3)
#define DW_MMC_INT_RTO          BIT(8)

#define DW_MMC_STATUS_FIFO_EMPTY BIT(2)
#define DW_MMC_BMOD_SWRESET     BIT(0)
#define DW_MMC_BMOD_ENABLE      BIT(7)

/* IP values probed by the drivers (see include/hw/sd/dw_mmc.h). */
#define DW_MMC_VERID_270A       0x0000270a
#define DW_MMC_FIFOTH_POR       0x00800000

#define DW_MMC_BLOCK_SIZE       512
#define DW_MMC_IMAGE_SIZE       (8 * MiB)
#define DW_MMC_IMAGE_BLOCKS     (DW_MMC_IMAGE_SIZE / DW_MMC_BLOCK_SIZE)
#define ROCK5B_SDMMC_SPI        203

/* GIC SPI N is INTID 32+N on the distributor; GICD_ISPENDR shows the level. */
#define ROCK5B_GIC_SPI_INTID(spi)   ((spi) + 32)
#define ROCK5B_GIC_ISPENDR(intid) \
    (ROCK5B_GIC_DIST_BASE + 0x200 + 4 * ((intid) / 32))

static uint32_t mmio_read32(QTestState *qts, uint64_t addr)
{
    return qtest_readl(qts, addr);
}

static void rock5b_uart_write(QTestState *qts, uint64_t base,
                              uint32_t reg, uint32_t value)
{
    qtest_writel(qts, base + reg, value);
}

static uint32_t rock5b_uart_read(QTestState *qts, uint64_t base,
                                 uint32_t reg)
{
    return qtest_readl(qts, base + reg);
}

static char *rock5b_uart_socket_path(void)
{
    g_autofree char *dir = g_dir_make_tmp("rock5b-uart-XXXXXX", NULL);

    g_assert_nonnull(dir);
    return g_build_filename(dir, "serial.sock", NULL);
}

static int rock5b_uart_connect(const char *socket_path)
{
    Error *err = NULL;
    int fd = unix_connect(socket_path, &err);

    g_assert_null(err);
    g_assert_cmpint(fd, >=, 0);
#ifndef _WIN32
    {
        struct timeval timeout = { .tv_sec = 2 };

        g_assert_cmpint(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                                   &timeout, sizeof(timeout)), ==, 0);
    }
#endif
    return fd;
}

static char rock5b_uart_recv_byte(int fd)
{
    char data;
    ssize_t ret;

    do {
        ret = recv(fd, &data, sizeof(data), 0);
    } while (ret < 0 && errno == EINTR);
    g_assert_cmpint(ret, ==, sizeof(data));
    return data;
}

static void rock5b_uart_send(int fd, const char *data, size_t size)
{
    g_assert_cmpint(qemu_write_full(fd, data, size), ==, size);
}

static void rock5b_uart_wait_lsr(QTestState *qts, uint64_t base,
                                 uint32_t mask, uint32_t value)
{
    uint32_t status = 0;

    for (unsigned int i = 0; i < 1000; i++) {
        status = rock5b_uart_read(qts, base, DW_UART_LSR);
        if ((status & mask) == value) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_cmphex(status & mask, ==, value);
}

static void rock5b_uart_wait_irq(QTestState *qts, int irq, bool level)
{
    for (unsigned int i = 0; i < 1000; i++) {
        if (qtest_get_irq(qts, irq) == level) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_cmpint(qtest_get_irq(qts, irq), ==, level);
}

static void rock5b_uart_qtest_quit(QTestState *qts, int fd,
                                  const char *socket_path)
{
    g_autofree char *socket_dir = g_path_get_dirname(socket_path);

    close(fd);
    qtest_quit(qts);
    g_unlink(socket_path);
    g_rmdir(socket_dir);
}

static QTestState *rock5b_uart2_qtest_init(int *serial_fd,
                                           char **socket_path)
{
    QTestState *qts;

    *socket_path = rock5b_uart_socket_path();
    /* Slot 0 is Type-C CDC; slot 1 is UART2 on the board header. */
    qts = qtest_initf("-machine rock-5b -m 256M "
                      "-chardev socket,id=uart2,path=%s,server=on,wait=off "
                      "-serial null -serial chardev:uart2 -display none",
                      *socket_path);
    *serial_fd = rock5b_uart_connect(*socket_path);
    return qts;
}

static void test_uart2_chardev(void)
{
    int serial_fd;
    g_autofree char *socket_path = NULL;
    QTestState *qts = rock5b_uart2_qtest_init(&serial_fd, &socket_path);
    const char rx_data[] = "R";

    /* A guest write to THR must arrive at the UART2 character backend. */
    rock5b_uart_write(qts, ROCK5B_UART2_BASE, DW_UART_THR, 'T');
    g_assert_cmpint(rock5b_uart_recv_byte(serial_fd), ==, 'T');
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_LSR) &
                    (DW_UART_LSR_THRE | DW_UART_LSR_TEMT), ==,
                    DW_UART_LSR_THRE | DW_UART_LSR_TEMT);

    /* Host input must become readable from RBR and clear the RX-ready bit. */
    rock5b_uart_send(serial_fd, rx_data, sizeof(rx_data) - 1);
    rock5b_uart_wait_lsr(qts, ROCK5B_UART2_BASE, DW_UART_LSR_DR,
                         DW_UART_LSR_DR);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_LSR) &
                    DW_UART_LSR_DR, ==, DW_UART_LSR_DR);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_USR) &
                    DW_UART_USR_RFNE, ==, DW_UART_USR_RFNE);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_RBR),
                    ==, 'R');
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_LSR) &
                    DW_UART_LSR_DR, ==, 0);

    rock5b_uart_qtest_quit(qts, serial_fd, socket_path);
}

static void test_uart2_rx_fifo_irq(void)
{
    int serial_fd;
    g_autofree char *socket_path = NULL;
    QTestState *qts = rock5b_uart2_qtest_init(&serial_fd, &socket_path);

    qtest_irq_intercept_in(qts, "/machine/soc/gic");
    rock5b_uart_write(qts, ROCK5B_UART2_BASE, DW_UART_FCR,
                      DW_UART_FCR_FE | DW_UART_FCR_ITL_4);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_IIR) &
                    DW_UART_IIR_FIFO_MASK, ==, DW_UART_IIR_FIFO_MASK);
    rock5b_uart_write(qts, ROCK5B_UART2_BASE, DW_UART_IER, DW_UART_IER_RDI);

    /* Four bytes reach the FIFO trigger level and raise the UART2 IRQ. */
    rock5b_uart_send(serial_fd, "AB", 2);
    rock5b_uart_wait_lsr(qts, ROCK5B_UART2_BASE, DW_UART_LSR_DR,
                         DW_UART_LSR_DR);
    g_assert_false(qtest_get_irq(qts, ROCK5B_UART2_IRQ));
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_IIR) &
                    DW_UART_IIR_FIFO_MASK, ==, DW_UART_IIR_FIFO_MASK);
    rock5b_uart_send(serial_fd, "CD", 2);
    rock5b_uart_wait_irq(qts, ROCK5B_UART2_IRQ, true);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_LSR) &
                    DW_UART_LSR_DR, ==, DW_UART_LSR_DR);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_IIR) &
                    0x0f, ==, DW_UART_IIR_RDI);
    rock5b_uart_wait_irq(qts, ROCK5B_UART2_IRQ, true);

    /* Masking drops the IRQ without discarding the pending FIFO contents. */
    rock5b_uart_write(qts, ROCK5B_UART2_BASE, DW_UART_IER, 0);
    g_assert_false(qtest_get_irq(qts, ROCK5B_UART2_IRQ));

    /* Re-enabling RDI reasserts the IRQ while the trigger condition remains. */
    rock5b_uart_write(qts, ROCK5B_UART2_BASE, DW_UART_IER, DW_UART_IER_RDI);
    g_assert_true(qtest_get_irq(qts, ROCK5B_UART2_IRQ));
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_RBR),
                    ==, 'A');
    g_assert_false(qtest_get_irq(qts, ROCK5B_UART2_IRQ));
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_RBR),
                    ==, 'B');
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_RBR),
                    ==, 'C');
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_RBR),
                    ==, 'D');
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_LSR) &
                    DW_UART_LSR_DR, ==, 0);

    /* A reset must clear a fresh pending receive interrupt and its data. */
    rock5b_uart_write(qts, ROCK5B_UART2_BASE, DW_UART_FCR, 0);
    rock5b_uart_send(serial_fd, "X", 1);
    rock5b_uart_wait_irq(qts, ROCK5B_UART2_IRQ, true);
    qtest_system_reset(qts);
    g_assert_false(qtest_get_irq(qts, ROCK5B_UART2_IRQ));
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_IER),
                    ==, 0);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_IIR) &
                    0x0f, ==, DW_UART_IIR_NO_INT);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_LSR) &
                    DW_UART_LSR_DR, ==, 0);

    rock5b_uart_qtest_quit(qts, serial_fd, socket_path);
}

static void test_uart3_isolation(void)
{
    g_autofree char *uart2_path = rock5b_uart_socket_path();
    g_autofree char *uart3_path = rock5b_uart_socket_path();
    int uart2_fd, uart3_fd;
    QTestState *qts = qtest_initf("-machine rock-5b -m 256M "
        "-chardev socket,id=uart2,path=%s,server=on,wait=off "
        "-chardev socket,id=uart3,path=%s,server=on,wait=off "
        "-serial null -serial chardev:uart2 -serial null "
        "-serial chardev:uart3 -display none", uart2_path, uart3_path);

    uart2_fd = rock5b_uart_connect(uart2_path);
    uart3_fd = rock5b_uart_connect(uart3_path);

    /* UART2 is slot 1 and UART3 is slot 3; each has a distinct backend. */
    rock5b_uart_write(qts, ROCK5B_UART2_BASE, DW_UART_SCR, 0x52);
    rock5b_uart_write(qts, ROCK5B_UART3_BASE, DW_UART_SCR, 0x53);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_SCR),
                    ==, 0x52);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART3_BASE, DW_UART_SCR),
                    ==, 0x53);

    rock5b_uart_write(qts, ROCK5B_UART2_BASE, DW_UART_THR, '2');
    rock5b_uart_write(qts, ROCK5B_UART3_BASE, DW_UART_THR, '3');
    g_assert_cmpint(rock5b_uart_recv_byte(uart2_fd), ==, '2');
    g_assert_cmpint(rock5b_uart_recv_byte(uart3_fd), ==, '3');

    rock5b_uart_send(uart2_fd, "a", 1);
    rock5b_uart_send(uart3_fd, "b", 1);
    rock5b_uart_wait_lsr(qts, ROCK5B_UART2_BASE, DW_UART_LSR_DR,
                         DW_UART_LSR_DR);
    rock5b_uart_wait_lsr(qts, ROCK5B_UART3_BASE, DW_UART_LSR_DR,
                         DW_UART_LSR_DR);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_RBR),
                    ==, 'a');
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART3_BASE, DW_UART_RBR),
                    ==, 'b');
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART2_BASE, DW_UART_LSR) &
                    DW_UART_LSR_DR, ==, 0);
    g_assert_cmphex(rock5b_uart_read(qts, ROCK5B_UART3_BASE, DW_UART_LSR) &
                    DW_UART_LSR_DR, ==, 0);

    rock5b_uart_qtest_quit(qts, uart2_fd, uart2_path);
    close(uart3_fd);
    g_unlink(uart3_path);
    g_autofree char *uart3_dir = g_path_get_dirname(uart3_path);
    g_rmdir(uart3_dir);
}

static void test_machine_topology(void)
{
    QTestState *qts = qtest_init("-machine rock-5b -m 256M -smp 8 "
                                 "-serial null -display none");

    /* Low RAM is populated and writable. */
    qtest_writel(qts, ROCK5B_LOW_RAM_BASE + 0x1000, 0x5a5aa5a5);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_LOW_RAM_BASE + 0x1000),
                    ==, 0x5a5aa5a5);
    qtest_writel(qts, ROCK5B_FIRMWARE_BASE, 0xdeadbeef);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_FIRMWARE_BASE), ==, 0xdeadbeef);

    /* GICv3 distributor and the two ITS windows are mapped. */
    g_assert_cmphex(mmio_read32(qts, ROCK5B_GIC_DIST_BASE + 0x8), !=, 0);
    (void)mmio_read32(qts, ROCK5B_GIC_ITS0_BASE);
    (void)mmio_read32(qts, ROCK5B_GIC_ITS0_BASE + 0x1000);

    /* SFC and the DW8250 UART respond. */
    (void)mmio_read32(qts, ROCK5B_SFC_BASE);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_UART2_BASE + DW_UART_USR),
                    ==, 0x6);

    /* The Type-C DWC3 UDC is modeled at its RK3588 window. */
    g_assert_cmphex(mmio_read32(qts, ROCK5B_USB3OTG0_BASE + DWC3_GCOREID),
                    ==, DWC3_GCOREID_VALUE);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_USB3OTG0_BASE + DWC3_GCTL),
                    ==, DWC3_GCTL_RESET);

    qtest_writel(qts, ROCK5B_USB3OTG0_BASE + DWC3_DCFG, 0x00000004);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_USB3OTG0_BASE + DWC3_DCFG),
                    ==, 0x00000004);

    qtest_quit(qts);
}

/*
 * The RK3588 pcie2x1l2 link must be modeled well enough for an unmodified
 * Zephyr pcie_dw_rockchip driver to bring the link up and enumerate the
 * onboard RTL8125 endpoint through the DesignWare iATU:
 *
 *  - the Rockchip APB window reports the LTSSM link-up bits the driver polls;
 *  - the 4 MiB DBI window is fully mapped (the driver writes DBI2 at +0x100000
 *    during root setup, so an unmapped tail would abort the guest);
 *  - programming the outbound CFG0 iATU viewport exposes the endpoint config
 *    space at the DT config window (0xf4000000), where vendor/device
 *    10ec:8125 must be visible.
 */
static void test_pcie_rtl8125(void)
{
    QTestState *qts = qtest_init("-machine rock-5b -m 256M "
                                 "-serial null -display none");

    /* Rockchip APB vendor window: LTSSM reports L0/link-up. */
    g_assert_cmphex(qtest_readl(qts, ROCK5B_PCIE2X1L2_APB +
                               RK_PCIE_CLIENT_LTSSM_STATUS),
                    ==, RK_PCIE_LTSSM_LINK_UP);
    g_assert_cmphex(qtest_readl(qts, ROCK5B_PCIE2X1L2_APB +
                               RK_PCIE_CLIENT_GENERAL_CON), ==, 0);

    /* DBI core header is the DesignWare root port (PCI-to-PCI bridge). */
    g_assert_cmphex(qtest_readl(qts, ROCK5B_PCIE2X1L2_DBI) & 0xffff, !=,
                    0xffff);
    g_assert_cmphex((qtest_readl(qts, ROCK5B_PCIE2X1L2_DBI + 0x0c) >> 16) &
                    0xff, ==, 0x01);

    /* DBI2 (inside the 4 MiB window) is mapped and RAZ/WI, not unassigned. */
    g_assert_cmphex(qtest_readl(qts, ROCK5B_PCIE2X1L2_DBI +
                               DW_PCIE_DBI_TAIL_OFFSET + 0x10), ==, 0);

    /* Root port bus numbers: primary 0, secondary 1, subordinate 0xff. */
    qtest_writel(qts, ROCK5B_PCIE2X1L2_DBI + 0x18, 0x00ff0100);

    /* Program outbound iATU viewport 0 as CFG0 for bus 1, dev 0, fn 0. */
    qtest_writel(qts, ROCK5B_PCIE2X1L2_DBI + DW_PCIE_ATU_VIEWPORT, 0);
    qtest_writel(qts, ROCK5B_PCIE2X1L2_DBI + DW_PCIE_ATU_LOWER_BASE,
                 ROCK5B_PCIE2X1L2_CFG);
    qtest_writel(qts, ROCK5B_PCIE2X1L2_DBI + DW_PCIE_ATU_UPPER_BASE, 0);
    qtest_writel(qts, ROCK5B_PCIE2X1L2_DBI + DW_PCIE_ATU_LIMIT,
                 ROCK5B_PCIE2X1L2_CFG + 0x100000 - 1);
    qtest_writel(qts, ROCK5B_PCIE2X1L2_DBI + DW_PCIE_ATU_LOWER_TARGET,
                 0x01000000);
    qtest_writel(qts, ROCK5B_PCIE2X1L2_DBI + DW_PCIE_ATU_UPPER_TARGET, 0);
    qtest_writel(qts, ROCK5B_PCIE2X1L2_DBI + DW_PCIE_ATU_CR1,
                 DW_PCIE_ATU_TYPE_CFG0);
    qtest_writel(qts, ROCK5B_PCIE2X1L2_DBI + DW_PCIE_ATU_CR2,
                 DW_PCIE_ATU_ENABLE);

    /* The RTL8125 endpoint is visible at bus 1, dev 0, fn 0. */
    g_assert_cmphex(qtest_readl(qts, ROCK5B_PCIE2X1L2_CFG) & 0xffff,
                    ==, RTL8125_VENDOR_ID);
    g_assert_cmphex(qtest_readl(qts, ROCK5B_PCIE2X1L2_CFG) >> 16,
                    ==, RTL8125_DEVICE_ID);
    /* Ethernet class, and a writable command register for MEM|MASTER. */
    g_assert_cmphex(qtest_readl(qts, ROCK5B_PCIE2X1L2_CFG + 0x08) &
                    0xffffff00, ==, 0x02000000);
    qtest_writel(qts, ROCK5B_PCIE2X1L2_CFG + 0x04, 0x6);
    g_assert_cmphex(qtest_readl(qts, ROCK5B_PCIE2X1L2_CFG + 0x04) & 0x7,
                    ==, 0x6);

    qtest_quit(qts);
}

/* SD card image offsets 0..511 carry a known test pattern. */
static void rock5b_sdmmc_pattern(uint8_t *buf)
{
    for (unsigned int i = 0; i < DW_MMC_BLOCK_SIZE; i++) {
        buf[i] = (i * 7 + 0x5a) & 0xff;
    }
}

static char *rock5b_sdmmc_create_image(void)
{
    uint8_t pattern[DW_MMC_BLOCK_SIZE];
    g_autoptr(GError) error = NULL;
    char *path = NULL;
    int fd;

    fd = g_file_open_tmp("rock5b-sdmmc-XXXXXX", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(ftruncate(fd, DW_MMC_IMAGE_SIZE), ==, 0);

    /*
     * Block 0 keeps the classic rock5b_sdmmc_pattern() used by the PIO
     * test; later blocks carry a block-index-dependent pattern so a large
     * IDMAC transfer can prove that every descriptor was serviced.
     */
    for (unsigned int block = 0; block < DW_MMC_IMAGE_BLOCKS; block++) {
        for (unsigned int i = 0; i < DW_MMC_BLOCK_SIZE; i++) {
            pattern[i] = (block * 13 + i * 7 + 0x5a) & 0xff;
        }
        g_assert_cmpint(lseek(fd, (off_t)block * DW_MMC_BLOCK_SIZE,
                              SEEK_SET), ==,
                        (off_t)block * DW_MMC_BLOCK_SIZE);
        g_assert_cmpint(qemu_write_full(fd, pattern, sizeof(pattern)), ==,
                        sizeof(pattern));
    }
    close(fd);

    return path;
}

static uint32_t rock5b_sdmmc_command(QTestState *qts, uint32_t index,
                                     uint32_t argument, uint32_t flags)
{
    uint32_t raw;

    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_RINTSTS, UINT32_MAX);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_CMDARG, argument);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_CMD,
                 DW_MMC_CMD_START | flags | index);
    raw = qtest_readl(qts, ROCK5B_SDMMC_BASE + DW_MMC_RINTSTS);
    g_assert_cmphex(raw & DW_MMC_INT_CMD_DONE, ==, DW_MMC_INT_CMD_DONE);
    g_assert_cmphex(raw & DW_MMC_INT_RTO, ==, 0);

    return qtest_readl(qts, ROCK5B_SDMMC_BASE + DW_MMC_RESP0);
}

/* Bring the attached SD card from idle to transfer state (SD spec path). */
static void rock5b_sdmmc_init_card(QTestState *qts)
{
    uint32_t rca;

    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_PWREN, 1);
    rock5b_sdmmc_command(qts, 0, 0, 0);
    rock5b_sdmmc_command(qts, 55, 0, DW_MMC_CMD_RESP_EXP);
    rock5b_sdmmc_command(qts, 41, 0x41200000, DW_MMC_CMD_RESP_EXP);
    rock5b_sdmmc_command(qts, 2, 0,
                         DW_MMC_CMD_RESP_EXP | DW_MMC_CMD_RESP_LONG);
    rca = rock5b_sdmmc_command(qts, 3, 0, DW_MMC_CMD_RESP_EXP) & 0xffff0000;
    g_assert_cmphex(rca, !=, 0);
    rock5b_sdmmc_command(qts, 7, rca, DW_MMC_CMD_RESP_EXP);
    rock5b_sdmmc_command(qts, 16, DW_MMC_BLOCK_SIZE, DW_MMC_CMD_RESP_EXP);
}

/*
 * sdmmc@fe2c0000 is the RK3588 SD-card controller (Synopsys dw_mmc).  The
 * test pins the reset-visible register contract that the firmware drivers
 * probe, then checks that no drive means "no card", and that an SD image on
 * the legacy -drive if=sd,index=2 interface is attached to the controller's
 * SD bus: card detect, command/response, GIC SPI 203 delivery and a PIO
 * block read from the card.
 */
static void test_sdmmc(void)
{
    g_autofree char *sd_path = rock5b_sdmmc_create_image();
    uint8_t expected[DW_MMC_BLOCK_SIZE];
    uint8_t actual[DW_MMC_BLOCK_SIZE];
    QTestState *qts;
    uint32_t ctrl;
    uint32_t raw;

    /* No drive: the slot is empty. */
    qts = qtest_init("-machine rock-5b -m 256M -serial null -display none");
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_VERID),
                    ==, DW_MMC_VERID_270A);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_HCON), ==, 0);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_RST_N), ==, 1);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_CDETECT) & 1,
                    ==, 1);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_STATUS) &
                    DW_MMC_STATUS_FIFO_EMPTY, ==, DW_MMC_STATUS_FIFO_EMPTY);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_FIFOTH),
                    ==, DW_MMC_FIFOTH_POR);

    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_CTRL,
                 DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET |
                 DW_MMC_CTRL_DMA_RESET);
    ctrl = mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_CTRL);
    g_assert_cmphex(ctrl & (DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET |
                            DW_MMC_CTRL_DMA_RESET), ==, 0);

    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_BMOD,
                 DW_MMC_BMOD_ENABLE | DW_MMC_BMOD_SWRESET);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_BMOD),
                    ==, DW_MMC_BMOD_ENABLE);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_DBADDR, 0x00300000);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_DBADDR),
                    ==, 0x00300000);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_IDINTEN, 0x00000303);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_IDINTEN),
                    ==, 0x00000303);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_IDSTS), ==, 0);

    /*
     * With no card attached a response-expecting command times out; a CMD0
     * with START only completes and the START bit self-clears.
     */
    rock5b_sdmmc_command(qts, 0, 0, 0);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_RINTSTS, UINT32_MAX);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_CMD,
                 DW_MMC_CMD_START | DW_MMC_CMD_RESP_EXP);
    raw = mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_RINTSTS);
    g_assert_cmphex(raw & DW_MMC_INT_RTO, ==, DW_MMC_INT_RTO);
    g_assert_cmphex(raw & DW_MMC_INT_CMD_DONE, ==, 0);

    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_CTRL, DW_MMC_CTRL_INT_ENABLE);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_INTMASK, DW_MMC_INT_CMD_DONE);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_CMD, DW_MMC_CMD_START);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_CMD) &
                    DW_MMC_CMD_START, ==, 0);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_MINTSTS),
                    ==, DW_MMC_INT_CMD_DONE);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_RINTSTS,
                 DW_MMC_INT_CMD_DONE);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_MINTSTS),
                    ==, 0);

    /* The 0x100 CDTHRCTL register and the legacy 0x200 data port. */
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_CDTHRCTL, 0x00100001);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_CDTHRCTL),
                    ==, 0x00100001);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_LEGACY_DATA, 0xa5a5a5a5);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_LEGACY_DATA),
                    ==, 0);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_ENABLE_SHIFT, 1);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_ENABLE_SHIFT),
                    ==, 1);

    /* RK3588 does not use the internal phase registers; they are RAZ/WI. */
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_RK_TIMING_CON0,
                 0xffffffff);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE +
                                DW_MMC_RK_TIMING_CON0), ==, 0);

    /* Controller reset restores the reset-visible state. */
    qtest_system_reset(qts);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_VERID),
                    ==, DW_MMC_VERID_270A);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_RINTSTS),
                    ==, 0);
    qtest_quit(qts);

    /*
     * -drive if=sd,index=2 attaches an SD card to the dw_mmc SD bus, the
     * same legacy drive interface the downstream machine used for the
     * board's microSD slot (mmc dev 1 in U-Boot).
     */
    qts = qtest_initf("-machine rock-5b -m 256M -serial null -display none "
                      "-drive file=%s,format=raw,if=sd,index=2",
                      sd_path);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_CDETECT) & 1,
                    ==, 0);

    /*
     * Command completion raises the controller IRQ line that is wired to GIC
     * SPI 203 (INTID 235).  A level-triggered GIC line reads as pending while
     * it is asserted, and RINTSTS W1C deasserts it again.
     */
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_CTRL, DW_MMC_CTRL_INT_ENABLE);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_INTMASK, DW_MMC_INT_CMD_DONE);
    rock5b_sdmmc_command(qts, 0, 0, 0);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_GIC_ISPENDR(
                    ROCK5B_GIC_SPI_INTID(ROCK5B_SDMMC_SPI))) &
                    BIT(ROCK5B_GIC_SPI_INTID(ROCK5B_SDMMC_SPI) % 32), ==,
                    BIT(ROCK5B_GIC_SPI_INTID(ROCK5B_SDMMC_SPI) % 32));
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_RINTSTS, UINT32_MAX);
    g_assert_cmphex(mmio_read32(qts, ROCK5B_GIC_ISPENDR(
                    ROCK5B_GIC_SPI_INTID(ROCK5B_SDMMC_SPI))) &
                    BIT(ROCK5B_GIC_SPI_INTID(ROCK5B_SDMMC_SPI) % 32), ==, 0);

    /* CMD8 (SEND_IF_COND) must echo the supplied check pattern. */
    g_assert_cmphex(rock5b_sdmmc_command(qts, 8, 0x1aa,
                                         DW_MMC_CMD_RESP_EXP),
                    ==, 0x1aa);

    /* Initialize the card, then read block 0 through the PIO FIFO. */
    rock5b_sdmmc_init_card(qts);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_BLKSIZ, DW_MMC_BLOCK_SIZE);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_BYTCNT, DW_MMC_BLOCK_SIZE);
    rock5b_sdmmc_command(qts, 17, 0,
                         DW_MMC_CMD_RESP_EXP | DW_MMC_CMD_DAT_EXP);
    for (unsigned int i = 0; i < sizeof(actual); i += sizeof(uint32_t)) {
        uint32_t word = qtest_readl(qts, ROCK5B_SDMMC_BASE + DW_MMC_FIFO);
        stl_le_p(actual + i, word);
    }
    raw = mmio_read32(qts, ROCK5B_SDMMC_BASE + DW_MMC_RINTSTS);
    g_assert_cmphex(raw & DW_MMC_INT_DATA_OVER, ==, DW_MMC_INT_DATA_OVER);
    rock5b_sdmmc_pattern(expected);
    g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));

    qtest_quit(qts);
    unlink(sd_path);
}

#define DW_MMC_IDMAC_DESC_BASE  0x00100000ULL
#define DW_MMC_IDMAC_DEST_BASE  0x01000000ULL
#define DW_MMC_IDMAC_DESC_COUNT 6000

static void rock5b_sdmmc_block_pattern(unsigned int block, uint8_t *buf)
{
    for (unsigned int i = 0; i < DW_MMC_BLOCK_SIZE; i++) {
        buf[i] = (block * 13 + i * 7 + 0x5a) & 0xff;
    }
}

/*
 * Large IDMAC transfers: real U-Boot reads a 108 MiB initramfs through the
 * IDMAC descriptor ring.  Each descriptor here covers 512 bytes; 6000
 * descriptors deliberately exceed the historical 4096-descriptor model cap,
 * which silently truncated the transfer after 2 MiB and corrupted the SD
 * payload.  Verify the tail descriptors are serviced too.
 */
static void test_sdmmc_idmac(void)
{
    char *sd_path = rock5b_sdmmc_create_image();
    QTestState *qts = qtest_initf("-machine rock-5b -m 256M -serial null "
                                  "-display none "
                                  "-drive file=%s,format=raw,if=sd,index=2",
                                  sd_path);
    g_autofree uint8_t *descs =
        g_malloc0(DW_MMC_IDMAC_DESC_COUNT * 16);
    uint8_t expected[DW_MMC_BLOCK_SIZE];
    uint8_t actual[DW_MMC_BLOCK_SIZE];
    const unsigned int samples[] = { 0, 4094, 4095, 4096, 4097, 5999 };
    uint32_t raw;

    for (unsigned int i = 0; i < DW_MMC_IDMAC_DESC_COUNT; i++) {
        uint8_t *d = descs + i * 16;
        uint32_t des0 = DW_MMC_DESC_OWN | DW_MMC_DESC_CH;
        uint64_t next = DW_MMC_IDMAC_DESC_BASE + (i + 1) * 16;

        if (i == DW_MMC_IDMAC_DESC_COUNT - 1) {
            des0 |= DW_MMC_DESC_LD;
        }
        stl_le_p(d, des0);
        stl_le_p(d + 4, DW_MMC_BLOCK_SIZE);             /* buffer1 size */
        stl_le_p(d + 8,
                 DW_MMC_IDMAC_DEST_BASE + i * DW_MMC_BLOCK_SIZE);
        stl_le_p(d + 12, next);                          /* next desc */
    }
    qtest_memwrite(qts, DW_MMC_IDMAC_DESC_BASE, descs,
                   DW_MMC_IDMAC_DESC_COUNT * 16);

    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_PWREN, 1);
    rock5b_sdmmc_init_card(qts);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_CTRL,
                 DW_MMC_CTRL_INT_ENABLE | DW_MMC_CTRL_DMA_ENABLE |
                 DW_MMC_CTRL_USE_IDMAC);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_BMOD,
                 DW_MMC_BMOD_ENABLE);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_DBADDR,
                 DW_MMC_IDMAC_DESC_BASE);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_BLKSIZ,
                 DW_MMC_BLOCK_SIZE);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_BYTCNT,
                 DW_MMC_IDMAC_DESC_COUNT * DW_MMC_BLOCK_SIZE);
    qtest_writel(qts, ROCK5B_SDMMC_BASE + DW_MMC_INTMASK,
                 DW_MMC_INT_DATA_OVER);

    rock5b_sdmmc_command(qts, 18, 0,
                         DW_MMC_CMD_RESP_EXP | DW_MMC_CMD_DAT_EXP);
    raw = qtest_readl(qts, ROCK5B_SDMMC_BASE + DW_MMC_RINTSTS);
    g_assert_cmphex(raw & DW_MMC_INT_DATA_OVER, ==, DW_MMC_INT_DATA_OVER);

    for (unsigned int i = 0; i < ARRAY_SIZE(samples); i++) {
        unsigned int block = samples[i];

        qtest_memread(qts, DW_MMC_IDMAC_DEST_BASE +
                      (uint64_t)block * DW_MMC_BLOCK_SIZE,
                      actual, sizeof(actual));
        rock5b_sdmmc_block_pattern(block, expected);
        g_assert_cmpmem(actual, sizeof(actual), expected, sizeof(expected));
    }

    qtest_quit(qts);
    unlink(sd_path);
}

/*
 * The CRU clock/reset banks use the Rockchip high-word write mask: the upper
 * 16 bits of the written value select the bits that are modified.  Real
 * firmware writes 0x30003000 to the reset-status bank at 0x30a04 and expects
 * 0x3000 when reading it back.
 */
static void test_cru_reset_bank(void)
{
    QTestState *qts = qtest_init("-machine rock-5b -m 256M -serial null "
                                 "-display none");

    qtest_writel(qts, ROCK5B_CRU_BASE + 0x30a04, 0x30003000);
    g_assert_cmphex(qtest_readl(qts, ROCK5B_CRU_BASE + 0x30a04), ==, 0x3000);

    /* A zero write mask changes nothing. */
    qtest_writel(qts, ROCK5B_CRU_BASE + 0x30a04, 0x00008000);
    g_assert_cmphex(qtest_readl(qts, ROCK5B_CRU_BASE + 0x30a04), ==, 0x3000);

    /* A full mask replaces only the low 16 bits with the supplied data. */
    qtest_writel(qts, ROCK5B_CRU_BASE + 0x30a04, 0xffff0030);
    g_assert_cmphex(qtest_readl(qts, ROCK5B_CRU_BASE + 0x30a04), ==, 0x30);

    /* Clock-gate registers follow the same high-word-mask rule. */
    qtest_writel(qts, ROCK5B_CRU_BASE + 0x0844, 0x00040004);
    g_assert_cmphex(qtest_readl(qts, ROCK5B_CRU_BASE + 0x0844), ==, 0x4);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/rock-5b/machine-topology", test_machine_topology);
    qtest_add_func("/rock-5b/pcie-rtl8125", test_pcie_rtl8125);
    qtest_add_func("/rock-5b/sdmmc", test_sdmmc);
    qtest_add_func("/rock-5b/cru", test_cru_reset_bank);
    qtest_add_func("/rock-5b/sdmmc-idmac", test_sdmmc_idmac);
    qtest_add_func("/rock-5b/uart2-chardev", test_uart2_chardev);
    qtest_add_func("/rock-5b/uart2-rx-fifo-irq", test_uart2_rx_fifo_irq);
    qtest_add_func("/rock-5b/uart3-isolation", test_uart3_isolation);

    return g_test_run();
}
