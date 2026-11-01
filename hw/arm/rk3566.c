/*
 * Rockchip RK3566 SoC
 *
 * The CPU cluster, GIC, clock and reset unit, DMA controllers, bus
 * controllers and the on-chip peripherals modeled by this series.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qobject/qlist.h"
#include "hw/arm/bsa.h"
#include "hw/arm/rk3566.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/gpio/rockchip_gpio.h"
#include "hw/i2c/rk3x_i2c.h"
#include "hw/intc/arm_gicv3_its_common.h"
#include "hw/misc/rk3588_scmi.h"
#include "hw/misc/rockchip_iommu.h"
#include "hw/misc/rockchip_syscon.h"
#include "hw/sd/dw_mmc.h"
#include "hw/sd/rockchip_dwcmshc.h"
#include "hw/sd/sd.h"
#include "hw/sd/sdhci.h"
#include "hw/ssi/rockchip_spi.h"
#include "hw/usb/dwc3-udc.h"
#include "hw/watchdog/k230_wdt.h"
#include "system/address-spaces.h"
#include "target/arm/gtimer.h"
#include "target/arm/internals.h"

static uint64_t debug_read(void *opaque, hwaddr addr, unsigned size)
{
    RK3566SoCState *s = opaque;
    unsigned cpu = (addr >> 12) & 3;
    uint64_t pc = s->cpu[cpu].env.pc;
    switch (addr & 0xfff) {
    case 0xfc8: return 3;
    case 0x314: return s->cpu[cpu].power_state == PSCI_ON;
    case 0x0a0: case 0x200: return (uint32_t)pc;
    case 0x0ac: case 0x204: return pc >> 32;
    default: return s->debug_regs[addr / 4];
    }
}

static void debug_write(void *opaque, hwaddr addr, uint64_t value,
                        unsigned size)
{
    RK3566SoCState *s = opaque;
    s->debug_regs[addr / 4] = value;
}

static const MemoryRegionOps debug_ops = {
    .read = debug_read,
    .write = debug_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t qos_read(void *opaque, hwaddr addr, unsigned size)
{
    return ((uint32_t *)opaque)[addr / 4];
}
static void qos_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    ((uint32_t *)opaque)[addr / 4] = value;
}
static const MemoryRegionOps qos_ops = {
    .read = qos_read, .write = qos_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void uart_irq(void *opaque, int index, int level)
{
    RK3566SoCState *s = opaque;
    SerialState *serial = &s->uart[index]->serial_mm.serial;
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->gic), 116 + index), level);
    if (s->dma[0]) {
        qemu_set_irq(qdev_get_gpio_in(s->dma[0], 2 * index),
                     !(serial->lsr & BIT(5)));
        qemu_set_irq(qdev_get_gpio_in(s->dma[0], 2 * index + 1),
                     !(serial->lsr & BIT(0)));
    }
}

static DeviceState *rk3566_device(RK3566SoCState *s, const char *type,
                                const char *name)
{
    DeviceState *dev = qdev_new(type);
    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    object_unref(OBJECT(dev));
    return dev;
}

static void rk3566_map(RK3566SoCState *s, DeviceState *dev, hwaddr address,
                      int irq)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, address);
    if (irq >= 0) {
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(&s->gic), irq));
    }
}

static void rk3566_cpus(RK3566SoCState *s)
{
    object_initialize_child(OBJECT(s), "gic", &s->gic, TYPE_ARM_GICV3);
    DeviceState *gic = DEVICE(&s->gic);
    QList *regions = qlist_new();
    object_initialize_child(OBJECT(s), "cluster", &s->cluster,
                            TYPE_CPU_CLUSTER);
    qdev_prop_set_uint32(DEVICE(&s->cluster), "cluster-id", 0);
    for (unsigned i = 0; i < RK3566_CPUS; i++) {
        g_autofree char *name = g_strdup_printf("cpu%u", i);
        object_initialize_child(OBJECT(&s->cluster), name, &s->cpu[i],
                                ARM_CPU_TYPE_NAME("cortex-a55"));
    }
    qdev_realize(DEVICE(&s->cluster), NULL, &error_fatal);
    for (unsigned i = 0; i < RK3566_CPUS; i++) {
        Object *cpu = OBJECT(&s->cpu[i]);
        object_property_set_int(cpu, "mp-affinity", i << 8, &error_abort);
        object_property_set_int(cpu, "cntfrq", 24000000, &error_abort);
        object_property_set_bool(cpu, "start-powered-off", i != 0,
                                 &error_abort);
        object_property_set_bool(cpu, "has_el3", false, &error_abort);
        object_property_set_link(cpu, "memory", OBJECT(get_system_memory()),
                                 &error_abort);
        qdev_realize(DEVICE(cpu), NULL, &error_fatal);
    }
    qdev_prop_set_uint32(gic, "num-cpu", RK3566_CPUS);
    qdev_prop_set_uint32(gic, "num-irq", RK3566_IRQS);
    qlist_append_int(regions, RK3566_CPUS);
    qdev_prop_set_array(gic, "redist-region-count", regions);
    qdev_prop_set_bit(gic, "has-lpi", true);
    object_property_set_link(OBJECT(gic), "sysmem", OBJECT(get_system_memory()),
                             &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 0, 0xfd400000);
    sysbus_mmio_map(SYS_BUS_DEVICE(gic), 1, 0xfd460000);
    for (unsigned i = 0; i < RK3566_CPUS; i++) {
        DeviceState *cpu = DEVICE(&s->cpu[i]);
        unsigned base = RK3566_IRQS - GIC_INTERNAL + i * GIC_INTERNAL;
        const unsigned timers[] = {
            ARCH_TIMER_NS_EL1_IRQ, ARCH_TIMER_VIRT_IRQ,
            ARCH_TIMER_NS_EL2_IRQ, ARCH_TIMER_S_EL1_IRQ,
            ARCH_TIMER_NS_EL2_VIRT_IRQ,
        };
        for (unsigned t = 0; t < ARRAY_SIZE(timers); t++) {
            qdev_connect_gpio_out(cpu, t,
                                  qdev_get_gpio_in(gic, base + timers[t]));
        }
        qdev_connect_gpio_out_named(cpu, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(gic, base +
                                                     ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpu, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(gic, base +
                                                     VIRTUAL_PMU_IRQ));
        for (unsigned line = 0; line < 4; line++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + line * RK3566_CPUS,
                               qdev_get_gpio_in(cpu, line));
        }
    }
    DeviceState *its = rk3566_device(s, its_class_name(), "its");
    object_property_set_link(OBJECT(its), "parent-gicv3", OBJECT(gic),
                             &error_abort);
    rk3566_map(s, its, 0xfd440000, -1);
}


static void rk3566_storage(RK3566SoCState *s)
{
    DeviceState *host = rk3566_device(s, TYPE_SYSBUS_SDHCI, "emmc");

    qdev_prop_set_uint8(host, "sd-spec-version", 3);
    qdev_prop_set_uint64(host, "capareg", 0x280737ec6481);
    object_property_set_uint(OBJECT(host), "uhs", UHS_I, &error_abort);
    object_property_set_uint(OBJECT(host), "vendor-area1", 0x500, &error_abort);
    object_property_set_uint(OBJECT(host), "vendor-area2", 0x800, &error_abort);
    rk3566_map(s, host, 0xfe310000, 19);
    DeviceState *vendor = rk3566_device(s, TYPE_ROCKCHIP_DWCMSHC_VENDOR,
                                        "emmc-vendor");
    rk3566_map(s, vendor, 0xfe310000 + ROCKCHIP_DWCMSHC_VENDOR_BASE, -1);
    s->sdhci = host;
}

static void rk3566_dma(RK3566SoCState *s)
{
    for (unsigned i = 0; i < 2; i++) {
        g_autofree char *name = g_strdup_printf("dmac%u", i);
        g_autofree char *irq_name = g_strdup_printf("dmac%u-irqs", i);
        DeviceState *dma = rk3566_device(s, "pl330", name);
        s->dma[i] = dma;
        qdev_prop_set_uint8(dma, "num_periph_req", 32);
        object_property_set_link(OBJECT(dma), "memory",
                                 OBJECT(get_system_memory()), &error_abort);
        rk3566_map(s, dma, 0xfe530000 + i * 0x20000, 13 + 2 * i);
        memory_region_init_alias(&s->dma_alias[i], OBJECT(s), name,
            sysbus_mmio_get_region(SYS_BUS_DEVICE(dma), 0), 0, 0x1000);
        memory_region_add_subregion(get_system_memory(),
                                    0xfe533000 + i * 0x20000,
                                    &s->dma_alias[i]);
        DeviceState *combine = rk3566_device(s, TYPE_OR_IRQ, irq_name);
        qdev_prop_set_uint16(combine, "num-lines", 16);
        qdev_realize(combine, NULL, &error_fatal);
        qdev_connect_gpio_out(combine, 0,
                              qdev_get_gpio_in(DEVICE(&s->gic), 14 + 2 * i));
        for (unsigned line = 0; line < 16; line++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(dma), line + 1,
                               qdev_get_gpio_in(combine, line));
        }
        for (unsigned request = 0; request < 32; request++) {
            qemu_set_irq(qdev_get_gpio_in(dma, request), 1);
        }
    }
}

static void rk3566_soc_realize(DeviceState *dev, Error **errp)
{
    RK3566SoCState *s = RK3566_SOC(dev);
    bool media = object_class_by_name("rk3566-isp") &&
                 object_class_by_name("rk3566-csi-dphy") &&
                 object_class_by_name("rk3566-rga2") &&
                 object_class_by_name("rk3566-rkvenc");

    rk3566_cpus(s);
    memory_region_init_io(&s->debug, OBJECT(s), &debug_ops, s,
                          "rk3566-debug", 0x10000);
    memory_region_add_subregion(get_system_memory(), 0xfd900000, &s->debug);
    rk3566_dma(s);
    const hwaddr qos[] = { 0xfe128000, 0xfe138080, 0xfe138100, 0xfe138180,
        0xfe148000, 0xfe148080, 0xfe148100, 0xfe150000, 0xfe158000, 0xfe158100,
        0xfe158180, 0xfe158200, 0xfe158280, 0xfe158300, 0xfe180000, 0xfe190000,
        0xfe190280, 0xfe190300, 0xfe190380, 0xfe190400, 0xfe198000, 0xfe1a8000,
        0xfe1a8080, 0xfe1a8100 };
    for (unsigned i = 0; i < ARRAY_SIZE(qos); i++) {
        g_autofree char *name = g_strdup_printf("qos-%x", (unsigned)qos[i]);
        memory_region_init_io(&s->qos[i], OBJECT(s), &qos_ops,
                              s->qos_regs[i], name, 0x20);
        memory_region_add_subregion(get_system_memory(), qos[i], &s->qos[i]);
    }

    s->cru = rk3566_device(s, "rk3566-cru", "cru");
    rk3566_map(s, s->cru, 0xfdd20000, -1);
    s->pmucru = rk3566_device(s, "rk3566-cru", "pmucru");
    qdev_prop_set_bit(s->pmucru, "pmu", true);
    rk3566_map(s, s->pmucru, 0xfdd00000, -1);

    const hwaddr uart_base[] = { 0xfdd50000, 0xfe650000,
                                 0xfe660000, 0xfe670000 };
    const int uart_irqs[] = { 116, 117, 118, 119 };

    for (unsigned i = 0; i < ARRAY_SIZE(uart_base); i++) {
        DeviceState *uart = DEVICE(s->uart[i]);

        qdev_prop_set_uint32(uart, "baudbase", 24000000 / 16);
        qdev_prop_set_uint32(uart, "fifo-size", 64);
        if (i > 0) {
            g_autofree char *clock_name = g_strdup_printf("sclk-uart%u", i);

            qdev_connect_clock_in(uart, "uartclk",
                                  qdev_get_clock_out(s->cru, clock_name));
        }
        sysbus_realize(SYS_BUS_DEVICE(uart), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(uart), 0, uart_base[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0,
                           qemu_allocate_irq(uart_irq, s, i));
        sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0,
                           qdev_get_gpio_in(DEVICE(&s->gic), uart_irqs[i]));
    }

    const hwaddr syscon[] = { 0xfdc20000, 0xfdc50000, 0xfdc60000,
        0xfdc80000, 0xfdc90000, 0xfdca0000, 0xfdcb0000,
        0xfe8a0000, 0xfe8b0000 };
    for (unsigned i = 0; i < ARRAY_SIZE(syscon); i++) {
        g_autofree char *name = g_strdup_printf("syscon%u", i);
        DeviceState *dev = rk3566_device(s, TYPE_ROCKCHIP_SYSCON, name);
        qdev_prop_set_uint32(dev, "size", 0x10000);
        rk3566_map(s, dev, syscon[i], -1);
    }
    const hwaddr gpio[] = { 0xfdd60000, 0xfe740000, 0xfe750000,
                            0xfe760000, 0xfe770000 };
    for (unsigned i = 0; i < ARRAY_SIZE(gpio); i++) {
        g_autofree char *name = g_strdup_printf("gpio%u", i);
        s->gpio[i] = rk3566_device(s, TYPE_ROCKCHIP_GPIO, name);
        rk3566_map(s, s->gpio[i], gpio[i], 33 + i);
    }

    if (media) {
        DeviceState *isp_mmu = rk3566_device(s, TYPE_ROCKCHIP_IOMMU,
                                             "isp-mmu");
        sysbus_realize(SYS_BUS_DEVICE(isp_mmu), &error_fatal);
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(isp_mmu), 0, 0xfdff1a00, 1);
        sysbus_connect_irq(SYS_BUS_DEVICE(isp_mmu), 0,
                           qdev_get_gpio_in(DEVICE(&s->gic), 59));
        s->isp = rk3566_device(s, "rk3566-isp", "isp");
        object_property_set_link(OBJECT(s->isp), "dma",
            OBJECT(rockchip_iommu_get_memory_region(
                ROCKCHIP_IOMMU(isp_mmu))), &error_abort);
        qdev_prop_set_chr(s->isp, "chardev",
                          qemu_chr_find(s->camera_chardev));
        rk3566_map(s, s->isp, 0xfdff0000, 57);
        sysbus_connect_irq(SYS_BUS_DEVICE(s->isp), 1,
                           qdev_get_gpio_in(DEVICE(&s->gic), 58));
        sysbus_connect_irq(SYS_BUS_DEVICE(s->isp), 2,
                           qdev_get_gpio_in(DEVICE(&s->gic), 60));
        DeviceState *dphy = rk3566_device(s, "rk3566-csi-dphy", "csi-dphy");
        rk3566_map(s, dphy, 0xfe870000, -1);
        qdev_connect_gpio_out_named(dphy, "link-enabled", 0,
            qdev_get_gpio_in_named(s->isp, "link-enabled", 0));
    }

    const hwaddr i2c[] = { 0xfdd40000, 0xfe5a0000, 0xfe5b0000, 0xfe5c0000 };

    for (unsigned i = 0; i < ARRAY_SIZE(i2c); i++) {
        g_autofree char *name = g_strdup_printf("i2c%u", i);

        s->i2c[i] = rk3566_device(s, TYPE_RK3X_I2C, name);
        rk3566_map(s, s->i2c[i], i2c[i], 46 + i);
    }

    DeviceState *scmi = rk3566_device(s, TYPE_RK3588_SCMI, "scmi");
    qdev_prop_set_string(scmi, "soc", "rk3566");
    s->scmi = RK3588_SCMI(scmi);
    sysbus_realize(SYS_BUS_DEVICE(scmi), &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(), 0x10f000,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(scmi), 0), 1);

    rk3566_storage(s);

    s->sdio = rk3566_device(s, TYPE_DW_MMC, "sdio");
    rk3566_map(s, s->sdio, 0xfe2c0000, 99);

    for (unsigned i = 1; i <= 3; i += 2) {
        g_autofree char *name = g_strdup_printf("spi%u", i);
        g_autofree char *clock = g_strdup_printf("sclk-spi%u", i);
        s->spi[i] = rk3566_device(s, TYPE_ROCKCHIP_SPI, name);
        qdev_connect_clock_in(s->spi[i], "spiclk",
                              qdev_get_clock_out(s->cru, clock));
        rk3566_map(s, s->spi[i], 0xfe610000 + i * 0x10000, 103 + i);
        qdev_connect_gpio_out_named(s->spi[i], "dma-busy", 0,
                                    qdev_get_gpio_in(s->dma[0], 20 + i * 2));
        qdev_connect_gpio_out_named(s->spi[i], "dma-busy", 1,
                                    qdev_get_gpio_in(s->dma[0], 21 + i * 2));
    }

    rk3566_map(s, rk3566_device(s, "rk3566-system", "pmu"), 0xfdd90000, -1);
    DeviceState *rng = rk3566_device(s, "rk3566-system", "rng");
    qdev_prop_set_bit(rng, "rng", true);
    rk3566_map(s, rng, 0xfe388000, -1);
    const hwaddr pvtm[] = { 0xfde00000, 0xfde80000, 0xfde90000 };
    for (unsigned i = 0; i < ARRAY_SIZE(pvtm); i++) {
        g_autofree char *name = g_strdup_printf("pvtm%u", i);
        DeviceState *dev = rk3566_device(s, "rk3566-system", name);
        qdev_prop_set_bit(dev, "pvtm", true);
        rk3566_map(s, dev, pvtm[i], -1);
    }

    qdev_prop_set_bit(s->usb, "host-connected", false);
    rk3566_map(s, s->usb, 0xfcc00000, 169);

    DeviceState *i2s1 = rk3566_device(s, "rk3566-i2s-tdm", "i2s1");
    qdev_connect_clock_in(i2s1, "mclk",
                          qdev_get_clock_out(s->cru, "mclk-i2s1"));
    rk3566_map(s, i2s1, 0xfe410000, 53);
    qdev_connect_gpio_out_named(i2s1, "dma-busy", 0,
                                qdev_get_gpio_in(s->dma[1], 2));
    qdev_connect_gpio_out_named(i2s1, "dma-busy", 1,
                                qdev_get_gpio_in(s->dma[1], 3));
    qdev_connect_gpio_out_named(i2s1, "lrck", 0,
                                qdev_get_gpio_in(s->gpio[1], 5));
    DeviceState *pdm = rk3566_device(s, "rk3566-pdm", "pdm");
    qdev_connect_clock_in(pdm, "mclk", qdev_get_clock_out(s->cru, "mclk-pdm"));
    rk3566_map(s, pdm, 0xfe440000, 76);
    qdev_connect_gpio_out_named(pdm, "dma-busy", 1,
                                qdev_get_gpio_in(s->dma[1], 9));
    if (media) {
        rk3566_map(s, rk3566_device(s, "rk3566-rga2", "rga2"),
                   0xfdeb0000, 90);
        DeviceState *venc_mmu[2];

        for (unsigned i = 0; i < 2; i++) {
            g_autofree char *name = g_strdup_printf("venc-mmu%u", i);

            venc_mmu[i] = rk3566_device(s, TYPE_ROCKCHIP_IOMMU, name);
            sysbus_realize(SYS_BUS_DEVICE(venc_mmu[i]), &error_fatal);
            memory_region_init_alias(&s->venc_mmu_window[i], OBJECT(s), name,
                sysbus_mmio_get_region(SYS_BUS_DEVICE(venc_mmu[i]), 0), 0,
                0x40);
            memory_region_add_subregion_overlap(get_system_memory(),
                0xfdf40f00 + i * 0x40, &s->venc_mmu_window[i], 1);
            sysbus_connect_irq(SYS_BUS_DEVICE(venc_mmu[i]), 0,
                               qdev_get_gpio_in(DEVICE(&s->gic), 141 + i));
        }
        DeviceState *venc = rk3566_device(s, "rk3566-rkvenc", "venc");

        object_property_set_link(OBJECT(venc), "dma",
            OBJECT(rockchip_iommu_get_memory_region(
                ROCKCHIP_IOMMU(venc_mmu[0]))), &error_abort);
        object_property_set_link(OBJECT(venc), "source-dma",
            OBJECT(rockchip_iommu_get_memory_region(
                ROCKCHIP_IOMMU(venc_mmu[1]))), &error_abort);
        rk3566_map(s, venc, 0xfdf40000, 140);
    }
    DeviceState *npu_mmu = rk3566_device(s, TYPE_ROCKCHIP_IOMMU, "npu-mmu");
    rk3566_map(s, npu_mmu, 0xfde4b000, -1);
    DeviceState *npu = rk3566_device(s, "rk3566-rknpu", "npu");
    object_property_set_link(OBJECT(npu), "dma",
        OBJECT(rockchip_iommu_get_memory_region(ROCKCHIP_IOMMU(npu_mmu))),
        &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(npu), &error_fatal);
    const hwaddr npu_regs[] = { 0xfde40000, 0xfde41000, 0xfde43000, 0xfde44000,
        0xfde4f000, 0xfde46000, 0xfde47000 };
    for (unsigned i = 0; i < ARRAY_SIZE(npu_regs); i++) {
        sysbus_mmio_map(SYS_BUS_DEVICE(npu), i, npu_regs[i]);
    }
    DeviceState *npu_irqs = rk3566_device(s, TYPE_OR_IRQ, "npu-irqs");
    qdev_prop_set_uint16(npu_irqs, "num-lines", 2);
    qdev_realize(npu_irqs, NULL, &error_fatal);
    sysbus_connect_irq(SYS_BUS_DEVICE(npu), 0, qdev_get_gpio_in(npu_irqs, 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(npu_mmu), 0,
                       qdev_get_gpio_in(npu_irqs, 1));
    qdev_connect_gpio_out(npu_irqs, 0, qdev_get_gpio_in(DEVICE(&s->gic), 151));
    rk3566_map(s, rk3566_device(s, "rk3566-otp", "otp"), 0xfe38c000, -1);
    rk3566_map(s, rk3566_device(s, "rk3566-tsadc", "tsadc"), 0xfe710000, 115);
    rk3566_map(s, rk3566_device(s, "rk3566-saradc", "saradc"), 0xfe720000, 93);
    rk3566_map(s, rk3566_device(s, "rk3566-pwm", "pwm4-7"), 0xfe6e0000, -1);
    rk3566_map(s, rk3566_device(s, "rk3566-pwm", "pwm8-11"), 0xfe6f0000, -1);
    DeviceState *wdt = rk3566_device(s, TYPE_K230_WDT, "watchdog");
    rk3566_map(s, wdt, 0xfe600000, 149);
    ptimer_transaction_begin(K230_WDT(wdt)->timer);
    ptimer_set_freq(K230_WDT(wdt)->timer, 24000000);
    ptimer_transaction_commit(K230_WDT(wdt)->timer);
    rk3566_map(s, rk3566_device(s, "rk3566-timer", "timer0"), 0xfe5f0000, 109);
}

static void rk3566_soc_instance_init(Object *obj)
{
    RK3566SoCState *s = RK3566_SOC(obj);

    for (unsigned i = 0; i < RK3566_NUM_UARTS; i++) {
        g_autofree char *name = g_strdup_printf("uart%u", i);
        g_autofree char *alias = g_strdup_printf("uart%u-chardev", i);

        s->uart[i] = DW8250(qdev_new(TYPE_DW8250));
        object_property_add_child(obj, name, OBJECT(s->uart[i]));
        object_property_add_alias(obj, alias, OBJECT(s->uart[i]), "chardev");
        object_unref(OBJECT(s->uart[i]));
    }

    s->usb = qdev_new(TYPE_DWC3_UDC);
    object_property_add_child(obj, "usb-otg", OBJECT(s->usb));
    object_property_add_alias(obj, "usb-chardev", OBJECT(s->usb), "chardev");
    object_unref(OBJECT(s->usb));
}

static const Property rk3566_soc_properties[] = {
    DEFINE_PROP_LINK("memory", RK3566SoCState, memory, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_STRING("camera-chardev", RK3566SoCState, camera_chardev),
};

static void rk3566_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = rk3566_soc_realize;
    device_class_set_props(dc, rk3566_soc_properties);
}

static const TypeInfo rk3566_soc_info = {
    .name = TYPE_RK3566_SOC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(RK3566SoCState),
    .instance_init = rk3566_soc_instance_init,
    .class_init = rk3566_soc_class_init,
};

static void rk3566_soc_register_types(void)
{
    type_register_static(&rk3566_soc_info);
}

type_init(rk3566_soc_register_types)
