/*
 * QTest for the SiFli SF32LB52X board machine model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "qapi/error.h"
#include "libqtest.h"

#define SF32LB52X_MACHINE "sf32lb52-devkit-lcd"

/* Memory map */
#define SF32LB52X_SRAM_BASE        0x20000000ULL
#define SF32LB52X_SRAM_SIZE        0x00080000ULL
#define SF32LB52X_SHARED_SRAM_BASE 0x20400000ULL
#define SF32LB52X_SHARED_SRAM_SIZE 0x00010000ULL
#define SF32LB52X_XIP_BASE         0x12000000ULL
#define SF32LB52X_XIP_SIZE         0x01000000ULL
#define SF32LB52X_XIP_IMAGE_ADDR   0x12010000ULL

/* Peripherals */
#define SF32LB52X_RCC_BASE       0x50000000ULL
#define SF32LB52X_PINMUX_BASE    0x50003000ULL
#define SF32LB52X_HPSYS_CFG_BASE 0x5000b000ULL
#define SF32LB52X_GPIO_BASE      0x500a0000ULL
#define SF32LB52X_USART1_BASE    0x50084000ULL
#define SF32LB52X_AON_BASE       0x500c0000ULL
#define SF32LB52X_PMUC_BASE      0x500ca000ULL

/* RCC registers */
#define RCC_CSR    0x20
#define RCC_CFGR   0x24
#define RCC_USBCR  0x28
#define RCC_DLL1CR 0x2c

#define RCC_DLL1CR_EN    (1U << 0)
#define RCC_DLL1CR_READY (1U << 31)

/* AON registers */
#define AON_ACR 0x10

#define AON_ACR_HXT48_REQ (1U << 1)
#define AON_ACR_HXT48_RDY (1U << 31)

/* HPSYS_CFG registers */
#define HPSYS_CFG_CAU2_CR 0x94

/* PINMUX pad registers */
#define PINMUX_PA18_PAD 0x7c
#define PINMUX_PA19_PAD 0x80

/* PMUC registers */
#define PMUC_HXT_CR1 0x68

/* USART registers */
#define USART_CR1 0x00
#define USART_ISR 0x1c
#define USART_ICR 0x20
#define USART_RDR 0x24
#define USART_TDR 0x28

#define USART_CR1_UE (1U << 0)
#define USART_CR1_RE (1U << 2)
#define USART_CR1_TE (1U << 3)

#define USART_ISR_RXNE (1U << 5)
#define USART_ISR_TC   (1U << 6)
#define USART_ISR_TXE  (1U << 7)

#define USART_ISR_RESET 0x020000c0

#define USART_ICR_TCCF (1U << 6)

#define USART_CR1_TCIE (1U << 6)

/* Round-1 device windows (all modeled, must not be unimp placeholders). */
#define SF32LB52X_RTC_BASE       0x500cb000ULL
#define SF32LB52X_WDT_BASE       0x50094000ULL
#define SF32LB52X_CRC_BASE       0x50048000ULL
#define SF32LB52X_TRNG_BASE      0x5000f000ULL
#define SF32LB52X_TSEN_BASE      0x50089000ULL
#define SF32LB52X_GPADC_BASE     0x50087000ULL
#define SF32LB52X_EFUSE_BASE     0x5000c000ULL

/* Round-2 device windows (models present in this worktree). */
#define SF32LB52X_I2C1_BASE      0x5009c000ULL
#define SF32LB52X_I2C_STRIDE     0x00001000ULL
#define SF32LB52X_SPI1_BASE      0x50095000ULL
#define SF32LB52X_SPI2_BASE      0x50096000ULL
#define SF32LB52X_DMAC_BASE      0x50081000ULL
#define SF32LB52X_MPI2_BASE      0x50042000ULL

/* Round-3 device windows (models present in this worktree). */
#define SF32LB52X_LCDC_BASE      0x50008000ULL
#define SF32LB52X_AUDCODEC_BASE  0x50088000ULL
#define SF32LB52X_MAILBOX1_BASE  0x50082000ULL
#define SF32LB52X_MAILBOX2_BASE  0x40002000ULL
#define SF32LB52X_ATIM1_BASE     0x50004000ULL
#define SF32LB52X_GPTIM2_BASE    0x500b0000ULL

/* External IRQ numbers (DTSI sf32lb52x.dtsi). */
#define SF32LB52X_USART1_IRQ     59
#define SF32LB52X_RTC_IRQ        49
#define SF32LB52X_GPADC_IRQ      65

