/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Smoke tests for the Pollen MicroDuck beta (Rockchip RK3566) machine.
 *
 * Copyright (c) 2026 Process Mission
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "rk3566-test-common.h"

#define RK3566_TEST_RAM_SIZE (512 * MiB)

/* ISP IOMMU window and registers from the microduck-beta board map. */
#define RK3566_ISP_IOMMU_BASE 0xfdff1a00ULL
#define RK3566_IOMMU_DTE_ADDR 0x00
#define RK3566_IOMMU_STATUS 0x04
#define RK3566_IOMMU_AUTO_GATING 0x24
#define RK3566_IOMMU_STATUS_RESET 0x80000018U

static void test_rk3566_machine_creation(void)
{
    QTestState *qts = rk3566_qtest_start();
    g_autofree char *machine_type =
        rk3566_qom_get_str(qts, "/machine", "type");
    g_autofree char *ram_backend =
        rk3566_qom_get_str(qts, "/machine", "memory-backend");
    g_autofree char *uart0_type =
        rk3566_qom_child_object_type(qts, "/machine", "uart0");
    g_autofree char *gic_type =
        rk3566_qom_child_object_type(qts, "/machine", "gic");
    g_autofree char *timer_type =
        rk3566_qom_child_object_type(qts, "/machine", "timer0");
    g_autofree char *otp_type =
        rk3566_qom_child_object_type(qts, "/machine", "otp");
    g_autofree char *cru_type =
        rk3566_qom_child_object_type(qts, "/machine", "cru");
    g_autofree char *npu_type =
        rk3566_qom_child_object_type(qts, "/machine", "npu");
    g_autofree char *ram_type =
        rk3566_qom_child_object_type(qts, "/objects", "microduck.ram");

    g_assert_true(rk3566_qom_object_exists(qts, "/machine"));
    g_assert_cmpstr(machine_type, ==, RK3566_MACHINE_TYPE);

    g_assert_cmpstr(uart0_type, ==, "dw8250");
    g_assert_cmpstr(gic_type, ==, "arm-gicv3");
    g_assert_cmpstr(timer_type, ==, "rk3566-timer");
    g_assert_cmpstr(otp_type, ==, "rk3566-otp");
    g_assert_cmpstr(cru_type, ==, "rk3566-cru");
    g_assert_cmpstr(npu_type, ==, "rk3566-rknpu");

    /*
     * The machine alias of the default RAM backend is the guest DRAM region;
     * check the backend object and its size through QOM.
     */
    g_assert_cmpstr(ram_backend, ==, "/objects/microduck.ram");
    g_assert_true(rk3566_qom_object_exists(qts, ram_backend));
    g_assert_cmpstr(ram_type, ==, "memory-backend-ram");
    g_assert_cmpuint(rk3566_qom_get_uint64(qts, ram_backend, "size"), ==,
                     RK3566_TEST_RAM_SIZE);
    g_assert_true(rk3566_qom_child_type_exists(qts, "/machine",
                                               "memory-region"));

    qtest_quit(qts);
}

static void test_rk3566_mmio_reset_values(void)
{
    QTestState *qts = rk3566_qtest_start();
    uint32_t value;

    /* The GICv3 distributor reports 320 interrupt lines: ITLinesNumber 9. */
    value = rk3566_mmio_read32_at(qts, RK3566_GICD_BASE, RK3566_GICD_TYPER);
    g_assert_cmphex(value & 0x1f, ==, 0x9);

    /* UART0 reports empty transmitter holding and shift registers. */
    value = rk3566_mmio_read32_at(qts, RK3566_UART0_BASE, RK3566_DW8250_LSR);
    g_assert_cmphex(value & 0x60, ==, 0x60);

    /* Timer0 starts with a zero load value, control and status. */
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                        RK3566_TIMER_LOAD), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                        RK3566_TIMER_LOAD_HI), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                        RK3566_TIMER_CONTROL), ==, 0);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                        RK3566_TIMER_STATUS), ==, 0);

    /* The timer load register is writable and reads back. */
    rk3566_mmio_write32_at(qts, RK3566_TIMER0_BASE, RK3566_TIMER_LOAD,
                           0x12345678);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_TIMER0_BASE,
                                        RK3566_TIMER_LOAD), ==,
                    0x12345678);

    /*
     * Read the identification word programmed at fuse byte 2 through the OTP
     * indirect access window.  The writes below 0x120 use the upper 16 bits
     * as a write mask.
     */
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                        RK3566_OTP_STATUS), ==, 0);
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, RK3566_OTP_CTRL,
                           (1u << 16) | 1);
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, RK3566_OTP_ADDR,
                           (1u << 16) | (RK3566_OTP_FUSE_BYTE_ADDR / 2));
    rk3566_mmio_write32_at(qts, RK3566_OTP_BASE, RK3566_OTP_TRIGGER,
                           (1u << 16) | 1);
    g_assert_cmphex(rk3566_mmio_read32_at(qts, RK3566_OTP_BASE,
                                        RK3566_OTP_DOUT), ==, 0x6635);
    value = rk3566_mmio_read32_at(qts, RK3566_OTP_BASE, RK3566_OTP_STATUS);
    g_assert_cmphex(value & RK3566_OTP_STATUS_READ_DONE, ==,
                    RK3566_OTP_STATUS_READ_DONE);

    qtest_quit(qts);
}

