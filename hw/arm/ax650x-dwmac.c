/*
 * AX650X DWMAC board integration
 *
 * This file owns the networking-related board topology and FDT nodes.  The
 * reusable devices themselves live in hw/net, hw/gpio, and hw/misc.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/ax650x-dwmac.h"
#include "hw/arm/fdt.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/gpio/dw-apb-gpio.h"
#include "hw/misc/ax650x-dwmac-glue.h"
#include "hw/misc/ax650x-hwspinlock.h"
#include "hw/misc/unimp.h"
#include "hw/net/dwmac4.h"
#include "net/net.h"
#include "system/device_tree.h"

#define AX650X_DWMAC_NR              2
#define AX650X_DWMAC0_BASE           0x10140000
#define AX650X_DWMAC1_BASE           0x30800000
#define AX650X_DWMAC0_IRQ            104
#define AX650X_DWMAC1_IRQ            186
#define AX650X_DWMAC0_GLB_BASE       0x10000000
#define AX650X_DWMAC1_GLB_BASE       0x30000000
#define AX650X_DWMAC0_CLK_BASE       0x10010000
#define AX650X_DWMAC1_CLK_BASE       0x30010000
#define AX650X_GPIO0_BASE            0x02003000
#define AX650X_GPIO1_BASE            0x02004000
#define AX650X_HWSPINLOCK_BASE       0x04510000
#define AX650X_DPHY_BASE             0x13c00000
#define AX650X_DPHY_SIZE             0x00080000

#define AX650X_DWMAC0_CLOCK_HZ       400000000
#define AX650X_DWMAC1_CLOCK_HZ       500000000
#define AX650X_EPHY_CLOCK_HZ         25000000
#define AX650X_RGMII_CLOCK_HZ        125000000
#define AX650X_RMII_CLOCK_HZ         50000000
#define AX650X_PTP_CLOCK_HZ          50000000
#define AX650X_HWSPINLOCK_CLOCK_HZ   200000000

static const hwaddr dwmac_base[AX650X_DWMAC_NR] = {
    AX650X_DWMAC0_BASE, AX650X_DWMAC1_BASE,
};

static const unsigned int dwmac_irq[AX650X_DWMAC_NR] = {
    AX650X_DWMAC0_IRQ, AX650X_DWMAC1_IRQ,
};

static const hwaddr dwmac_glb_base[AX650X_DWMAC_NR] = {
    AX650X_DWMAC0_GLB_BASE, AX650X_DWMAC1_GLB_BASE,
};

static const hwaddr dwmac_clk_base[AX650X_DWMAC_NR] = {
    AX650X_DWMAC0_CLK_BASE, AX650X_DWMAC1_CLK_BASE,
};

static const hwaddr gpio_base[AX650X_DWMAC_NR] = {
    AX650X_GPIO0_BASE, AX650X_GPIO1_BASE,
};

static const char *const eth_path[AX650X_DWMAC_NR] = {
    "/soc/ethernet@10140000",
    "/soc/ethernet@30800000",
};

static const char *const gpio_path[AX650X_DWMAC_NR] = {
    "/soc/gpio@2003000",
    "/soc/gpio@2004000",
};

void ax650x_dwmac_create(DeviceState *gic)
{
    DeviceState *hwspinlock = qdev_new(TYPE_AX650X_HWSPINLOCK);
    SysBusDevice *sbd = SYS_BUS_DEVICE(hwspinlock);

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, AX650X_HWSPINLOCK_BASE);

    /*
     * The vendor AXERA GPIO driver reads all eight DPHY windows before it
     * registers GPIO lines.  DPHY behavior is not part of networking, but a
     * named unimplemented window keeps those discovery reads non-fatal.
     */
    create_unimplemented_device("ax650x.dphy", AX650X_DPHY_BASE,
                                AX650X_DPHY_SIZE);

    for (unsigned int i = 0; i < AX650X_DWMAC_NR; i++) {
        DeviceState *gpio = qdev_new(TYPE_DW_APB_GPIO);
        DeviceState *glue = qdev_new(TYPE_AX650X_DWMAC_GLUE);
        DeviceState *mac = qdev_new(TYPE_DWMAC4);

        sbd = SYS_BUS_DEVICE(gpio);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, gpio_base[i]);

        qdev_prop_set_uint8(glue, "port", i);
        sbd = SYS_BUS_DEVICE(glue);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, dwmac_glb_base[i]);
        sysbus_mmio_map(sbd, 1, dwmac_clk_base[i]);

        qemu_configure_nic_device(mac, i == 0, i == 0 ? "gmac0" : "gmac1");
        qdev_prop_set_uint8(mac, "snps-version", 0x52);
        qdev_prop_set_uint8(mac, "user-version", 0x10);
        qdev_prop_set_uint8(mac, "dma-width", 40);
        qdev_prop_set_uint8(mac, "phy-addr", 1);
        qdev_prop_set_uint16(mac, "phy-id1", 0x937c);
        qdev_prop_set_uint16(mac, "phy-id2", 0x4030);
        qdev_prop_set_uint32(mac, "rx-fifo-size", 65536);
        qdev_prop_set_uint32(mac, "tx-fifo-size", 32768);
        sbd = SYS_BUS_DEVICE(mac);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, dwmac_base[i]);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(gic, dwmac_irq[i]));
    }
}

