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

#ifndef HW_ARM_RK3588_H
#define HW_ARM_RK3588_H

#include "qom/object.h"
#include "target/arm/cpu.h"
#include "hw/char/dw8250.h"
#include "hw/core/qdev.h"
#include "hw/cpu/cluster.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/intc/arm_gicv3_its_common.h"
#include "hw/usb/rk3588_usb2_host.h"
#include "system/memory.h"

#define TYPE_RK3588_SOC "rockchip.rk3588.soc"
OBJECT_DECLARE_SIMPLE_TYPE(RK3588SoCState, RK3588_SOC)

#define RK3588_A55_CPUS              (4)
#define RK3588_A76_CPUS_PER_CLUSTER  (2)

typedef enum RK3588CpuClusterId {
    RK3588_CLUSTER_A55,
    RK3588_CLUSTER_A76_0,
    RK3588_CLUSTER_A76_1,
    RK3588_CLUSTER_COUNT,
} RK3588CpuClusterId;

#define RK3588_NUM_CPUS (RK3588_A55_CPUS + \
    RK3588_A76_CPUS_PER_CLUSTER * (RK3588_CLUSTER_COUNT - 1))

#define RK3588_NUM_UARTS             (10)

/* SPI0-SPI3 carry the SPI controllers; SPI4 is not routed on the boards. */
#define RK3588_NUM_SPIS              (4)

/* PWM0-PWM3 each contain four channels. */
#define RK3588_NUM_PWMS              (4)

/* I2C0-I2C8 are modeled. */
#define RK3588_NUM_I2CS              (9)

typedef enum RK3588PCIEController {
    RK3588_PCIE3X4,
    RK3588_PCIE3X2,
    RK3588_PCIE2X1L0,
    RK3588_PCIE2X1L2,
    RK3588_PCIE_COUNT,
} RK3588PCIEController;

/* Board configuration, supplied before realizing the SoC. */
typedef struct RK3588PCIEConfig {
    bool enabled;
    bool link_up;
    uint32_t domain;
    uint8_t bus_nr;
    const char *id;
} RK3588PCIEConfig;

/* Indices into rk3588_memmap[], see hw/arm/rk3588.c. */
enum {
    RK3588_DEV_GIC_DIST,
    RK3588_DEV_GIC_REDIST,
    RK3588_DEV_GIC_ITS0,
    RK3588_DEV_GIC_ITS1,
    RK3588_DEV_CRU,
    RK3588_DEV_PWM0,
    RK3588_DEV_PWM1,
    RK3588_DEV_PWM2,
    RK3588_DEV_PWM3,
    RK3588_DEV_PMU0_GRF,
    RK3588_DEV_PMU1_GRF,
    RK3588_DEV_SYS_GRF,
    RK3588_DEV_OTP,
    RK3588_DEV_SFC,
    RK3588_DEV_SDMMC,
    RK3588_DEV_SDHCI,
    RK3588_DEV_USB3OTG0,
    RK3588_DEV_UART0,
    RK3588_DEV_UART1,
    RK3588_DEV_UART2,
    RK3588_DEV_UART3,
    RK3588_DEV_UART4,
    RK3588_DEV_UART5,
    RK3588_DEV_UART6,
    RK3588_DEV_UART7,
    RK3588_DEV_UART8,
    RK3588_DEV_UART9,
    RK3588_DEV_SPI0,
    RK3588_DEV_SPI1,
    RK3588_DEV_SPI2,
    RK3588_DEV_SPI3,
    RK3588_DEV_I2C0,
    RK3588_DEV_I2C1,
    RK3588_DEV_I2C2,
    RK3588_DEV_I2C3,
    RK3588_DEV_I2C4,
    RK3588_DEV_I2C5,
    RK3588_DEV_I2C6,
    RK3588_DEV_I2C7,
    RK3588_DEV_I2C8,
};

struct RK3588SoCState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    ARMCPU cpu[RK3588_NUM_CPUS];
    CPUClusterState cluster[RK3588_CLUSTER_COUNT];
    GICv3State gic;
    DeviceState *its[2];
    DW8250State uart[RK3588_NUM_UARTS];
    DeviceState *usb3otg0;
    RK3588USB2HostState *usb2_host;
    DeviceState *pcie[RK3588_PCIE_COUNT];
    RK3588PCIEConfig pcie_config[RK3588_PCIE_COUNT];
    DeviceState *cru;
    DeviceState *pwm[RK3588_NUM_PWMS];
    DeviceState *ddr;
    DeviceState *scmi;
    DeviceState *otp;
    DeviceState *sfc;
    DeviceState *sdmmc;
    DeviceState *sdhci;
    DeviceState *gmac[2];
    DeviceState *secure_otp;
    DeviceState *crypto;
    DeviceState *tsadc;
    DeviceState *saradc;
    DeviceState *spi[RK3588_NUM_SPIS];
    DeviceState *dmac1;
    DeviceState *gpio[5];
    DeviceState *i2c[RK3588_NUM_I2CS];
    MemoryRegion *memory;
    bool rknpu;
    bool secure_otp_unfused;
    bool crypto_v2_sha256;
    uint64_t ram_size;
    uint32_t dram_type;
    uint32_t num_cpus;
    uint32_t gmac_mask;
};

/* Prepare nonsecure interrupt ownership for the next GIC reset. */
void rk3588_soc_prepare_nonsecure_interrupts(RK3588SoCState *s);

/* Firmware service used by boards that intercept the SCMI SMC. */
bool rk3588_soc_handle_scmi_smc(RK3588SoCState *s, ARMCPU *cpu);

#endif /* HW_ARM_RK3588_H */