/* RTC registers and alarm bits (round-1 model, rtc.h). */
#define RTC_TR      0x00
#define RTC_DR      0x04
#define RTC_CR      0x08
#define RTC_ISR     0x0c
#define RTC_ALRMTR  0x18
#define RTC_ALRMDR  0x1c

#define RTC_ISR_ALRMF  (1U << 1)
#define RTC_ISR_INIT   (1U << 10)
#define RTC_CR_ALRME   (1U << 8)
#define RTC_CR_ALRMIE  (1U << 11)

#define RTC_ALRMDR_MSKMN (1U << 25)
#define RTC_ALRMDR_MSKD  (1U << 27)
#define RTC_ALRMDR_MSKM  (1U << 28)
#define RTC_ALRMDR_MSKWD (1U << 29)

/* GPADC registers and bits (round-1 model, gpadc.h/SVD). */
#define GPADC_CFG1        0x00
#define GPADC_SLOT(n)     (0x04 + 4 * (n))
#define GPADC_CTRL        0x38
#define GPADC_IRQ         0x44

#define GPADC_SLOT_RESET  0x00020801U
#define GPADC_SLOT_EN     (1U << 0)
#define GPADC_SLOT_PCHNL(ch) ((uint32_t)(ch) << 8)
#define GPADC_CTRL_START  (1U << 1)
#define GPADC_IRQ_ICR     (1U << 0)

#define ONE_SEC_NS 1000000000LL

#define CONSOLE_TIMEOUT (5 * G_TIME_SPAN_SECOND)

static QTestState *sf32lb52x_qtest_start(void)
{
    return qtest_init("-machine " SF32LB52X_MACHINE " -serial null");
}

static QTestState *sf32lb52x_qtest_start_serial(int *sock_fd)
{
    QTestState *qts = qtest_init_with_serial("-machine " SF32LB52X_MACHINE,
                                             sock_fd);

    qemu_set_blocking(*sock_fd, false, &error_abort);
    return qts;
}

static uint32_t rcc_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, SF32LB52X_RCC_BASE + offset);
}

static void rcc_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, SF32LB52X_RCC_BASE + offset, value);
}

static uint32_t aon_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, SF32LB52X_AON_BASE + offset);
}

static void aon_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, SF32LB52X_AON_BASE + offset, value);
}

static uint32_t hpsys_cfg_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, SF32LB52X_HPSYS_CFG_BASE + offset);
}

static void hpsys_cfg_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, SF32LB52X_HPSYS_CFG_BASE + offset, value);
}

static uint32_t pinmux_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, SF32LB52X_PINMUX_BASE + offset);
}

static void pinmux_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, SF32LB52X_PINMUX_BASE + offset, value);
}

static uint32_t usart1_readl(QTestState *qts, uint64_t offset)
{
    return qtest_readl(qts, SF32LB52X_USART1_BASE + offset);
}

static void usart1_writel(QTestState *qts, uint64_t offset, uint32_t value)
{
    qtest_writel(qts, SF32LB52X_USART1_BASE + offset, value);
}

static uint8_t console_recv_byte(int sock_fd)
{
    gint64 deadline = g_get_monotonic_time() + CONSOLE_TIMEOUT;
    uint8_t byte;

    do {
        ssize_t ret = recv(sock_fd, &byte, sizeof(byte), 0);

        if (ret == 1) {
            return byte;
        }
        g_assert_cmpint(ret, ==, -1);
        g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EINTR);
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    g_assert_not_reached();
}