static void test_rk3566_media_devices(void)
{
    QTestState *qts = rk3566_qtest_start();
    g_autofree char *isp_type = NULL;
    g_autofree char *dphy_type = NULL;
    g_autofree char *rga_type = NULL;
    g_autofree char *venc_type = NULL;

    if (!rk3566_qtest_media_present(qts)) {
        g_test_skip("RK3566 media devices are not built "
                    "(libavcodec, libavutil, libswscale or x264 missing)");
        qtest_quit(qts);
        return;
    }

    isp_type = rk3566_qom_child_object_type(qts, "/machine", "isp");
    dphy_type = rk3566_qom_child_object_type(qts, "/machine", "csi-dphy");
    rga_type = rk3566_qom_child_object_type(qts, "/machine", "rga2");
    venc_type = rk3566_qom_child_object_type(qts, "/machine", "venc");

    g_assert_cmpstr(isp_type, ==, "rk3566-isp");
    g_assert_cmpstr(dphy_type, ==, "rk3566-csi-dphy");
    g_assert_cmpstr(rga_type, ==, "rk3566-rga2");
    g_assert_cmpstr(venc_type, ==, "rk3566-rkvenc");

    qtest_quit(qts);
}

static void test_rk3566_iommu_access_width(void)
{
    QTestState *qts = rk3566_qtest_start();
    uint64_t base = RK3566_ISP_IOMMU_BASE;
    uint32_t status;

    if (!rk3566_qtest_media_present(qts)) {
        g_test_skip("RK3566 media devices are not built "
                    "(libavcodec, libavutil, libswscale or x264 missing)");
        qtest_quit(qts);
        return;
    }

    status = qtest_readl(qts, base + RK3566_IOMMU_STATUS);
    g_assert_cmphex(status, ==, RK3566_IOMMU_STATUS_RESET);
    g_assert_cmphex(qtest_readl(qts, base + RK3566_IOMMU_AUTO_GATING), ==, 1);

    /*
     * The register ops declare a 4 byte implementation and a valid range of
     * 1 to 8 bytes, so narrower reads are extracted from a 32 bit access.
     */
    g_assert_cmphex(qtest_readb(qts, base + RK3566_IOMMU_STATUS), ==,
                    status & 0xff);
    g_assert_cmphex(qtest_readw(qts, base + RK3566_IOMMU_STATUS), ==,
                    status & 0xffff);

    /* An 8 byte read combines two adjacent 32 bit registers. */
    g_assert_cmphex(qtest_readq(qts, base + RK3566_IOMMU_DTE_ADDR), ==,
                    (uint64_t)status << 32);

    /* Sub-word writes are widened to a full 32 bit register access. */
    qtest_writeb(qts, base + RK3566_IOMMU_AUTO_GATING, 0);
    g_assert_cmphex(qtest_readl(qts, base + RK3566_IOMMU_AUTO_GATING), ==, 0);
    qtest_writew(qts, base + RK3566_IOMMU_AUTO_GATING, 1);
    g_assert_cmphex(qtest_readl(qts, base + RK3566_IOMMU_AUTO_GATING), ==, 1);

    /*
     * Unaligned accesses are accepted by the .valid description; the 4 byte
     * implementation forwards the unaligned address to the register file,
     * which reports it as an unimplemented register and leaves the state
     * untouched.
     */
    g_assert_cmphex(qtest_readl(qts, base + RK3566_IOMMU_STATUS + 2), ==, 0);
    g_assert_cmphex(qtest_readw(qts, base + RK3566_IOMMU_STATUS + 1), ==, 0);
    qtest_writel(qts, base + RK3566_IOMMU_STATUS + 2, 0xffffffff);
    g_assert_cmphex(qtest_readl(qts, base + RK3566_IOMMU_STATUS), ==, status);

    /* An 8 byte write is split into two 32 bit register writes. */
    qtest_writeq(qts, base + RK3566_IOMMU_DTE_ADDR, 0x1122334455667788ULL);
    g_assert_cmphex(qtest_readl(qts, base + RK3566_IOMMU_DTE_ADDR), ==,
                    0x55667788);
    g_assert_cmphex(qtest_readl(qts, base + RK3566_IOMMU_STATUS), ==, status);

    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    if (!qtest_has_machine(RK3566_MACHINE_NAME)) {
        g_test_skip(RK3566_MACHINE_NAME " machine is not available");
        return 0;
    }

    qtest_add_func("/rk3566/machine-creation",
                   test_rk3566_machine_creation);
    qtest_add_func("/rk3566/mmio-reset-values",
                   test_rk3566_mmio_reset_values);
    qtest_add_func("/rk3566/media-devices",
                   test_rk3566_media_devices);
    qtest_add_func("/rk3566/iommu-access-width",
                   test_rk3566_iommu_access_width);

    return g_test_run();
}