static uint32_t ax650x_fdt_add_fixed_clock(void *fdt, const char *path,
                                           uint32_t frequency)
{
    uint32_t phandle;

    qemu_fdt_add_subnode(fdt, path);
    qemu_fdt_setprop_string(fdt, path, "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(fdt, path, "#clock-cells", 0);
    qemu_fdt_setprop_cell(fdt, path, "clock-frequency", frequency);
    phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_setprop_cell(fdt, path, "phandle", phandle);

    return phandle;
}

static void ax650x_dwmac_add_phy_fdt(void *fdt, const char *path,
                                     uint32_t *phandle)
{
    qemu_fdt_add_subnode(fdt, path);
    qemu_fdt_setprop_string(fdt, path, "compatible",
                            "ethernet-phy-id937c.4030");
    qemu_fdt_setprop_cell(fdt, path, "reg", 1);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,led-enable", 0x5);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,led-mode", 0x6251);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,led-period", 0x3);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,led-on", 0x2);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,led-polarity", 0x1c00);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,patch-enable", 0x1);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,rgmii-enable", 0x5);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,clk-enable", 0x9);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,fld-enable", 0x3);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,fld-delay", 0);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,wol-enable", 0x3);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,interrupt-enable", 0x5);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,downshift-enable", 0x3);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,downshift-count", 0x3);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,work_mode-enable", 0x1);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,work_mode-mode", 0);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,lpbk-enable", 0x1);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,lpbk-mode", 0x2);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,slew_rate-enable", 0x1);
    qemu_fdt_setprop_cell(fdt, path, "jl2xxx,rxc_out-enable", 0x1);
    *phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_setprop_cell(fdt, path, "phandle", *phandle);
}