static void console_send_byte(int sock_fd, uint8_t byte)
{
    gint64 deadline = g_get_monotonic_time() + CONSOLE_TIMEOUT;

    do {
        ssize_t ret = send(sock_fd, &byte, sizeof(byte), 0);

        if (ret == 1) {
            return;
        }
        g_assert_cmpint(ret, ==, -1);
        g_assert_true(errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == EINTR);
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    g_assert_not_reached();
}

static uint32_t usart1_wait_isr(QTestState *qts, uint32_t mask)
{
    gint64 deadline = g_get_monotonic_time() + CONSOLE_TIMEOUT;
    uint32_t isr;

    do {
        isr = usart1_readl(qts, USART_ISR);
        if ((isr & mask) == mask) {
            return isr;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    return isr;
}

static void test_machine_constructs(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    qtest_quit(qts);
}

static void test_sram_read_write(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    qtest_writel(qts, SF32LB52X_SRAM_BASE, 0xdeadbeef);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SRAM_BASE), ==, 0xdeadbeef);

    qtest_writel(qts, SF32LB52X_SRAM_BASE + 0x1000, 0x5555aaaa);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SRAM_BASE + 0x1000), ==,
                    0x5555aaaa);

    qtest_writeb(qts, SF32LB52X_SRAM_BASE + 0x800, 0xa5);
    qtest_writew(qts, SF32LB52X_SRAM_BASE + 0x802, 0x5aa5);
    g_assert_cmphex(qtest_readb(qts, SF32LB52X_SRAM_BASE + 0x800), ==, 0xa5);
    g_assert_cmphex(qtest_readw(qts, SF32LB52X_SRAM_BASE + 0x802), ==, 0x5aa5);

    qtest_writel(qts, SF32LB52X_SRAM_BASE + SF32LB52X_SRAM_SIZE - 4,
                 0x12345678);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SRAM_BASE +
                                SF32LB52X_SRAM_SIZE - 4), ==, 0x12345678);

    qtest_quit(qts);
}

static void test_shared_sram_read_write(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    qtest_writel(qts, SF32LB52X_SHARED_SRAM_BASE, 0xcafef00d);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SHARED_SRAM_BASE), ==,
                    0xcafef00d);

    qtest_writel(qts, SF32LB52X_SHARED_SRAM_BASE + 0x100, 0x0badf00d);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SHARED_SRAM_BASE + 0x100), ==,
                    0x0badf00d);

    qtest_writel(qts, SF32LB52X_SHARED_SRAM_BASE +
                 SF32LB52X_SHARED_SRAM_SIZE - 4, 0x5a5a5a5a);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_SHARED_SRAM_BASE +
                                SF32LB52X_SHARED_SRAM_SIZE - 4), ==,
                    0x5a5a5a5a);

    qtest_quit(qts);
}

/*
 * The XIP window is RAM-backed so that MPI2 program/erase and CPU XIP share
 * one backing store (see hw/arm/sf32lb52x.c). Without -kernel the window
 * reads as zero and is stable; direct CPU stores are a documented model
 * simplification of the RAM backing, so this test checks the baseline, not
 * read-only trap behaviour.
 */
static void test_xip_window_baseline(void)
{
    QTestState *qts = sf32lb52x_qtest_start();
    static const uint64_t windows[] = {
        SF32LB52X_XIP_IMAGE_ADDR,
        SF32LB52X_XIP_BASE,
        SF32LB52X_XIP_BASE + SF32LB52X_XIP_SIZE - 8,
    };
    uint8_t first[8], second[8];
    size_t i, j;

    for (i = 0; i < ARRAY_SIZE(windows); i++) {
        qtest_memread(qts, windows[i], first, sizeof(first));
        qtest_memread(qts, windows[i], second, sizeof(second));
        g_assert_cmpmem(first, sizeof(first), second, sizeof(second));

        /* No -kernel was given: the whole XIP window is uninitialized ROM. */
        for (j = 0; j < sizeof(first); j++) {
            g_assert_cmphex(first[j], ==, 0);
        }
    }

    qtest_quit(qts);
}

static void test_rcc_store(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    rcc_writel(qts, RCC_CSR, 0x12345678);
    rcc_writel(qts, RCC_CFGR, 0x0badf00d);
    rcc_writel(qts, RCC_USBCR, 0x00000007);

    g_assert_cmphex(rcc_readl(qts, RCC_CSR), ==, 0x12345678);
    g_assert_cmphex(rcc_readl(qts, RCC_CFGR), ==, 0x0badf00d);
    g_assert_cmphex(rcc_readl(qts, RCC_USBCR), ==, 0x00000007);

    qtest_quit(qts);
}

static void test_rcc_dll1_ready(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    g_assert_cmphex(rcc_readl(qts, RCC_DLL1CR) & RCC_DLL1CR_READY, ==, 0);

    rcc_writel(qts, RCC_DLL1CR, RCC_DLL1CR_EN);
    g_assert_cmphex(rcc_readl(qts, RCC_DLL1CR) & RCC_DLL1CR_READY, ==,
                    RCC_DLL1CR_READY);

    qtest_quit(qts);
}

