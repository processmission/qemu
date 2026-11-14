/*
 * Rockchip RK3588 SoC
 *
 * Copyright (C) 2026 Process Mission
 *
 * Authors:
 *    Chao Liu <chao.liu@processmission.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/rk3588.h"
#include "hw/adc/rk3588_saradc.h"
#include "hw/arm/bsa.h"
#include "hw/arm/linux-boot-if.h"
#include "hw/core/or-irq.h"
#include "hw/core/qdev-properties.h"
#include "net/net.h"
#include "hw/core/sysbus.h"
#include "hw/cpu/cluster.h"
#include "hw/misc/unimp.h"
#include "hw/core/register.h"
#include "hw/gpio/rockchip_gpio.h"
#include "hw/i2c/rk3x_i2c.h"
#include "hw/misc/rockchip_crypto_v2.h"
#include "hw/misc/rk3588_ddr.h"
#include "hw/misc/rk3588_grf.h"
#include "hw/misc/rk3588_pl330.h"
#include "hw/misc/rk3588_pwm.h"
#include "hw/misc/rk3588_rng.h"
#include "hw/misc/rk3588_rknpu.h"
#include "hw/misc/rk3588_scmi.h"
#include "hw/misc/rk3588_tsadc.h"
#include "hw/misc/rockchip_iommu.h"
#include "hw/misc/rockchip_syscon.h"
#include "hw/net/dwmac4.h"
#include "hw/nvram/rk3588_secure_otp.h"
#include "hw/pci-host/rockchip_pcie.h"
#include "hw/sd/dw_mmc.h"
#include "hw/sd/rockchip_dwcmshc.h"
#include "hw/sd/sdhci.h"
#include "hw/ssi/rockchip_spi.h"
#include "hw/timer/rockchip_stimer.h"
#include "hw/usb/dwc3-udc.h"
#include "hw/usb/rk3588_usb2_host.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "qobject/qlist.h"
#include "target/arm/gtimer.h"

/* MPIDR Aff0 values per Aff1: 1 (one CPU per RK3588 cluster). */
#define RK3588_MPIDR_AFF0_PER_AFF1   (1)

#define RK3588_CNTFRQ                24000000
#define RK3588_SDMMC_SPI             203
#define RK3588_SDHCI_SPI             205
#define RK3588_SFC_SPI               206
#define RK3588_USB3OTG0_SPI          220
#define RK3588_USB2_HOST0_EHCI_BASE  0xfc800000ULL
#define RK3588_USB2_HOST0_OHCI_BASE  0xfc840000ULL
#define RK3588_USB2_HOST1_EHCI_BASE  0xfc880000ULL
#define RK3588_USB2_HOST1_OHCI_BASE  0xfc8c0000ULL
#define RK3588_GIC_NUM_IRQS          512

#define RK3588_GMAC0_BASE            0xfe1b0000
#define RK3588_GMAC1_BASE            0xfe1c0000
#define RK3588_GMAC0_SPI             227
#define RK3588_GMAC1_SPI             234
#define RK3588_SECURE_OTP_BASE       0xfe3a0000
#define RK3588_CRYPTO_BASE           0xfe370000
#define RK3588_TSADC_BASE            0xfec00000
#define RK3588_TSADC_SPI             397
#define RK3588_SARADC_BASE           0xfec10000
#define RK3588_SARADC_SPI            398
#define RK3588_SPI0_SPI              326
#define RK3588_RNG_BASE              0xfe378000
#define RK3588_RNG_SPI               400
#define RK3588_DMAC1_BASE            0xfea10000
#define RK3588_DMAC1_SPI             86
#define RK3588_STIMER_BASE           0xfd8c8000
#define RK3588_GPIO0_SPI             277
#define RK3588_I2C6_BASE             0xfec80000
#define RK3588_I2C6_SPI              323
#define RK3588_DDR_LEGACY_BASE       0xf7000000ULL
#define RK3588_DDR_LEGACY_PHY_AUX_BASE 0xfe000000ULL
#define RK3588_DDRPHY_BASE           0xfd8d8000ULL
#define RK3588_DDR_PHY_GATE_BASE     0xfe0c0000ULL

#define RK3588_UART0_SPI             331
#define RK3588_UART_CLOCK            24000000
#define RK3588_UART_BAUD_BASE        (RK3588_UART_CLOCK / 16)

/* Memory map of the modeled peripherals. */
static const MemMapEntry rk3588_memmap[] = {
    [RK3588_DEV_GIC_DIST]   = { 0xfe600000, 0x00010000 },
    [RK3588_DEV_GIC_REDIST] = { 0xfe680000, 0x00100000 },
    [RK3588_DEV_GIC_ITS0]   = { 0xfe640000, 0x00020000 },
    [RK3588_DEV_GIC_ITS1]   = { 0xfe660000, 0x00020000 },
    [RK3588_DEV_CRU]        = { 0xfd7c0000, 0x0005c000 },
    [RK3588_DEV_PWM0]       = { 0xfd8b0000, 0x00000100 },
    [RK3588_DEV_PWM1]       = { 0xfebd0000, 0x00000100 },
    [RK3588_DEV_PWM2]       = { 0xfebe0000, 0x00000100 },
    [RK3588_DEV_PWM3]       = { 0xfebf0000, 0x00000100 },
    [RK3588_DEV_PMU0_GRF]   = { 0xfd588000, RK3588_GRF_MMIO_SIZE },
    [RK3588_DEV_PMU1_GRF]   = { 0xfd58a000, RK3588_GRF_MMIO_SIZE },
    [RK3588_DEV_SYS_GRF]    = { 0xfd58c000, RK3588_GRF_MMIO_SIZE },
    [RK3588_DEV_OTP]        = { 0xfecc0000, 0x00000400 },
    [RK3588_DEV_SFC]        = { 0xfe2b0000, 0x00004000 },
    [RK3588_DEV_SDMMC]      = { 0xfe2c0000, 0x00004000 },
    [RK3588_DEV_SDHCI]      = { 0xfe2e0000, 0x00010000 },
    [RK3588_DEV_USB3OTG0]   = { 0xfc000000, 0x00400000 },
    [RK3588_DEV_UART0]      = { 0xfd890000, 0x00010000 },
    [RK3588_DEV_UART1]      = { 0xfeb40000, 0x00010000 },
    [RK3588_DEV_UART2]      = { 0xfeb50000, 0x00010000 },
    [RK3588_DEV_UART3]      = { 0xfeb60000, 0x00010000 },
    [RK3588_DEV_UART4]      = { 0xfeb70000, 0x00010000 },
    [RK3588_DEV_UART5]      = { 0xfeb80000, 0x00010000 },
    [RK3588_DEV_UART6]      = { 0xfeb90000, 0x00010000 },
    [RK3588_DEV_UART7]      = { 0xfeba0000, 0x00010000 },
    [RK3588_DEV_UART8]      = { 0xfebb0000, 0x00010000 },
    [RK3588_DEV_UART9]      = { 0xfebc0000, 0x00010000 },
    [RK3588_DEV_SPI0]       = { 0xfeb00000, 0x00010000 },
    [RK3588_DEV_SPI1]       = { 0xfeb10000, 0x00010000 },
    [RK3588_DEV_SPI2]       = { 0xfeb20000, 0x00010000 },
    [RK3588_DEV_SPI3]       = { 0xfeb30000, 0x00010000 },
};

/* Object names of the SPI controllers, indexed like the state array. */
static const char *const rk3588_spi_names[RK3588_NUM_SPIS] = {
    "spi0", "spi1", "spi2", "spi3",
};

static const char *const rk3588_pwm_names[RK3588_NUM_PWMS] = {
    "pwm0", "pwm1", "pwm2", "pwm3",
};

#define RK3588_UNIMPLEMENTED(name, base, size) \
    { "unimpl-" name, "rk3588." name, base, size, -1000 }

/*
 * Peripheral windows from RK3588 TRM v1.0, Table 1-1.
 * Implemented devices take priority over these fallback regions.
 */