void ax650x_dwmac_create_fdt(void *fdt)
{
    static const unsigned int phy_reset_pin[AX650X_DWMAC_NR] = { 7, 11 };
    const char clock_names[] =
        "emac_aclk\0ephy_clk\0rgmii_tx_clk\0rmii_phy_clk\0rmii_rx_clk\0"
        "ptp_clk";
    uint32_t dwmac_clock_phandles[AX650X_DWMAC_NR];
    uint32_t gpio_phandles[AX650X_DWMAC_NR];
    uint32_t ephy_clock_phandle;
    uint32_t rgmii_clock_phandle;
    uint32_t rmii_clock_phandle;
    uint32_t ptp_clock_phandle;
    uint32_t hwspinlock_clock_phandle;
    uint32_t stmmac_axi_phandle;
    uint32_t mtl_rx_phandle;
    uint32_t mtl_tx_phandle;

    qemu_fdt_setprop_string(fdt, "/aliases", "ethernet0", eth_path[0]);
    qemu_fdt_setprop_string(fdt, "/aliases", "ethernet1", eth_path[1]);

    dwmac_clock_phandles[0] = ax650x_fdt_add_fixed_clock(
        fdt, "/dwmac0-clock", AX650X_DWMAC0_CLOCK_HZ);
    dwmac_clock_phandles[1] = ax650x_fdt_add_fixed_clock(
        fdt, "/dwmac1-clock", AX650X_DWMAC1_CLOCK_HZ);
    ephy_clock_phandle = ax650x_fdt_add_fixed_clock(
        fdt, "/ephy-clock", AX650X_EPHY_CLOCK_HZ);
    rgmii_clock_phandle = ax650x_fdt_add_fixed_clock(
        fdt, "/rgmii-clock", AX650X_RGMII_CLOCK_HZ);
    rmii_clock_phandle = ax650x_fdt_add_fixed_clock(
        fdt, "/rmii-clock", AX650X_RMII_CLOCK_HZ);
    ptp_clock_phandle = ax650x_fdt_add_fixed_clock(
        fdt, "/ptp-clock", AX650X_PTP_CLOCK_HZ);
    hwspinlock_clock_phandle = ax650x_fdt_add_fixed_clock(
        fdt, "/hwspinlock-clock", AX650X_HWSPINLOCK_CLOCK_HZ);

    qemu_fdt_add_subnode(fdt, "/soc/ax_hwspinlock@4510000");
    qemu_fdt_setprop_string(fdt, "/soc/ax_hwspinlock@4510000", "compatible",
                            "axera,hwspinlock-r1p0");
    qemu_fdt_setprop_sized_cells(fdt, "/soc/ax_hwspinlock@4510000", "reg",
                                 2, AX650X_HWSPINLOCK_BASE,
                                 2, AX650X_HWSPINLOCK_MMIO_SIZE);
    qemu_fdt_setprop_cell(fdt, "/soc/ax_hwspinlock@4510000", "clocks",
                          hwspinlock_clock_phandle);
    qemu_fdt_setprop_string(fdt, "/soc/ax_hwspinlock@4510000", "clock-names",
                            "spinlock_pclk");
    qemu_fdt_setprop_string(fdt, "/soc/ax_hwspinlock@4510000", "status",
                            "okay");

    qemu_fdt_add_subnode(fdt, "/soc/stmmac-axi-config");
    qemu_fdt_setprop_cells(fdt, "/soc/stmmac-axi-config",
                           "snps,wr_osr_lmt", 15);
    qemu_fdt_setprop_cells(fdt, "/soc/stmmac-axi-config",
                           "snps,rd_osr_lmt", 15);
    qemu_fdt_setprop_cells(fdt, "/soc/stmmac-axi-config", "snps,blen",
                           0, 0, 0, 32, 16, 8, 4);
    stmmac_axi_phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_setprop_cell(fdt, "/soc/stmmac-axi-config", "phandle",
                          stmmac_axi_phandle);

    qemu_fdt_add_subnode(fdt, "/soc/rx-queues-config");
    qemu_fdt_setprop_cell(fdt, "/soc/rx-queues-config",
                          "snps,rx-queues-to-use", 1);
    qemu_fdt_add_subnode(fdt, "/soc/rx-queues-config/queue0");
    mtl_rx_phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_setprop_cell(fdt, "/soc/rx-queues-config", "phandle",
                          mtl_rx_phandle);

    qemu_fdt_add_subnode(fdt, "/soc/tx-queues-config");
    qemu_fdt_setprop_cell(fdt, "/soc/tx-queues-config",
                          "snps,tx-queues-to-use", 1);
    qemu_fdt_add_subnode(fdt, "/soc/tx-queues-config/queue0");
    mtl_tx_phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_setprop_cell(fdt, "/soc/tx-queues-config", "phandle",
                          mtl_tx_phandle);

    for (unsigned int i = 0; i < AX650X_DWMAC_NR; i++) {
        g_autofree char *gpio_port_path =
            g_strdup_printf("%s/gpio-controller@0", gpio_path[i]);
        g_autofree char *mdio_path = g_strdup_printf("%s/mdio", eth_path[i]);
        g_autofree char *phy_path = g_strdup_printf("%s/jl2xxx-phy@1",
                                                    mdio_path);
        uint32_t phy_phandle;

        qemu_fdt_add_subnode(fdt, gpio_path[i]);
        qemu_fdt_setprop_string(fdt, gpio_path[i], "compatible",
                                "snps,ax-apb-gpio");
        qemu_fdt_setprop_cell(fdt, gpio_path[i], "#address-cells", 1);
        qemu_fdt_setprop_cell(fdt, gpio_path[i], "#size-cells", 0);
        qemu_fdt_setprop_sized_cells(fdt, gpio_path[i], "reg",
                                     2, gpio_base[i],
                                     2, DW_APB_GPIO_MMIO_SIZE);
        if (i == 0) {
            qemu_fdt_setprop_cell(fdt, gpio_path[i], "hwlock_id", 12);
        }
        qemu_fdt_setprop_string(fdt, gpio_path[i], "status", "okay");

        qemu_fdt_add_subnode(fdt, gpio_port_path);
        qemu_fdt_setprop_string(fdt, gpio_port_path, "compatible",
                                "snps,dw-apb-gpio-port");
        qemu_fdt_setprop_cell(fdt, gpio_port_path, "reg", 0);
        qemu_fdt_setprop(fdt, gpio_port_path, "gpio-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, gpio_port_path, "#gpio-cells", 2);
        qemu_fdt_setprop_cell(fdt, gpio_port_path, "snps,nr-gpios",
                              DW_APB_GPIO_NR_PINS);
        gpio_phandles[i] = qemu_fdt_alloc_phandle(fdt);
        qemu_fdt_setprop_cell(fdt, gpio_port_path, "phandle",
                              gpio_phandles[i]);

        qemu_fdt_add_subnode(fdt, eth_path[i]);
        qemu_fdt_setprop_string(fdt, eth_path[i], "compatible",
                                "axera,dwmac-4.10a");
        qemu_fdt_setprop_sized_cells(fdt, eth_path[i], "reg",
                                     2, dwmac_base[i],
                                     2, DWMAC4_MMIO_SIZE);
        qemu_fdt_setprop_cells(fdt, eth_path[i], "interrupts",
                               GIC_FDT_IRQ_TYPE_SPI, dwmac_irq[i],
                               GIC_FDT_IRQ_FLAGS_LEVEL_HI);
        qemu_fdt_setprop_string(fdt, eth_path[i], "interrupt-names",
                                "macirq");
        qemu_fdt_setprop_cells(fdt, eth_path[i], "clocks",
                               dwmac_clock_phandles[i], ephy_clock_phandle,
                               rgmii_clock_phandle, rmii_clock_phandle,
                               rmii_clock_phandle, ptp_clock_phandle);
        qemu_fdt_setprop(fdt, eth_path[i], "clock-names", clock_names,
                         sizeof(clock_names));
        qemu_fdt_setprop_cell(fdt, eth_path[i], "rx-fifo-depth", 65536);
        qemu_fdt_setprop_cell(fdt, eth_path[i], "tx-fifo-depth", 32768);
        qemu_fdt_setprop_cell(fdt, eth_path[i], "snps,axi-config",
                              stmmac_axi_phandle);
        qemu_fdt_setprop_cell(fdt, eth_path[i], "snps,mtl-rx-config",
                              mtl_rx_phandle);
        qemu_fdt_setprop_cell(fdt, eth_path[i], "snps,mtl-tx-config",
                              mtl_tx_phandle);
        qemu_fdt_setprop_cells(fdt, eth_path[i], "phy-rst-gpio",
                               gpio_phandles[i], phy_reset_pin[i], 0);
        qemu_fdt_setprop_string(fdt, eth_path[i], "phy-mode", "rgmii");
        qemu_fdt_setprop_string(fdt, eth_path[i], "status", "okay");

        qemu_fdt_add_subnode(fdt, mdio_path);
        qemu_fdt_setprop_string(fdt, mdio_path, "compatible",
                                "snps,dwmac-mdio");
        qemu_fdt_setprop_cell(fdt, mdio_path, "#address-cells", 1);
        qemu_fdt_setprop_cell(fdt, mdio_path, "#size-cells", 0);

        ax650x_dwmac_add_phy_fdt(fdt, phy_path, &phy_phandle);
        qemu_fdt_setprop_cell(fdt, eth_path[i], "phy-handle", phy_phandle);
    }
}