static void test_aon_hxt48_ready(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    g_assert_cmphex(aon_readl(qts, AON_ACR) & AON_ACR_HXT48_RDY, ==, 0);

    aon_writel(qts, AON_ACR, AON_ACR_HXT48_REQ);
    g_assert_cmphex(aon_readl(qts, AON_ACR) & AON_ACR_HXT48_RDY, ==,
                    AON_ACR_HXT48_RDY);

    qtest_quit(qts);
}

static void test_hpsys_cfg_cau2_cr(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    hpsys_cfg_writel(qts, HPSYS_CFG_CAU2_CR, 0x12345678);
    g_assert_cmphex(hpsys_cfg_readl(qts, HPSYS_CFG_CAU2_CR), ==, 0x12345678);

    hpsys_cfg_writel(qts, HPSYS_CFG_CAU2_CR, 0x00000000);
    g_assert_cmphex(hpsys_cfg_readl(qts, HPSYS_CFG_CAU2_CR), ==, 0x00000000);

    qtest_quit(qts);
}

static void test_pinmux_pads(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    pinmux_writel(qts, PINMUX_PA18_PAD, 0x00001234);
    pinmux_writel(qts, PINMUX_PA19_PAD, 0x00005678);

    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA18_PAD), ==, 0x00001234);
    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA19_PAD), ==, 0x00005678);

    pinmux_writel(qts, PINMUX_PA18_PAD, 0x0000abcd);

    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA18_PAD), ==, 0x0000abcd);
    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA19_PAD), ==, 0x00005678);

    qtest_quit(qts);
}

static void test_usart1_console(void)
{
    static const uint8_t tx_byte = 0x41;
    QTestState *qts;
    uint8_t rdr[4];
    int sock_fd;

    qts = sf32lb52x_qtest_start_serial(&sock_fd);

    g_assert_cmphex(usart1_readl(qts, USART_ISR), ==, USART_ISR_RESET);

    /* Enable the transmitter, clear TC, and check a TDR write sets TC. */
    usart1_writel(qts, USART_CR1, USART_CR1_UE | USART_CR1_TE);
    usart1_writel(qts, USART_ICR, USART_ICR_TCCF);
    g_assert_cmphex(usart1_readl(qts, USART_ISR) & USART_ISR_TC, ==, 0);

    qtest_memwrite(qts, SF32LB52X_USART1_BASE + USART_TDR, &tx_byte,
                   sizeof(tx_byte));
    g_assert_cmphex(usart1_readl(qts, USART_ISR) & USART_ISR_TC, ==,
                    USART_ISR_TC);
    g_assert_cmphex(console_recv_byte(sock_fd), ==, 0x41);

    /* Receiving requires CR1.RE in addition to CR1.UE. */
    usart1_writel(qts, USART_CR1,
                  USART_CR1_UE | USART_CR1_RE | USART_CR1_TE);
    console_send_byte(sock_fd, 0x42);
    g_assert_cmphex(usart1_wait_isr(qts, USART_ISR_RXNE) & USART_ISR_RXNE,
                    ==, USART_ISR_RXNE);

    qtest_memread(qts, SF32LB52X_USART1_BASE + USART_RDR, rdr, sizeof(rdr));
    g_assert_cmphex(rdr[0], ==, 0x42);

    close(sock_fd);
    qtest_quit(qts);
}

static void test_system_reset(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    rcc_writel(qts, RCC_CSR, 0x12345678);
    aon_writel(qts, AON_ACR, AON_ACR_HXT48_REQ);
    hpsys_cfg_writel(qts, HPSYS_CFG_CAU2_CR, 0x12345678);
    pinmux_writel(qts, PINMUX_PA18_PAD, 0x00001234);
    qtest_writel(qts, SF32LB52X_PMUC_BASE + PMUC_HXT_CR1, 0x12345678);
    qtest_writel(qts, SF32LB52X_GPIO_BASE, 0x12345678);

    /* Make sure every bank is dirty before the reset. */
    g_assert_cmphex(rcc_readl(qts, RCC_CSR), ==, 0x12345678);
    g_assert_cmphex(aon_readl(qts, AON_ACR) & AON_ACR_HXT48_REQ, ==,
                    AON_ACR_HXT48_REQ);
    g_assert_cmphex(hpsys_cfg_readl(qts, HPSYS_CFG_CAU2_CR), ==, 0x12345678);
    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA18_PAD), ==, 0x00001234);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_PMUC_BASE + PMUC_HXT_CR1), ==,
                    0x12345678);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_GPIO_BASE), ==, 0x12345678);

    qtest_system_reset(qts);

    g_assert_cmphex(rcc_readl(qts, RCC_CSR), ==, 0);
    g_assert_cmphex(aon_readl(qts, AON_ACR), ==, 0);
    g_assert_cmphex(hpsys_cfg_readl(qts, HPSYS_CFG_CAU2_CR), ==, 0);
    g_assert_cmphex(pinmux_readl(qts, PINMUX_PA18_PAD), ==, 0);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_PMUC_BASE + PMUC_HXT_CR1), ==,
                    0);
    g_assert_cmphex(qtest_readl(qts, SF32LB52X_GPIO_BASE), ==, 0);
    g_assert_cmphex(usart1_readl(qts, USART_ISR), ==, USART_ISR_RESET);

    qtest_quit(qts);
}