static const UnimplementedDeviceConfig rk3588_unimpl_devices[] = {
    RK3588_UNIMPLEMENTED("pcie3x4-config", 0xf0000000, 16 * MiB),
    RK3588_UNIMPLEMENTED("pcie3x2-config", 0xf1000000, 16 * MiB),
    RK3588_UNIMPLEMENTED("pcie2x1l0-config", 0xf2000000, 16 * MiB),
    RK3588_UNIMPLEMENTED("pcie2x1l1-config", 0xf3000000, 16 * MiB),
    RK3588_UNIMPLEMENTED("pcie3x4-dbi-low", 0xf5000000, 4 * MiB),
    RK3588_UNIMPLEMENTED("pcie3x2-dbi-low", 0xf5400000, 4 * MiB),
    RK3588_UNIMPLEMENTED("pcie2x1l0-dbi-low", 0xf5800000, 4 * MiB),
    RK3588_UNIMPLEMENTED("pcie2x1l1-dbi-low", 0xf5c00000, 4 * MiB),
    RK3588_UNIMPLEMENTED("ddrctl0", 0xf7000000, 16 * MiB),
    RK3588_UNIMPLEMENTED("ddrctl1", 0xf8000000, 16 * MiB),
    RK3588_UNIMPLEMENTED("ddrctl2", 0xf9000000, 16 * MiB),
    RK3588_UNIMPLEMENTED("ddrctl3", 0xfa000000, 16 * MiB),
    RK3588_UNIMPLEMENTED("gpu-g610", 0xfb000000, 16 * MiB),
    RK3588_UNIMPLEMENTED("usb3-1", 0xfc400000, 4 * MiB),
    RK3588_UNIMPLEMENTED("mmu600-pcie", 0xfc900000, 2 * MiB),
    RK3588_UNIMPLEMENTED("mmu600-php", 0xfcb00000, 2 * MiB),
    RK3588_UNIMPLEMENTED("usb3-2", 0xfcd00000, 4 * MiB),
    RK3588_UNIMPLEMENTED("daplite2", 0xfd100000, 512 * KiB),
    RK3588_UNIMPLEMENTED("pmu1-sgrf", 0xfd582000, 8 * KiB),
    RK3588_UNIMPLEMENTED("bus-sgrf", 0xfd586000, 4 * KiB),
    RK3588_UNIMPLEMENTED("dsu-sgrf", 0xfd587000, 4 * KiB),
    RK3588_UNIMPLEMENTED("pmu0-grf", 0xfd588000, 8 * KiB),
    RK3588_UNIMPLEMENTED("pmu1-grf", 0xfd58a000, 8 * KiB),
    RK3588_UNIMPLEMENTED("sys-grf", 0xfd58c000, 16 * KiB),
    RK3588_UNIMPLEMENTED("bigcore0-grf", 0xfd590000, 8 * KiB),
    RK3588_UNIMPLEMENTED("bigcore1-grf", 0xfd592000, 8 * KiB),
    RK3588_UNIMPLEMENTED("litcore-grf", 0xfd594000, 16 * KiB),
    RK3588_UNIMPLEMENTED("dsu-grf", 0xfd598000, 16 * KiB),
    RK3588_UNIMPLEMENTED("ddr01-grf", 0xfd59c000, 4 * KiB),
    RK3588_UNIMPLEMENTED("ddr23-grf", 0xfd59d000, 4 * KiB),
    RK3588_UNIMPLEMENTED("center-grf", 0xfd59e000, 8 * KiB),
    RK3588_UNIMPLEMENTED("gpu-grf", 0xfd5a0000, 8 * KiB),
    RK3588_UNIMPLEMENTED("npu-grf", 0xfd5a2000, 8 * KiB),
    RK3588_UNIMPLEMENTED("vop-grf", 0xfd5a4000, 8 * KiB),
    RK3588_UNIMPLEMENTED("vo0-grf", 0xfd5a6000, 8 * KiB),
    RK3588_UNIMPLEMENTED("vo1-grf", 0xfd5a8000, 16 * KiB),
    RK3588_UNIMPLEMENTED("usb-grf", 0xfd5ac000, 16 * KiB),
    RK3588_UNIMPLEMENTED("php-grf", 0xfd5b0000, 16 * KiB),
    RK3588_UNIMPLEMENTED("csidphy0-grf", 0xfd5b4000, 4 * KiB),
    RK3588_UNIMPLEMENTED("csidphy1-grf", 0xfd5b5000, 4 * KiB),
    RK3588_UNIMPLEMENTED("pcie3phy-grf", 0xfd5b8000, 16 * KiB),
    RK3588_UNIMPLEMENTED("pipe-phy0-grf", 0xfd5bc000, 16 * KiB),
    RK3588_UNIMPLEMENTED("pipe-phy1-grf", 0xfd5c0000, 16 * KiB),
    RK3588_UNIMPLEMENTED("pipe-phy2-grf", 0xfd5c4000, 16 * KiB),
    RK3588_UNIMPLEMENTED("usbdpphy0-grf", 0xfd5c8000, 16 * KiB),
    RK3588_UNIMPLEMENTED("usbdpphy1-grf", 0xfd5cc000, 16 * KiB),
    RK3588_UNIMPLEMENTED("usb2phy0-grf", 0xfd5d0000, 16 * KiB),
    RK3588_UNIMPLEMENTED("usb2phy1-grf", 0xfd5d4000, 16 * KiB),
    RK3588_UNIMPLEMENTED("usb2phy2-grf", 0xfd5d8000, 16 * KiB),
    RK3588_UNIMPLEMENTED("usb2phy3-grf", 0xfd5dc000, 16 * KiB),
    RK3588_UNIMPLEMENTED("hdptxphy0-grf", 0xfd5e0000, 16 * KiB),
    RK3588_UNIMPLEMENTED("hdptxphy1-grf", 0xfd5e4000, 16 * KiB),
    RK3588_UNIMPLEMENTED("mipidcphy0-grf", 0xfd5e8000, 16 * KiB),
    RK3588_UNIMPLEMENTED("mipidcphy1-grf", 0xfd5ec000, 16 * KiB),
    RK3588_UNIMPLEMENTED("pmu1-ioc", 0xfd5f0000, 16 * KiB),
    RK3588_UNIMPLEMENTED("pmu2-ioc", 0xfd5f4000, 16 * KiB),
    RK3588_UNIMPLEMENTED("vccio1-4-ioc", 0xfd5f9000, 4 * KiB),
    RK3588_UNIMPLEMENTED("vccio3-5-ioc", 0xfd5fa000, 4 * KiB),
    RK3588_UNIMPLEMENTED("vccio2-ioc", 0xfd5fb000, 4 * KiB),
    RK3588_UNIMPLEMENTED("vccio6-ioc", 0xfd5fc000, 4 * KiB),
    RK3588_UNIMPLEMENTED("emmc-ioc", 0xfd5fd000, 4 * KiB),
    RK3588_UNIMPLEMENTED("i2c0", 0xfd880000, 64 * KiB),
    RK3588_UNIMPLEMENTED("uart0", 0xfd890000, 64 * KiB),
    RK3588_UNIMPLEMENTED("pvtm-pmu", 0xfd8c0000, 32 * KiB),
    RK3588_UNIMPLEMENTED("hptimer", 0xfd8c8000, 16 * KiB),
    RK3588_UNIMPLEMENTED("pmu", 0xfd8d0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("wdt-pmu", 0xfd8e0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("timer-pmu", 0xfd8f0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("osc-chk", 0xfd9b0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("pvtm-core-b0", 0xfda40000, 32 * KiB),
    RK3588_UNIMPLEMENTED("pvtm-core-b1", 0xfda50000, 32 * KiB),
    RK3588_UNIMPLEMENTED("pvtm-core-l", 0xfda60000, 64 * KiB),
    RK3588_UNIMPLEMENTED("rknn-c0", 0xfdab0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("rknn-c1", 0xfdac0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("rknn-c2", 0xfdad0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("pvtm-npu", 0xfdaf0000, 32 * KiB),
    RK3588_UNIMPLEMENTED("wdt-npu", 0xfdaf8000, 32 * KiB),
    RK3588_UNIMPLEMENTED("timer-npu", 0xfdb00000, 32 * KiB),
    RK3588_UNIMPLEMENTED("pvtm-gpu", 0xfdb30000, 64 * KiB),
    RK3588_UNIMPLEMENTED("vdpu", 0xfdb50000, 64 * KiB),
    RK3588_UNIMPLEMENTED("rga3-0", 0xfdb60000, 64 * KiB),
    RK3588_UNIMPLEMENTED("rga3-1", 0xfdb70000, 64 * KiB),
    RK3588_UNIMPLEMENTED("rga2", 0xfdb80000, 64 * KiB),
    RK3588_UNIMPLEMENTED("jpeg-dec", 0xfdb90000, 64 * KiB),
    RK3588_UNIMPLEMENTED("jpeg-enc0", 0xfdba0000, 16 * KiB),
    RK3588_UNIMPLEMENTED("jpeg-enc1", 0xfdba4000, 16 * KiB),
    RK3588_UNIMPLEMENTED("jpeg-enc2", 0xfdba8000, 16 * KiB),
    RK3588_UNIMPLEMENTED("jpeg-enc3", 0xfdbac000, 16 * KiB),
    RK3588_UNIMPLEMENTED("iep", 0xfdbb0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("rkvenc0", 0xfdbd0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("rkvenc1", 0xfdbe0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("rkvdec-ccu", 0xfdc30000, 32 * KiB),
    RK3588_UNIMPLEMENTED("rkvdec0", 0xfdc38000, 32 * KiB),
    RK3588_UNIMPLEMENTED("rkvdec1", 0xfdc40000, 64 * KiB),
    RK3588_UNIMPLEMENTED("av1", 0xfdc70000, 256 * KiB),
    RK3588_UNIMPLEMENTED("isp0", 0xfdcb0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("isp1", 0xfdcc0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("fisheye0", 0xfdcd0000, 32 * KiB),
    RK3588_UNIMPLEMENTED("fisheye1", 0xfdcd8000, 32 * KiB),
    RK3588_UNIMPLEMENTED("vicap", 0xfdce0000, 128 * KiB),
    RK3588_UNIMPLEMENTED("csi-host0", 0xfdd10000, 64 * KiB),
    RK3588_UNIMPLEMENTED("csi-host1", 0xfdd20000, 64 * KiB),
    RK3588_UNIMPLEMENTED("csi-host2", 0xfdd30000, 64 * KiB),
    RK3588_UNIMPLEMENTED("csi-host3", 0xfdd40000, 64 * KiB),
    RK3588_UNIMPLEMENTED("csi-host4", 0xfdd50000, 64 * KiB),
    RK3588_UNIMPLEMENTED("csi-host5", 0xfdd60000, 64 * KiB),
    RK3588_UNIMPLEMENTED("vop", 0xfdd90000, 64 * KiB),
    RK3588_UNIMPLEMENTED("hdcp0-mmu", 0xfdda0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("spdif-tx2", 0xfddb0000, 32 * KiB),
    RK3588_UNIMPLEMENTED("spdif-tx5", 0xfddb8000, 32 * KiB),
    RK3588_UNIMPLEMENTED("i2s4", 0xfddc0000, 32 * KiB),
    RK3588_UNIMPLEMENTED("i2s8", 0xfddc8000, 32 * KiB),
    RK3588_UNIMPLEMENTED("hdcp1-mmu", 0xfddd0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("spdif-tx3", 0xfdde0000, 32 * KiB),
    RK3588_UNIMPLEMENTED("spdif-tx4", 0xfdde8000, 32 * KiB),
    RK3588_UNIMPLEMENTED("i2s5", 0xfddf0000, 16 * KiB),
    RK3588_UNIMPLEMENTED("i2s6", 0xfddf4000, 16 * KiB),
    RK3588_UNIMPLEMENTED("i2s7", 0xfddf8000, 16 * KiB),
    RK3588_UNIMPLEMENTED("i2s9", 0xfddfc000, 16 * KiB),
    RK3588_UNIMPLEMENTED("i2s10", 0xfde00000, 16 * KiB),
    RK3588_UNIMPLEMENTED("spdif-rx0", 0xfde08000, 32 * KiB),
    RK3588_UNIMPLEMENTED("spdif-rx1", 0xfde10000, 32 * KiB),
    RK3588_UNIMPLEMENTED("spdif-rx2", 0xfde18000, 32 * KiB),
    RK3588_UNIMPLEMENTED("dsi-host0", 0xfde20000, 64 * KiB),
    RK3588_UNIMPLEMENTED("dsi-host1", 0xfde30000, 64 * KiB),
    RK3588_UNIMPLEMENTED("hdcp0", 0xfde40000, 32 * KiB),
    RK3588_UNIMPLEMENTED("hdcp0-trng", 0xfde48000, 32 * KiB),
    RK3588_UNIMPLEMENTED("dp0", 0xfde50000, 64 * KiB),
    RK3588_UNIMPLEMENTED("dp1", 0xfde60000, 64 * KiB),
    RK3588_UNIMPLEMENTED("hdcp1", 0xfde70000, 32 * KiB),
    RK3588_UNIMPLEMENTED("hdcp1-trng", 0xfde78000, 32 * KiB),
    RK3588_UNIMPLEMENTED("hdmi-tx0", 0xfde80000, 128 * KiB),
    RK3588_UNIMPLEMENTED("hdmi-tx1", 0xfdea0000, 128 * KiB),
    RK3588_UNIMPLEMENTED("edp0", 0xfdec0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("edp1", 0xfded0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("hdmi-rx", 0xfdee0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("interconnect", 0xfdf30000, 896 * KiB),
    RK3588_UNIMPLEMENTED("firewall-dsuddr", 0xfe010000, 32 * KiB),
    RK3588_UNIMPLEMENTED("ahb2apb", 0xfe128000, 32 * KiB),
    RK3588_UNIMPLEMENTED("pcie3x4-apb", 0xfe150000, 64 * KiB),
    RK3588_UNIMPLEMENTED("pcie3x2-apb", 0xfe160000, 64 * KiB),
    RK3588_UNIMPLEMENTED("pcie2x1l0-apb", 0xfe170000, 64 * KiB),
    RK3588_UNIMPLEMENTED("pcie2x1l1-apb", 0xfe180000, 64 * KiB),
    RK3588_UNIMPLEMENTED("gmac0", 0xfe1b0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("gmac1", 0xfe1c0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("sata0", 0xfe210000, 64 * KiB),
    RK3588_UNIMPLEMENTED("sata1", 0xfe220000, 64 * KiB),
    RK3588_UNIMPLEMENTED("sata2", 0xfe230000, 64 * KiB),
    RK3588_UNIMPLEMENTED("sdio", 0xfe2d0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("crypto-ns", 0xfe370000, 32 * KiB),
    RK3588_UNIMPLEMENTED("trng-ns", 0xfe378000, 32 * KiB),
    RK3588_UNIMPLEMENTED("i2s0", 0xfe470000, 64 * KiB),
    RK3588_UNIMPLEMENTED("i2s1", 0xfe480000, 64 * KiB),
    RK3588_UNIMPLEMENTED("i2s2", 0xfe490000, 64 * KiB),
    RK3588_UNIMPLEMENTED("i2s3", 0xfe4a0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("pdm0", 0xfe4b0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("pdm1", 0xfe4c0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("vad", 0xfe4d0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("spdif-tx0", 0xfe4e0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("spdif-tx1", 0xfe4f0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("acdcdig-dsm", 0xfe500000, 64 * KiB),
    RK3588_UNIMPLEMENTED("spinlock", 0xfe5a0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("dmac0", 0xfea10000, 64 * KiB),
    RK3588_UNIMPLEMENTED("dmac1", 0xfea30000, 64 * KiB),
    RK3588_UNIMPLEMENTED("can0", 0xfea50000, 64 * KiB),
    RK3588_UNIMPLEMENTED("can1", 0xfea60000, 64 * KiB),
    RK3588_UNIMPLEMENTED("can2", 0xfea70000, 64 * KiB),
    RK3588_UNIMPLEMENTED("decompress", 0xfea80000, 64 * KiB),
    RK3588_UNIMPLEMENTED("i2c1", 0xfea90000, 64 * KiB),
    RK3588_UNIMPLEMENTED("i2c2", 0xfeaa0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("i2c3", 0xfeab0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("i2c4", 0xfeac0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("i2c5", 0xfead0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("timer-ns0", 0xfeae0000, 32 * KiB),
    RK3588_UNIMPLEMENTED("timer-ns1", 0xfeae8000, 32 * KiB),
    RK3588_UNIMPLEMENTED("wdt-ns", 0xfeaf0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("spi2", 0xfeb20000, 64 * KiB),
    RK3588_UNIMPLEMENTED("tsadc", 0xfec00000, 64 * KiB),
    RK3588_UNIMPLEMENTED("mailbox0", 0xfec60000, 64 * KiB),
    RK3588_UNIMPLEMENTED("mailbox1", 0xfec70000, 64 * KiB),
    RK3588_UNIMPLEMENTED("i2c6", 0xfec80000, 64 * KiB),
    RK3588_UNIMPLEMENTED("i2c7", 0xfec90000, 64 * KiB),
    RK3588_UNIMPLEMENTED("i2c8", 0xfeca0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("spi4", 0xfecb0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("mailbox2", 0xfece0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("intmux-pmu", 0xfecf0000, 32 * KiB),
    RK3588_UNIMPLEMENTED("dmac2", 0xfed10000, 64 * KiB),
    RK3588_UNIMPLEMENTED("hdptx-phy0", 0xfed60000, 64 * KiB),
    RK3588_UNIMPLEMENTED("hdptx-phy1", 0xfed70000, 64 * KiB),
    RK3588_UNIMPLEMENTED("usbdp-phy0", 0xfed80000, 64 * KiB),
    RK3588_UNIMPLEMENTED("usbdp-phy1", 0xfed90000, 64 * KiB),
    RK3588_UNIMPLEMENTED("mipi-cd-phy0", 0xfeda0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("mipi-cd-phy1", 0xfedb0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("mipi-csi-dphy0", 0xfedc0000, 32 * KiB),
    RK3588_UNIMPLEMENTED("mipi-csi-dphy1", 0xfedc8000, 32 * KiB),
    RK3588_UNIMPLEMENTED("hdmi-rx-phy", 0xfede0000, 64 * KiB),
    RK3588_UNIMPLEMENTED("combo-pipe-phy0", 0xfee00000, 64 * KiB),
    RK3588_UNIMPLEMENTED("combo-pipe-phy1", 0xfee10000, 64 * KiB),
    RK3588_UNIMPLEMENTED("combo-pipe-phy2", 0xfee20000, 64 * KiB),
    RK3588_UNIMPLEMENTED("pcie3-phy", 0xfee80000, 512 * KiB),
    RK3588_UNIMPLEMENTED("pcie3x4-memory", 0x900000000ULL, 1 * GiB),
    RK3588_UNIMPLEMENTED("pcie3x2-memory", 0x940000000ULL, 1 * GiB),
    RK3588_UNIMPLEMENTED("pcie2x1l0-memory", 0x980000000ULL, 1 * GiB),
    RK3588_UNIMPLEMENTED("pcie2x1l1-memory", 0x9c0000000ULL, 1 * GiB),
    RK3588_UNIMPLEMENTED("pcie3x4-dbi-high", 0xa40000000ULL, 4 * MiB),
    RK3588_UNIMPLEMENTED("pcie3x2-dbi-high", 0xa40400000ULL, 4 * MiB),
    RK3588_UNIMPLEMENTED("pcie2x1l0-dbi-high", 0xa40800000ULL, 4 * MiB),
    RK3588_UNIMPLEMENTED("pcie2x1l1-dbi-high", 0xa40c00000ULL, 4 * MiB),
    { }
};

#undef RK3588_UNIMPLEMENTED

/* The RK3588 CPU map has one A55 quad cluster and two A76 dual clusters. */
static const struct {
    unsigned int num_cpus;
    const char *cpu_type;
} rk3588_cpu_clusters[] = {
    [RK3588_CLUSTER_A55] = {
        .num_cpus = RK3588_A55_CPUS,
        .cpu_type = ARM_CPU_TYPE_NAME("cortex-a55"),
    },
    [RK3588_CLUSTER_A76_0] = {
        .num_cpus = RK3588_A76_CPUS_PER_CLUSTER,
        .cpu_type = ARM_CPU_TYPE_NAME("cortex-a76"),
    },
    [RK3588_CLUSTER_A76_1] = {
        .num_cpus = RK3588_A76_CPUS_PER_CLUSTER,
        .cpu_type = ARM_CPU_TYPE_NAME("cortex-a76"),
    },
};

G_STATIC_ASSERT(ARRAY_SIZE(rk3588_cpu_clusters) == RK3588_CLUSTER_COUNT);

static bool rk3588_soc_create_gic(RK3588SoCState *s, Error **errp)
{
    DeviceState *gicdev = DEVICE(&s->gic);
    SysBusDevice *gicsbd = SYS_BUS_DEVICE(gicdev);
    QList *redist_region_count = qlist_new();

    unsigned int num_irqs = RK3588_GIC_NUM_IRQS;

    qdev_prop_set_uint32(gicdev, "num-cpu", s->num_cpus);
    /* RK3588 TRM, Table 11-1: 480 SPIs and security extensions. */
    qdev_prop_set_uint32(gicdev, "num-irq", num_irqs);
    qdev_prop_set_bit(gicdev, "has-security-extensions", true);
    qlist_append_int(redist_region_count, s->num_cpus);
    qdev_prop_set_array(gicdev, "redist-region-count", redist_region_count);
    object_property_set_link(OBJECT(gicdev), "sysmem",
                             OBJECT(s->memory), &error_abort);
    /* The RK3588 GICv3 exposes LPIs so that its ITS instances can realize. */
    qdev_prop_set_bit(gicdev, "has-lpi", true);

    if (!sysbus_realize(gicsbd, errp)) {
        return false;
    }

    memory_region_add_subregion(s->memory,
        rk3588_memmap[RK3588_DEV_GIC_DIST].base,
        sysbus_mmio_get_region(gicsbd, 0));
    memory_region_add_subregion(s->memory,
        rk3588_memmap[RK3588_DEV_GIC_REDIST].base,
        sysbus_mmio_get_region(gicsbd, 1));

    for (unsigned int i = 0; i < s->num_cpus; i++) {
        DeviceState *cpudev = DEVICE(&s->cpu[i]);
        int intidbase = num_irqs - GIC_INTERNAL + (i * GIC_INTERNAL);
        static const int timer_irqs[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP] = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC] = ARCH_TIMER_S_EL1_IRQ,
            [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ,
        };

        for (unsigned int j = 0; j < ARRAY_SIZE(timer_irqs); j++) {
            qdev_connect_gpio_out(cpudev, j,
                                  qdev_get_gpio_in(gicdev,
                                                   intidbase + timer_irqs[j]));
        }

        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(gicdev,
                                                     intidbase +
                                                     ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(gicdev,
                                                     intidbase +
                                                     VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicsbd, i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicsbd, i + s->num_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicsbd, i + 2 * s->num_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicsbd, i + 3 * s->num_cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }

    return true;
}

void rk3588_soc_prepare_nonsecure_interrupts(RK3588SoCState *s)
{
    ARMLinuxBootIf *albif = ARM_LINUX_BOOT_IF(&s->gic);
    ARMLinuxBootIfClass *albifc = ARM_LINUX_BOOT_IF_GET_CLASS(albif);

    albifc->arm_linux_init(albif, false);
}

static bool rk3588_soc_create_its(RK3588SoCState *s, Error **errp)
{
    static const int its_memmap[] = {
        RK3588_DEV_GIC_ITS0,
        RK3588_DEV_GIC_ITS1,
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(its_memmap); i++) {
        g_autofree char *name = g_strdup_printf("its%u", i);
        DeviceState *dev = qdev_new(its_class_name());
        SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        object_property_set_link(OBJECT(dev), "parent-gicv3",
                                 OBJECT(&s->gic), &error_abort);
        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        memory_region_add_subregion(s->memory,
            rk3588_memmap[its_memmap[i]].base,
            sysbus_mmio_get_region(sbd, 0));
        s->its[i] = dev;
        object_unref(OBJECT(dev));
    }

    return true;
}

static bool rk3588_soc_create_cru(RK3588SoCState *s, Error **errp)
{
    s->cru = qdev_new("rockchip-cru");
    object_property_add_child(OBJECT(s), "cru", OBJECT(s->cru));
    object_unref(OBJECT(s->cru));
    SysBusDevice *cru = SYS_BUS_DEVICE(s->cru);

    if (!sysbus_realize(cru, errp)) {
        return false;
    }

    memory_region_add_subregion(s->memory,
        rk3588_memmap[RK3588_DEV_CRU].base,
        sysbus_mmio_get_region(cru, 0));

    return true;
}

static bool rk3588_soc_create_grf(RK3588SoCState *s, Error **errp)
{
    static const unsigned int grf_memmap[RK3588_GRF_MMIO_COUNT] = {
        [RK3588_GRF_MMIO_PMU0] = RK3588_DEV_PMU0_GRF,
        [RK3588_GRF_MMIO_PMU1] = RK3588_DEV_PMU1_GRF,
        [RK3588_GRF_MMIO_SYS] = RK3588_DEV_SYS_GRF,
    };
    DeviceState *dev = qdev_new(TYPE_RK3588_GRF);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), "grf", OBJECT(dev));
    object_unref(OBJECT(dev));
    qdev_prop_set_uint64(dev, "ram-size", s->ram_size);
    qdev_prop_set_uint32(dev, "dram-type", s->dram_type);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(grf_memmap); i++) {
        memory_region_add_subregion(s->memory,
            rk3588_memmap[grf_memmap[i]].base,
            sysbus_mmio_get_region(sbd, i));
    }

    return true;
}

static bool rk3588_soc_create_syscons(RK3588SoCState *s, Error **errp)
{
    static const struct {
        const char *name;
        hwaddr base;
        uint32_t size;
    } syscons[] = {
        { "php-grf", 0xfd5b0000, 4 * KiB },
        { "usb-grf", 0xfd5ac000, 4 * KiB },
        { "pmu1-ioc", 0xfd5f0000, 4 * KiB },
        { "pmu2-ioc", 0xfd5f4000, 4 * KiB },
        { "bus-ioc", 0xfd5f8000, 4 * KiB },
        { "firewall-ddr", 0xfe030000, 4 * KiB },
        { "firewall-sysmem", 0xfe038000, 4 * KiB },
        { "combphy0", 0xfee00000, 0x100 },
        { "combphy1", 0xfee10000, 0x100 },
        { "combphy2", 0xfee20000, 0x100 },
        { "pcie30phy", 0xfee80000, 128 * KiB },
        { "pipe-phy-grf0", 0xfd5bc000, 4 * KiB },
        { "pipe-phy-grf1", 0xfd5c0000, 4 * KiB },
        { "pipe-phy-grf2", 0xfd5c4000, 4 * KiB },
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(syscons); i++) {
        DeviceState *dev = qdev_new(TYPE_ROCKCHIP_SYSCON);
        SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

        qdev_prop_set_uint32(dev, "size", syscons[i].size);
        object_property_add_child(OBJECT(s), syscons[i].name, OBJECT(dev));
        object_unref(OBJECT(dev));
        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        sysbus_mmio_map(sbd, 0, syscons[i].base);
    }

    return true;
}

static bool rk3588_soc_create_scmi(RK3588SoCState *s, Error **errp)
{
    SysBusDevice *sbd;

    s->scmi = qdev_new(TYPE_RK3588_SCMI);
    object_property_add_child(OBJECT(s), "scmi", OBJECT(s->scmi));
    object_unref(OBJECT(s->scmi));
    sbd = SYS_BUS_DEVICE(s->scmi);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    /* Board RAM takes precedence over the emulated firmware mailbox. */
    sysbus_mmio_map_overlap(sbd, 0, RK3588_SCMI_SHMEM_BASE, -1);
    return true;
}

bool rk3588_soc_handle_scmi_smc(RK3588SoCState *s, ARMCPU *cpu)
{
    CPUARMState *env = &cpu->env;
    uint64_t fn = is_a64(env) ? env->xregs[0] : env->regs[0];

    if ((uint32_t)fn != RK3588_SCMI_SMC_ID) {
        return false;
    }
    rk3588_scmi_handle_smc(RK3588_SCMI(s->scmi));
    if (is_a64(env)) {
        env->xregs[0] = 0;
    } else {
        env->regs[0] = 0;
    }
    return true;
}

static bool rk3588_soc_create_ddr(RK3588SoCState *s, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_DDR);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->ddr = dev;
    object_property_add_child(OBJECT(s), "ddr", OBJECT(dev));
    object_unref(OBJECT(dev));
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }

    for (unsigned int i = 0; i < RK3588_DDR_LEGACY_CHANNEL_COUNT; i++) {
        hwaddr base = RK3588_DDR_LEGACY_BASE +
                      i * RK3588_DDR_LEGACY_CHANNEL_STRIDE;

        for (unsigned int j = 0;
             j < RK3588_DDR_LEGACY_CTRL_WINDOWS_PER_CHANNEL; j++) {
            sysbus_mmio_map(sbd, RK3588_DDR_MMIO_LEGACY(i, j),
                            base + j * RK3588_DDR_LEGACY_WINDOW_STRIDE);
        }
        sysbus_mmio_map(sbd, RK3588_DDR_MMIO_LEGACY_GATE(i),
                        RK3588_DDR_PHY_GATE_BASE +
                        i * RK3588_DDR_LEGACY_WINDOW_STRIDE);
    }
    sysbus_mmio_map(sbd, RK3588_DDR_MMIO_LEGACY_PHY_AUX,
                    RK3588_DDR_LEGACY_PHY_AUX_BASE);
    sysbus_mmio_map(sbd, RK3588_DDR_MMIO_DDRPHY, RK3588_DDRPHY_BASE);

    return true;
}

static bool rk3588_soc_create_otp(RK3588SoCState *s, Error **errp)
{
    s->otp = qdev_new("rockchip-otp");
    object_property_add_child(OBJECT(s), "otp", OBJECT(s->otp));
    object_unref(OBJECT(s->otp));
    SysBusDevice *otp = SYS_BUS_DEVICE(s->otp);

    if (!sysbus_realize(otp, errp)) {
        return false;
    }

    memory_region_add_subregion(s->memory,
        rk3588_memmap[RK3588_DEV_OTP].base,
        sysbus_mmio_get_region(otp, 0));

    return true;
}

static bool rk3588_soc_create_sfc(RK3588SoCState *s, Error **errp)
{
    s->sfc = qdev_new("rockchip-sfc");
    object_property_add_child(OBJECT(s), "sfc", OBJECT(s->sfc));
    object_unref(OBJECT(s->sfc));
    SysBusDevice *sfc = SYS_BUS_DEVICE(s->sfc);

    object_property_set_link(OBJECT(s->sfc), "dma-memory",
                             OBJECT(s->memory), &error_abort);
    if (!sysbus_realize(sfc, errp)) {
        return false;
    }

    memory_region_add_subregion(s->memory,
        rk3588_memmap[RK3588_DEV_SFC].base,
        sysbus_mmio_get_region(sfc, 0));
    sysbus_connect_irq(sfc, 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RK3588_SFC_SPI));

    return true;
}

static bool rk3588_soc_create_sdmmc(RK3588SoCState *s, Error **errp)
{
    s->sdmmc = qdev_new(TYPE_DW_MMC);
    object_property_add_child(OBJECT(s), "sdmmc", OBJECT(s->sdmmc));
    object_unref(OBJECT(s->sdmmc));
    SysBusDevice *sbd = SYS_BUS_DEVICE(s->sdmmc);

    if (!sysbus_realize(sbd, errp)) {
        return false;
    }

    /*
     * mmio[0] is the core register bank through the legacy FIFO data port at
     * +0x200; mmio[1] is the RAZ/WI vendor and reserved tail.  Both are
     * contiguous sub-windows of the 0x4000 controller MMIO.
     */
    memory_region_add_subregion(s->memory,
        rk3588_memmap[RK3588_DEV_SDMMC].base,
        sysbus_mmio_get_region(sbd, 0));
    memory_region_add_subregion(s->memory,
        rk3588_memmap[RK3588_DEV_SDMMC].base + 0x204,
        sysbus_mmio_get_region(sbd, 1));
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RK3588_SDMMC_SPI));

    return true;
}

static bool rk3588_soc_create_sdhci(RK3588SoCState *s, Error **errp)
{
    SysBusDevice *sbd;
    DeviceState *vendor;
    SysBusDevice *vendor_sbd;

    s->sdhci = qdev_new(TYPE_SYSBUS_SDHCI);
    object_property_add_child(OBJECT(s), "sdhci", OBJECT(s->sdhci));
    object_unref(OBJECT(s->sdhci));
    object_property_set_uint(OBJECT(s->sdhci), "sd-spec-version", 3,
                             &error_abort);
    object_property_set_uint(OBJECT(s->sdhci), "capareg", 0x280737ec6481,
                             &error_abort);
    object_property_set_uint(OBJECT(s->sdhci), "uhs", UHS_I, &error_abort);
    object_property_set_uint(OBJECT(s->sdhci), "vendor-area1", 0x500,
                             &error_abort);
    object_property_set_uint(OBJECT(s->sdhci), "vendor-area2", 0x800,
                             &error_abort);
    sbd = SYS_BUS_DEVICE(s->sdhci);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    memory_region_add_subregion(s->memory,
        rk3588_memmap[RK3588_DEV_SDHCI].base,
        sysbus_mmio_get_region(sbd, 0));
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RK3588_SDHCI_SPI));

    vendor = qdev_new(TYPE_ROCKCHIP_DWCMSHC_VENDOR);
    vendor_sbd = SYS_BUS_DEVICE(vendor);
    object_property_add_child(OBJECT(s), "sdhci-vendor", OBJECT(vendor));
    object_unref(OBJECT(vendor));
    if (!sysbus_realize(vendor_sbd, errp)) {
        return false;
    }
    memory_region_add_subregion(s->memory,
        rk3588_memmap[RK3588_DEV_SDHCI].base +
        ROCKCHIP_DWCMSHC_VENDOR_BASE,
        sysbus_mmio_get_region(vendor_sbd, 0));

    return true;
}

static bool rk3588_soc_create_uart(RK3588SoCState *s, Error **errp)
{
    for (unsigned int i = 0; i < RK3588_NUM_UARTS; i++) {
        SysBusDevice *uart = SYS_BUS_DEVICE(&s->uart[i]);

        qdev_prop_set_uint32(DEVICE(uart), "baudbase", RK3588_UART_BAUD_BASE);
        if (!sysbus_realize(uart, errp)) {
            return false;
        }

        memory_region_add_subregion(s->memory,
            rk3588_memmap[RK3588_DEV_UART0 + i].base,
            sysbus_mmio_get_region(uart, 0));
        sysbus_connect_irq(uart, 0,
                           qdev_get_gpio_in(DEVICE(&s->gic),
                                            RK3588_UART0_SPI + i));
    }

    return true;
}

static bool rk3588_soc_create_usb(RK3588SoCState *s, Error **errp)
{
    SysBusDevice *usb = SYS_BUS_DEVICE(s->usb3otg0);

    if (!sysbus_realize(usb, errp)) {
        return false;
    }

    memory_region_add_subregion(s->memory,
        rk3588_memmap[RK3588_DEV_USB3OTG0].base,
        sysbus_mmio_get_region(usb, 0));
    sysbus_connect_irq(usb, 0,
                       qdev_get_gpio_in(DEVICE(&s->gic),
                                        RK3588_USB3OTG0_SPI));

    return true;
}

static bool rk3588_soc_create_usb2_host(RK3588SoCState *s, Error **errp)
{
    static const hwaddr bases[RK3588_USB2_HOST_MMIO_COUNT] = {
        [RK3588_USB2_HOST_EHCI0] = RK3588_USB2_HOST0_EHCI_BASE,
        [RK3588_USB2_HOST_OHCI0] = RK3588_USB2_HOST0_OHCI_BASE,
        [RK3588_USB2_HOST_EHCI1] = RK3588_USB2_HOST1_EHCI_BASE,
        [RK3588_USB2_HOST_OHCI1] = RK3588_USB2_HOST1_OHCI_BASE,
    };
    SysBusDevice *sbd;

    s->usb2_host = RK3588_USB2_HOST(qdev_new(TYPE_RK3588_USB2_HOST));
    object_property_add_child(OBJECT(s), "usb2-host",
                              OBJECT(s->usb2_host));
    object_unref(OBJECT(s->usb2_host));
    sbd = SYS_BUS_DEVICE(s->usb2_host);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(bases); i++) {
        memory_region_add_subregion(s->memory, bases[i],
                                    sysbus_mmio_get_region(sbd, i));
    }

    rk3588_usb2_host_set_active(s->usb2_host, true);

    return true;
}

static bool rk3588_soc_create_rknpu(RK3588SoCState *s, Error **errp)
{
    static const struct {
        hwaddr base;
        hwaddr mmu[2];
        int spi;
    } cores[] = {
        { 0xfdab0000, { 0xfdab9000, 0xfdaba000 }, 110 },
        { 0xfdac0000, { 0xfdaca000, 0 }, 111 },
        { 0xfdad0000, { 0xfdada000, 0 }, 112 },
    };
    /* Sysbus MMIO order: PC, CNA, CORE, DPU, GLOBAL, PPU, PPU_RDMA. */
    static const hwaddr offsets[] = {
        0, 0x1000, 0x3000, ROCKCHIP_RKNN_DPU_OFFSET,
        ROCKCHIP_RKNN_GLOBAL_OFFSET, ROCKCHIP_RKNN_PPU_OFFSET,
        ROCKCHIP_RKNN_PPU_RDMA_OFFSET,
    };
    DeviceState *mmu[ARRAY_SIZE(cores)];
    DeviceState *irq_or[ARRAY_SIZE(cores)];

    if (!s->rknpu) {
        return true;
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(cores); i++) {
        g_autofree char *name = g_strdup_printf("rknn-mmu%u", i);
        g_autofree char *irq_name = g_strdup_printf("rknn-irq-or%u", i);
        unsigned int num_mmu = cores[i].mmu[1] ? 2 : 1;
        SysBusDevice *sbd;

        mmu[i] = qdev_new(TYPE_ROCKCHIP_IOMMU);
        qdev_prop_set_uint32(mmu[i], "num-mmu", num_mmu);
        qdev_prop_set_uint32(mmu[i], "core-index", i);
        object_property_add_child(OBJECT(s), name, OBJECT(mmu[i]));
        object_unref(OBJECT(mmu[i]));
        sbd = SYS_BUS_DEVICE(mmu[i]);
        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        for (unsigned int j = 0; j < num_mmu; j++) {
            sysbus_mmio_map(sbd, j, cores[i].mmu[j]);
        }

        irq_or[i] = qdev_new(TYPE_OR_IRQ);
        qdev_prop_set_uint16(irq_or[i], "num-lines", 2);
        object_property_add_child(OBJECT(s), irq_name, OBJECT(irq_or[i]));
        object_unref(OBJECT(irq_or[i]));
        if (!qdev_realize(irq_or[i], NULL, errp)) {
            return false;
        }
        qdev_connect_gpio_out(irq_or[i], 0,
                              qdev_get_gpio_in(DEVICE(&s->gic), cores[i].spi));
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(irq_or[i], 0));
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(cores); i++) {
        g_autofree char *name = g_strdup_printf("rknn%u", i);
        MemoryRegion *dma_mr = rockchip_iommu_get_memory_region(
            ROCKCHIP_IOMMU(mmu[i]));
        DeviceState *dev = qdev_new(TYPE_RK3588_RKNPU);
        SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

        qdev_prop_set_uint32(dev, "core-index", i);
        object_property_set_link(OBJECT(dev), "dma", OBJECT(dma_mr),
                                 &error_abort);
        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        object_unref(OBJECT(dev));
        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        for (unsigned int j = 0; j < ARRAY_SIZE(offsets); j++) {
            sysbus_mmio_map(sbd, j, cores[i].base + offsets[j]);
        }
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(irq_or[i], 1));
        qdev_connect_gpio_out_named(s->cru, "rknpu-reset", i,
                                    qdev_get_gpio_in_named(dev, "reset", 0));
    }

    return true;
}

static bool rk3588_soc_create_pcie(RK3588SoCState *s, Error **errp)
{
    static const struct {
        const char *name;
        hwaddr dbi;
        hwaddr apb;
        int err_spi;
        int legacy_spi;
        int msg_spi;
        int pmc_spi;
        int sys_spi;
    } hosts[RK3588_PCIE_COUNT] = {
        [RK3588_PCIE3X4] = {
            "pcie3x4", 0xa40000000ULL, 0xfe150000,
            259, 260, 261, 262, 263,
        },
        [RK3588_PCIE3X2] = {
            "pcie3x2", 0xa40400000ULL, 0xfe160000,
            254, 255, 256, 257, 258,
        },
        [RK3588_PCIE2X1L0] = {
            "pcie2x1l0", 0xa40800000ULL, 0xfe170000,
            239, 240, 241, 242, 243,
        },
        [RK3588_PCIE2X1L2] = {
            "pcie2x1l2", 0xa41000000ULL, 0xfe190000,
            249, 250, 251, 252, 253,
        },
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(hosts); i++) {
        const RK3588PCIEConfig *config = &s->pcie_config[i];
        DeviceState *host;
        SysBusDevice *sbd;

        if (!config->enabled) {
            continue;
        }
        host = qdev_new(TYPE_ROCKCHIP_PCIE_HOST);
        s->pcie[i] = host;
        host->id = g_strdup(config->id);
        qdev_prop_set_uint32(host, "domain", config->domain);
        qdev_prop_set_uint8(host, "bus-nr", config->bus_nr);
        qdev_prop_set_bit(host, "link-up", config->link_up);
        object_property_add_child(OBJECT(s), hosts[i].name, OBJECT(host));
        object_unref(OBJECT(host));
        sbd = SYS_BUS_DEVICE(host);
        if (!sysbus_realize(sbd, errp)) {
            return false;
        }

        /* CFG and outbound memory windows are mapped by the guest's iATU. */
        sysbus_mmio_map(sbd, 0, hosts[i].dbi);
        sysbus_mmio_map(sbd, 1, hosts[i].apb);
        sysbus_mmio_map(sbd, 2, hosts[i].dbi + ROCKCHIP_PCIE_DBI_CORE_SIZE);

        /* INTA..INTD share the legacy SPI. */
        for (unsigned int j = 0; j < 4; j++) {
            sysbus_connect_irq(sbd, j,
                qdev_get_gpio_in(DEVICE(&s->gic), hosts[i].legacy_spi));
        }
        sysbus_connect_irq(sbd, ROCKCHIP_PCIE_MSG_IRQ,
            qdev_get_gpio_in(DEVICE(&s->gic), hosts[i].msg_spi));
        sysbus_connect_irq(sbd, ROCKCHIP_PCIE_ERR_IRQ,
            qdev_get_gpio_in(DEVICE(&s->gic), hosts[i].err_spi));
        sysbus_connect_irq(sbd, ROCKCHIP_PCIE_PMC_IRQ,
            qdev_get_gpio_in(DEVICE(&s->gic), hosts[i].pmc_spi));
        sysbus_connect_irq(sbd, ROCKCHIP_PCIE_SYS_IRQ,
            qdev_get_gpio_in(DEVICE(&s->gic), hosts[i].sys_spi));
    }

    return true;
}

static bool rk3588_soc_create_secure_otp(RK3588SoCState *s, Error **errp)
{
    SysBusDevice *sbd;

    if (!s->secure_otp_unfused) {
        return true;
    }

    s->secure_otp = qdev_new(TYPE_RK3588_SECURE_OTP);
    object_property_add_child(OBJECT(s), "secure-otp",
                              OBJECT(s->secure_otp));
    object_unref(OBJECT(s->secure_otp));
    sbd = SYS_BUS_DEVICE(s->secure_otp);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, RK3588_SECURE_OTP_BASE);
    return true;
}

static bool rk3588_soc_create_crypto(RK3588SoCState *s, Error **errp)
{
    SysBusDevice *sbd;

    if (!s->crypto_v2_sha256) {
        return true;
    }

    s->crypto = qdev_new(TYPE_ROCKCHIP_CRYPTO_V2);
    object_property_add_child(OBJECT(s), "crypto", OBJECT(s->crypto));
    object_unref(OBJECT(s->crypto));
    sbd = SYS_BUS_DEVICE(s->crypto);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, RK3588_CRYPTO_BASE);
    return true;
}

static bool rk3588_soc_create_tsadc(RK3588SoCState *s, Error **errp)
{
    SysBusDevice *sbd;

    s->tsadc = qdev_new(TYPE_RK3588_TSADC);
    object_property_add_child(OBJECT(s), "tsadc", OBJECT(s->tsadc));
    object_unref(OBJECT(s->tsadc));
    sbd = SYS_BUS_DEVICE(s->tsadc);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, RK3588_TSADC_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RK3588_TSADC_SPI));
    return true;
}

static bool rk3588_soc_create_saradc(RK3588SoCState *s, Error **errp)
{
    SysBusDevice *sbd;

    s->saradc = qdev_new(TYPE_RK3588_SARADC);
    object_property_add_child(OBJECT(s), "saradc", OBJECT(s->saradc));
    object_unref(OBJECT(s->saradc));
    sbd = SYS_BUS_DEVICE(s->saradc);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, RK3588_SARADC_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RK3588_SARADC_SPI));
    return true;
}

static bool rk3588_soc_create_spi_controller(RK3588SoCState *s,
                                            unsigned int dev, Error **errp)
{
    unsigned int idx = dev - RK3588_DEV_SPI0;
    SysBusDevice *sbd;

    assert(idx < RK3588_NUM_SPIS);

    s->spi[idx] = qdev_new(TYPE_ROCKCHIP_SPI);
    s->spi[idx]->id = g_strdup(rk3588_spi_names[idx]);
    object_property_add_child(OBJECT(s), rk3588_spi_names[idx],
                              OBJECT(s->spi[idx]));
    object_unref(OBJECT(s->spi[idx]));
    /* SPI2 peripherals use the board's existing chip-select wiring. */
    if (dev != RK3588_DEV_SPI2) {
        qdev_prop_set_bit(s->spi[idx], "mirror-cs", true);
    }
    sbd = SYS_BUS_DEVICE(s->spi[idx]);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, rk3588_memmap[dev].base);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(DEVICE(&s->gic),
                                        RK3588_SPI0_SPI + idx));
    return true;
}

static bool rk3588_soc_create_spi(RK3588SoCState *s, Error **errp)
{
    for (unsigned int i = 0; i < RK3588_NUM_SPIS; i++) {
        if (!rk3588_soc_create_spi_controller(s, RK3588_DEV_SPI0 + i, errp)) {
            return false;
        }
    }
    return true;
}

static bool rk3588_soc_create_rng(RK3588SoCState *s, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_RNG);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), "rng", OBJECT(dev));
    object_unref(OBJECT(dev));
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, RK3588_RNG_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RK3588_RNG_SPI));
    return true;
}

static bool rk3588_soc_create_dmac1(RK3588SoCState *s, Error **errp)
{
    SysBusDevice *sbd;

    s->dmac1 = qdev_new(TYPE_RK3588_PL330);
    object_property_add_child(OBJECT(s), "dmac1", OBJECT(s->dmac1));
    object_unref(OBJECT(s->dmac1));
    sbd = SYS_BUS_DEVICE(s->dmac1);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, RK3588_DMAC1_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RK3588_DMAC1_SPI));
    sysbus_connect_irq(sbd, 1,
                       qdev_get_gpio_in(DEVICE(&s->gic),
                                        RK3588_DMAC1_SPI + 1));
    return true;
}

static bool rk3588_soc_create_stimer(RK3588SoCState *s, Error **errp)
{
    DeviceState *dev = qdev_new(TYPE_ROCKCHIP_STIMER);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), "stimer", OBJECT(dev));
    object_unref(OBJECT(dev));
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, RK3588_STIMER_BASE);
    return true;
}

static bool rk3588_soc_create_gpio(RK3588SoCState *s, Error **errp)
{
    static const hwaddr gpio_bases[] = {
        0xfd8a0000, 0xfec20000, 0xfec30000, 0xfec40000, 0xfec50000,
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(gpio_bases); i++) {
        g_autofree char *name = g_strdup_printf("gpio%u", i);
        SysBusDevice *sbd;

        s->gpio[i] = qdev_new(TYPE_ROCKCHIP_GPIO);
        object_property_add_child(OBJECT(s), name, OBJECT(s->gpio[i]));
        object_unref(OBJECT(s->gpio[i]));
        sbd = SYS_BUS_DEVICE(s->gpio[i]);
        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        sysbus_mmio_map(sbd, 0, gpio_bases[i]);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(DEVICE(&s->gic),
                                            RK3588_GPIO0_SPI + i));
    }
    return true;
}

static bool rk3588_soc_create_pwm(RK3588SoCState *s, Error **errp)
{
    for (unsigned int i = 0; i < RK3588_NUM_PWMS; i++) {
        s->pwm[i] = qdev_new(TYPE_RK3588_PWM);
        object_property_add_child(OBJECT(s), rk3588_pwm_names[i],
                                  OBJECT(s->pwm[i]));
        if (!sysbus_realize(SYS_BUS_DEVICE(s->pwm[i]), errp)) {
            return false;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(s->pwm[i]), 0,
                        rk3588_memmap[RK3588_DEV_PWM0 + i].base);
        object_unref(OBJECT(s->pwm[i]));
    }
    return true;
}

static bool rk3588_soc_create_i2c6(RK3588SoCState *s, Error **errp)
{
    SysBusDevice *sbd;

    s->i2c6 = qdev_new(TYPE_RK3X_I2C);
    object_property_add_child(OBJECT(s), "i2c6", OBJECT(s->i2c6));
    object_unref(OBJECT(s->i2c6));
    sbd = SYS_BUS_DEVICE(s->i2c6);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, RK3588_I2C6_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(DEVICE(&s->gic), RK3588_I2C6_SPI));

    return true;
}

static bool rk3588_soc_create_gmac(RK3588SoCState *s, Error **errp)
{
    static const struct {
        hwaddr base;
        int spi;
        const char *name;
        bool match_default;
    } gmacs[] = {
        { RK3588_GMAC0_BASE, RK3588_GMAC0_SPI, "gmac0", false },
        { RK3588_GMAC1_BASE, RK3588_GMAC1_SPI, "gmac1", true },
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(gmacs); i++) {
        SysBusDevice *sbd;

        if (!(s->gmac_mask & BIT(i))) {
            continue;
        }
        s->gmac[i] = qdev_new(TYPE_DWMAC4);
        object_property_add_child(OBJECT(s), gmacs[i].name,
                                  OBJECT(s->gmac[i]));
        qemu_configure_nic_device(s->gmac[i], gmacs[i].match_default,
                                  gmacs[i].name);
        object_unref(OBJECT(s->gmac[i]));
        sbd = SYS_BUS_DEVICE(s->gmac[i]);
        if (!sysbus_realize(sbd, errp)) {
            return false;
        }
        sysbus_mmio_map(sbd, 0, gmacs[i].base);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(DEVICE(&s->gic), gmacs[i].spi));
    }
    return true;
}

static void rk3588_soc_realize(DeviceState *dev, Error **errp)
{
    RK3588SoCState *s = RK3588_SOC(dev);

    if (!s->memory) {
        error_setg(errp, "rk3588: memory link is required");
        return;
    }

    unsigned int n = 0;

    for (unsigned int c = 0;
         c < RK3588_CLUSTER_COUNT && n < s->num_cpus; c++) {
        g_autofree char *cluster_name = g_strdup_printf("cluster%u", c);

        object_initialize_child(OBJECT(s), cluster_name, &s->cluster[c],
                                TYPE_CPU_CLUSTER);
        qdev_prop_set_uint32(DEVICE(&s->cluster[c]), "cluster-id", c);

        for (unsigned int i = 0; i < rk3588_cpu_clusters[c].num_cpus &&
                                n < s->num_cpus; i++, n++) {
            g_autofree char *name = g_strdup_printf("cpu%u", n);

            object_initialize_child(OBJECT(&s->cluster[c]), name,
                                    &s->cpu[n],
                                    rk3588_cpu_clusters[c].cpu_type);
        }

        if (!qdev_realize(DEVICE(&s->cluster[c]), NULL, errp)) {
            return;
        }
    }

    for (n = 0; n < s->num_cpus; n++) {
        ARMCPU *cpu = &s->cpu[n];
        uint64_t mpidr = arm_build_mp_affinity(n, RK3588_MPIDR_AFF0_PER_AFF1);

        CPU(cpu)->cpu_index = n;
        object_property_set_int(OBJECT(cpu), "mp-affinity", mpidr,
                                &error_abort);
        object_property_set_int(OBJECT(cpu), "cntfrq", RK3588_CNTFRQ,
                                &error_abort);
        object_property_set_link(OBJECT(cpu), "memory",
                                 OBJECT(s->memory), &error_abort);
        if (n > 0) {
            object_property_set_bool(OBJECT(cpu), "start-powered-off", true,
                                     &error_abort);
        }

        if (!qdev_realize(DEVICE(cpu), NULL, errp)) {
            return;
        }
    }

    if (!rk3588_soc_create_gic(s, errp) ||
        !rk3588_soc_create_its(s, errp) ||
        !rk3588_soc_create_uart(s, errp) ||
        !rk3588_soc_create_sdhci(s, errp) ||
        !rk3588_soc_create_sdmmc(s, errp) ||
        !rk3588_soc_create_sfc(s, errp) ||
        !rk3588_soc_create_usb2_host(s, errp) ||
        !rk3588_soc_create_usb(s, errp) ||
        !rk3588_soc_create_cru(s, errp) ||
        !rk3588_soc_create_grf(s, errp) ||
        !rk3588_soc_create_syscons(s, errp) ||
        !rk3588_soc_create_ddr(s, errp) ||
        !rk3588_soc_create_scmi(s, errp) ||
        !rk3588_soc_create_rknpu(s, errp) ||
        !rk3588_soc_create_pcie(s, errp) ||
        !rk3588_soc_create_secure_otp(s, errp) ||
        !rk3588_soc_create_crypto(s, errp) ||
        !rk3588_soc_create_tsadc(s, errp) ||
        !rk3588_soc_create_saradc(s, errp) ||
        !rk3588_soc_create_spi(s, errp) ||
        !rk3588_soc_create_rng(s, errp) ||
        !rk3588_soc_create_dmac1(s, errp) ||
        !rk3588_soc_create_stimer(s, errp) ||
        !rk3588_soc_create_gpio(s, errp) ||
        !rk3588_soc_create_pwm(s, errp) ||
        !rk3588_soc_create_i2c6(s, errp) ||
        !rk3588_soc_create_gmac(s, errp) ||
        !rk3588_soc_create_otp(s, errp)) {
        return;
    }

    create_unimplemented_devices(OBJECT(s), s->memory, rk3588_unimpl_devices);
}

static void rk3588_soc_instance_init(Object *obj)
{
    RK3588SoCState *s = RK3588_SOC(obj);

    object_initialize_child(obj, "gic", &s->gic, TYPE_ARM_GICV3);

    for (unsigned int i = 0; i < RK3588_NUM_UARTS; i++) {
        g_autofree char *name = g_strdup_printf("uart%u", i);
        g_autofree char *alias = g_strdup_printf("uart%u-chardev", i);

        object_initialize_child(obj, name, &s->uart[i], TYPE_DW8250);
        object_property_add_alias(obj, alias, OBJECT(&s->uart[i]),
                                  "chardev");
    }

    s->usb3otg0 = qdev_new(TYPE_DWC3_UDC);
    object_property_add_child(obj, "usb3otg0", OBJECT(s->usb3otg0));
    object_property_add_alias(obj, "usb3otg0-chardev",
                              OBJECT(s->usb3otg0), "chardev");
    object_unref(OBJECT(s->usb3otg0));

}

static const Property rk3588_soc_properties[] = {
    DEFINE_PROP_LINK("memory", RK3588SoCState, memory, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT32("num-cpus", RK3588SoCState, num_cpus,
                       RK3588_NUM_CPUS),
    DEFINE_PROP_UINT64("ram-size", RK3588SoCState, ram_size, 0),
    DEFINE_PROP_BOOL("rknpu", RK3588SoCState, rknpu, false),
    DEFINE_PROP_UINT32("dram-type", RK3588SoCState, dram_type, 0),
    DEFINE_PROP_UINT32("gmac-mask", RK3588SoCState, gmac_mask, 0),
    DEFINE_PROP_BOOL("secure-otp-unfused", RK3588SoCState,
                     secure_otp_unfused, false),
    DEFINE_PROP_BOOL("crypto-v2-sha256", RK3588SoCState,
                     crypto_v2_sha256, false),
};

static const VMStateDescription vmstate_rk3588_soc = {
    .name = TYPE_RK3588_SOC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void rk3588_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rk3588_soc_realize;
    dc->desc = "Rockchip RK3588";
    dc->vmsd = &vmstate_rk3588_soc;
    dc->user_creatable = false;
    device_class_set_props(dc, rk3588_soc_properties);
}

static const TypeInfo rk3588_soc_typeinfo = {
    .name = TYPE_RK3588_SOC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(RK3588SoCState),
    .class_init = rk3588_soc_class_init,
    .instance_init = rk3588_soc_instance_init,
};

static void rk3588_soc_register_types(void)
{
    type_register_static(&rk3588_soc_typeinfo);
}

type_init(rk3588_soc_register_types)