static void test_kernel_load(void)
{
    static const uint8_t image[16] = {
        0x00, 0x10, 0x00, 0x20,  /* initial SP: 0x20001000 */
        0x01, 0x01, 0x01, 0x12,  /* reset PC: 0x12010101 */
    };
    uint8_t loaded[sizeof(image)];
    g_autoptr(GError) error = NULL;
    g_autofree char *path = NULL;
    QTestState *qts;
    int fd;

    fd = g_file_open_tmp("sf32lb52x-kernel-XXXXXX", &path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, image, sizeof(image)), ==, sizeof(image));
    g_assert_cmpint(close(fd), ==, 0);

    qts = qtest_initf("-machine " SF32LB52X_MACHINE
                      " -kernel %s -serial null", path);
    qtest_memread(qts, SF32LB52X_XIP_IMAGE_ADDR, loaded, sizeof(loaded));
    g_assert_cmpmem(loaded, sizeof(loaded), image, sizeof(image));
    qtest_quit(qts);

    g_assert_cmpint(g_unlink(path), ==, 0);
}

static uint32_t rtc_readl(QTestState *qts, uint32_t offset)
{
    return qtest_readl(qts, SF32LB52X_RTC_BASE + offset);
}

static void rtc_writel(QTestState *qts, uint32_t offset, uint32_t value)
{
    qtest_writel(qts, SF32LB52X_RTC_BASE + offset, value);
}

/* BCD encoders for the TR/DR register layout (rtc.h). */
static uint32_t bcd_time(unsigned hour, unsigned min, unsigned sec)
{
    return (((hour / 10U) & 0x3U) << 29) | ((hour % 10U) << 25) |
           (((min / 10U) & 0x7U) << 22) | ((min % 10U) << 18) |
           (((sec / 10U) & 0x7U) << 15) | ((sec % 10U) << 11);
}

static uint32_t bcd_date(unsigned year, unsigned month, unsigned day,
                         unsigned weekday)
{
    return (((year / 10U) & 0xfU) << 20) | ((year % 10U) << 16) |
           ((weekday & 0x7U) << 13) | (((month / 10U) & 0x1U) << 12) |
           ((month % 10U) << 8) | (((day / 10U) & 0x3U) << 4) | (day % 10U);
}

/* Driver-style ISR update: read-modify-write, INIT/flag bits as written. */
static void rtc_isr_rmw(QTestState *qts, uint32_t set, uint32_t clear)
{
    uint32_t isr = rtc_readl(qts, RTC_ISR);

    rtc_writel(qts, RTC_ISR, (isr | set) & ~clear);
}

static void rtc_enter_init(QTestState *qts)
{
    rtc_isr_rmw(qts, RTC_ISR_INIT, 0);
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_INIT, ==, RTC_ISR_INIT);
}

static void rtc_exit_init(QTestState *qts)
{
    rtc_isr_rmw(qts, 0, RTC_ISR_INIT);
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_INIT, ==, 0);
}

static void rtc_set_datetime(QTestState *qts, uint32_t tr, uint32_t dr)
{
    rtc_enter_init(qts);
    rtc_writel(qts, RTC_TR, tr);
    rtc_writel(qts, RTC_DR, dr);
    rtc_exit_init(qts);
}

/* Observe the armv7m NVIC inputs: line index == external IRQ number. */
static void nvic_irq_intercept(QTestState *qts)
{
    qtest_irq_intercept_in(qts, "/machine/soc/armv7m");
}

/*
 * End-to-end IRQ delivery: device output level and SoC routing to the
 * armv7m NVIC input are both exercised (round-1 re-review MINOR 6).
 */
static void test_nvic_usart1_delivery(void)
{
    QTestState *qts = sf32lb52x_qtest_start();

    nvic_irq_intercept(qts);
    g_assert_cmpint(qtest_get_irq(qts, SF32LB52X_USART1_IRQ), ==, 0);

    /* TC is set at reset; enabling TCIE must reach NVIC IRQ 59. */
    usart1_writel(qts, USART_CR1, USART_CR1_TCIE);
    g_assert_cmpint(qtest_get_irq(qts, SF32LB52X_USART1_IRQ), ==, 1);

    usart1_writel(qts, USART_CR1, 0);
    g_assert_cmpint(qtest_get_irq(qts, SF32LB52X_USART1_IRQ), ==, 0);

    qtest_quit(qts);
}

static void test_nvic_rtc_delivery(void)
{
    QTestState *qts = sf32lb52x_qtest_start();
    uint32_t isr;

    nvic_irq_intercept(qts);
    rtc_set_datetime(qts, bcd_time(12, 0, 0), bcd_date(24, 1, 7, 1));
    rtc_writel(qts, RTC_ALRMTR, bcd_time(12, 0, 1));
    rtc_writel(qts, RTC_ALRMDR,
               RTC_ALRMDR_MSKD | RTC_ALRMDR_MSKM | RTC_ALRMDR_MSKWD);
    rtc_writel(qts, RTC_CR, RTC_CR_ALRME | RTC_CR_ALRMIE);
    g_assert_cmpint(qtest_get_irq(qts, SF32LB52X_RTC_IRQ), ==, 0);

    qtest_clock_step(qts, ONE_SEC_NS);
    g_assert_cmphex(rtc_readl(qts, RTC_ISR) & RTC_ISR_ALRMF, ==,
                    RTC_ISR_ALRMF);
    g_assert_cmpint(qtest_get_irq(qts, SF32LB52X_RTC_IRQ), ==, 1);

    /* rw0c: clearing the alarm flag drops the delivered level. */
    isr = rtc_readl(qts, RTC_ISR);
    rtc_writel(qts, RTC_ISR, isr & ~RTC_ISR_ALRMF);
    g_assert_cmpint(qtest_get_irq(qts, SF32LB52X_RTC_IRQ), ==, 0);

    qtest_quit(qts);
}

static void test_nvic_gpadc_delivery(void)
{
    QTestState *qts = sf32lb52x_qtest_start();
    unsigned int i;

    nvic_irq_intercept(qts);
    /* SLOTn reset with SLOT_EN set; disable them, then enable slot 0. */
    for (i = 0; i < 8; i++) {
        qtest_writel(qts, SF32LB52X_GPADC_BASE + GPADC_SLOT(i),
                     GPADC_SLOT_RESET & ~GPADC_SLOT_EN);
    }
    qtest_writel(qts, SF32LB52X_GPADC_BASE + GPADC_SLOT(0),
                 GPADC_SLOT_EN | GPADC_SLOT_PCHNL(0));
    g_assert_cmpint(qtest_get_irq(qts, SF32LB52X_GPADC_IRQ), ==, 0);

    qtest_writel(qts, SF32LB52X_GPADC_BASE + GPADC_CTRL, GPADC_CTRL_START);
    g_assert_cmpint(qtest_get_irq(qts, SF32LB52X_GPADC_IRQ), ==, 1);

    qtest_writel(qts, SF32LB52X_GPADC_BASE + GPADC_IRQ, GPADC_IRQ_ICR);
    g_assert_cmpint(qtest_get_irq(qts, SF32LB52X_GPADC_IRQ), ==, 0);

    qtest_quit(qts);
}

/*
 * AC1 regression: every modeled peripheral window must be served by its
 * device model, not by a create_unimplemented_device() placeholder.
 */
static void test_modeled_device_windows(void)
{
    static const uint64_t windows[] = {
        SF32LB52X_RTC_BASE, SF32LB52X_WDT_BASE, SF32LB52X_CRC_BASE,
        SF32LB52X_TRNG_BASE, SF32LB52X_TSEN_BASE, SF32LB52X_GPADC_BASE,
        SF32LB52X_EFUSE_BASE,
        SF32LB52X_I2C1_BASE,
        SF32LB52X_I2C1_BASE + SF32LB52X_I2C_STRIDE,
        SF32LB52X_I2C1_BASE + 2 * SF32LB52X_I2C_STRIDE,
        SF32LB52X_I2C1_BASE + 3 * SF32LB52X_I2C_STRIDE,
        SF32LB52X_SPI1_BASE, SF32LB52X_SPI2_BASE,
        SF32LB52X_DMAC_BASE, SF32LB52X_MPI2_BASE,
        SF32LB52X_LCDC_BASE, SF32LB52X_AUDCODEC_BASE,
        SF32LB52X_MAILBOX1_BASE, SF32LB52X_MAILBOX2_BASE,
        SF32LB52X_ATIM1_BASE, SF32LB52X_GPTIM2_BASE,
    };
    g_autofree gchar *log_path = NULL;
    g_autofree gchar *log = NULL;
    QTestState *qts;
    GError *error = NULL;
    size_t i;
    int fd;

    fd = g_file_open_tmp("sf32lb52x-unimp-XXXXXX", &log_path, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(close(fd), ==, 0);
    qts = qtest_initf("-machine " SF32LB52X_MACHINE
                      " -serial null -d guest_errors,unimp -D %s",
                      log_path);

    for (i = 0; i < ARRAY_SIZE(windows); i++) {
        (void)qtest_readl(qts, windows[i]);
    }

    /* Force the log to be written and prove the capture works. */
    (void)qtest_readl(qts, SF32LB52X_RTC_BASE + 0xffc);
    qtest_quit(qts);

    g_assert_true(g_file_get_contents(log_path, &log, NULL, NULL));
    g_assert_nonnull(strstr(log, "bad offset"));
    g_assert_null(strstr(log, "unimplemented device"));
    g_assert_cmpint(g_unlink(log_path), ==, 0);
}

/*
 * Round-2 models expose their sysbus IRQ lines. Delivery to the NVIC is
 * covered once the R2 devices drive those lines (tracked in the report).
 */
static void test_r2_irq_lines_present(void)
{
    static const char *const paths[] = {
        "/machine/soc/i2c[0]",
        "/machine/soc/i2c[3]",
        "/machine/soc/spi[0]",
        "/machine/soc/spi[1]",
        "/machine/soc/dmac",
    };
    size_t i;

    for (i = 0; i < ARRAY_SIZE(paths); i++) {
        QTestState *qts = sf32lb52x_qtest_start();

        /* Succeeds only if the named sysbus IRQ line exists. */
        qtest_irq_intercept_out_named(qts, paths[i], "sysbus-irq");
        qtest_quit(qts);
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    qtest_add_func("/sf32lb52x/machine/constructs",
                   test_machine_constructs);
    qtest_add_func("/sf32lb52x/sram/read-write", test_sram_read_write);
    qtest_add_func("/sf32lb52x/shared-sram/read-write",
                   test_shared_sram_read_write);
    qtest_add_func("/sf32lb52x/xip/window-baseline",
                   test_xip_window_baseline);
    qtest_add_func("/sf32lb52x/rcc/store", test_rcc_store);
    qtest_add_func("/sf32lb52x/rcc/dll1-ready", test_rcc_dll1_ready);
    qtest_add_func("/sf32lb52x/aon/hxt48-ready", test_aon_hxt48_ready);
    qtest_add_func("/sf32lb52x/hpsys-cfg/cau2-cr", test_hpsys_cfg_cau2_cr);
    qtest_add_func("/sf32lb52x/pinmux/pads", test_pinmux_pads);
    qtest_add_func("/sf32lb52x/usart1/console", test_usart1_console);
    qtest_add_func("/sf32lb52x/syscon/reset", test_system_reset);
    qtest_add_func("/sf32lb52x/kernel/load", test_kernel_load);
    qtest_add_func("/sf32lb52x/nvic/usart1-delivery",
                   test_nvic_usart1_delivery);
    qtest_add_func("/sf32lb52x/nvic/rtc-delivery",
                   test_nvic_rtc_delivery);
    qtest_add_func("/sf32lb52x/nvic/gpadc-delivery",
                   test_nvic_gpadc_delivery);
    qtest_add_func("/sf32lb52x/modeled-windows",
                   test_modeled_device_windows);
    qtest_add_func("/sf32lb52x/r2-irq-lines-present",
                   test_r2_irq_lines_present);

    return g_test_run();
}
