/* SPDX-License-Identifier: GPL-2.0-or-later */
/* RK3566 MicroDuck beta board, using the vendor Linux device addresses. */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include <libfdt.h>
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "chardev/char.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/dw8250.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/cpu/cluster.h"
#include "hw/gpio/rockchip_gpio.h"
#include "hw/i2c/rk3x_i2c.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/intc/arm_gicv3_its_common.h"
#include "hw/misc/rockchip_syscon.h"
#include "hw/misc/rk3588_scmi.h"
#include "hw/misc/rockchip_iommu.h"
#include "hw/sd/sd.h"
#include "hw/sd/dw_mmc.h"
#include "hw/sd/sdhci.h"
#include "hw/sd/rockchip_dwcmshc.h"
#include "hw/usb/dwc3-udc.h"
#include "hw/ssi/rockchip_spi.h"
#include "hw/watchdog/k230_wdt.h"
#include "qobject/qlist.h"
#include "qom/object.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "system/system.h"
#include "system/qtest.h"
#include "target/arm/gtimer.h"
#include "target/arm/internals.h"

#define TYPE_MICRODUCK_MACHINE MACHINE_TYPE_NAME("microduck-beta")
OBJECT_DECLARE_SIMPLE_TYPE(MicroDuckState, MICRODUCK_MACHINE)
#define RK3566_CPUS 4
#define RK3566_IRQS 320

struct MicroDuckState {
    MachineState parent_obj;
    CPUClusterState cluster;
    ARMCPU cpu[RK3566_CPUS];
    GICv3State gic;
    RK3588SCMIState *scmi;
    MemoryRegion ram;
    MemoryRegion debug;
    uint32_t debug_regs[0x10000 / 4];
    DeviceState *dma[2];
    DW8250State *uart[4];
    DeviceState *gpio[5];
    DeviceState *cru;
    DeviceState *spi[4];
    DeviceState *isp;
    MemoryRegion venc_mmu_window[2];
    MemoryRegion dma_alias[2];
    MemoryRegion qos[24];
    uint32_t qos_regs[24][8];
    struct arm_boot_info boot;
};

static uint64_t debug_read(void *opaque, hwaddr addr, unsigned size)
{
    MicroDuckState *s = opaque;
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
    MicroDuckState *s = opaque;
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
    MicroDuckState *s = opaque;
    SerialState *serial = &s->uart[index]->serial_mm.serial;
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->gic), 116 + index), level);
    if (s->dma[0]) {
        qemu_set_irq(qdev_get_gpio_in(s->dma[0], 2 * index),
                     !(serial->lsr & BIT(5)));
        qemu_set_irq(qdev_get_gpio_in(s->dma[0], 2 * index + 1),
                     !(serial->lsr & BIT(0)));
    }
}

static DeviceState *rk3566_device(MicroDuckState *s, const char *type,
                                const char *name)
{
    DeviceState *dev = qdev_new(type);
    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    object_unref(OBJECT(dev));
    return dev;
}

static void rk3566_map(MicroDuckState *s, DeviceState *dev, hwaddr address,
                      int irq)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, address);
    if (irq >= 0) {
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(&s->gic), irq));
    }
}

static bool rk3566_smc(ARMCPU *cpu)
{
    MicroDuckState *s = MICRODUCK_MACHINE(qdev_get_machine());
    CPUARMState *env = &cpu->env;
    switch ((uint32_t)env->xregs[0]) {
    case RK3588_SCMI_SMC_ID:
        rk3588_scmi_handle_smc(s->scmi);
        env->xregs[0] = 0;
        return true;
    case 0x82000009: /* Shared context page for the IRQ-mode debug console. */
    case 0xc2000005:
        env->xregs[0] = 0;
        env->xregs[1] = 0x110000;
        return true;
    default:
        return false;
    }
}

static void rk3566_cpus(MicroDuckState *s)
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

static void rk3566_storage(MicroDuckState *s)
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
    DriveInfo *drive = drive_get(IF_SD, 0, 0);
    if (drive) {
        DeviceState *card = qdev_new(TYPE_EMMC);
        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(drive),
                                &error_fatal);
        qdev_realize_and_unref(card, qdev_get_child_bus(host, "sd-bus"),
                               &error_fatal);
    }
}

static void rk3566_dma(MicroDuckState *s)
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

static void rk3566_boot_dtb(const struct arm_boot_info *info, void *fdt)
{
    const char *node = "/reserved-memory/scmi@10f000";
    qemu_fdt_add_subnode(fdt, node);
    qemu_fdt_setprop_cells(fdt, node, "reg", 0, 0x10f000, 0, 0x1000);
    qemu_fdt_setprop(fdt, node, "no-map", NULL, 0);
    qemu_fdt_setprop_cell(fdt, "/fiq-debugger", "rockchip,serial-id", 3);
    /* QEMU's PSCI firmware implements WFI retention, without power-down. */
    for (unsigned i = 0; i < RK3566_CPUS; i++) {
        g_autofree char *cpu = g_strdup_printf("/cpus/cpu@%x", i << 8);
        int offset = fdt_path_offset(fdt, cpu);
        if (offset >= 0) {
            fdt_delprop(fdt, offset, "cpu-idle-states");
        }
    }
    /* The control board configuration exposes the implemented peripherals. */
    const char *absent[] = {
        "/gpu@fde60000", "/gpu-mali@fde60000",
        "/video-codec@fdea0400", "/vdpu@fdea0400", "/jpegd@fded0000",
        "/vepu@fdee0000", "/iep@fdef0000", "/rkvdec@fdf80200",
        "/rkcif@fdfe0000",
        "/rkcif_dvp", "/rkcif_dvp_sditf", "/rkcif_mipi_lvds",
        "/rkcif_mipi_lvds_sditf",
        "/display-subsystem", "/mipi-csi2", "/mipi-csi2-hw@fdfb0000",
    };
    for (unsigned i = 0; i < ARRAY_SIZE(absent); i++) {
        if (fdt_path_offset(fdt, absent[i]) >= 0) {
            qemu_fdt_setprop_string(fdt, absent[i], "status", "disabled");
        }
    }
    node = "/virtio@ffe00000";
    qemu_fdt_add_subnode(fdt, node);
    qemu_fdt_setprop_string(fdt, node, "compatible", "virtio,mmio");
    qemu_fdt_setprop_cells(fdt, node, "reg", 0, 0xffe00000, 0, 0x200);
    qemu_fdt_setprop_cells(fdt, node, "interrupts", 0, 257, 4);
    qemu_fdt_setprop(fdt, node, "dma-coherent", NULL, 0);
}

static void microduck_init(MachineState *ms)
{
    MicroDuckState *s = MICRODUCK_MACHINE(ms);
    if (!ms->kernel_filename && !qtest_enabled()) {
        error_report("microduck-beta requires a kernel or firmware ELF "
                     "via -kernel");
        exit(EXIT_FAILURE);
    }
    bool media = object_class_by_name("rk3566-isp") &&
                 object_class_by_name("rk3566-csi-dphy") &&
                 object_class_by_name("rk3566-rga2") &&
                 object_class_by_name("rk3566-rkvenc");

    if (!media && !qtest_enabled()) {
        error_report("microduck-beta requires the RK3566 media devices; "
                     "install libavcodec, libavutil, libswscale and x264");
        exit(EXIT_FAILURE);
    }
    memory_region_init_alias(&s->ram, OBJECT(s), "rk3566.ram", ms->ram, 0,
                             ms->ram_size);
    memory_region_add_subregion(get_system_memory(), 0, &s->ram);
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
    DeviceState *pmucru = rk3566_device(s, "rk3566-cru", "pmucru");
    qdev_prop_set_bit(pmucru, "pmu", true);
    rk3566_map(s, pmucru, 0xfdd00000, -1);
    const hwaddr uart_base[] = { 0xfdd50000, 0xfe650000,
                                 0xfe660000, 0xfe670000 };
    const int uart_irqs[] = { 116, 117, 118, 119 };
    for (unsigned i = 0; i < ARRAY_SIZE(uart_base); i++) {
        g_autofree char *name = g_strdup_printf("uart%u", i);
        DeviceState *uart = rk3566_device(s, TYPE_DW8250, name);
        s->uart[i] = DW8250(uart);
        qdev_prop_set_uint32(uart, "baudbase", 24000000 / 16);
        qdev_prop_set_uint32(uart, "fifo-size", 64);
        if (i > 0) {
            g_autofree char *clock_name = g_strdup_printf("sclk-uart%u", i);

            qdev_connect_clock_in(uart, "uartclk",
                                  qdev_get_clock_out(s->cru, clock_name));
        }
        qdev_prop_set_chr(uart, "chardev",
                          serial_hd(i == 2 ? 0 : i == 3 ? 1 : i + 2));
        rk3566_map(s, uart, uart_base[i], uart_irqs[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(uart), 0,
                           qemu_allocate_irq(uart_irq, s, i));
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
    const hwaddr i2c[] = { 0xfdd40000, 0xfe5a0000, 0xfe5b0000, 0xfe5c0000 };
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
        qdev_prop_set_chr(s->isp, "chardev", qemu_chr_find("camera"));
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
    for (unsigned i = 0; i < ARRAY_SIZE(i2c); i++) {
        g_autofree char *name = g_strdup_printf("i2c%u", i);
        DeviceState *host = rk3566_device(s, TYPE_RK3X_I2C, name);
        rk3566_map(s, host, i2c[i], 46 + i);
        I2CBus *bus = I2C_BUS(qdev_get_child_bus(host, "i2c-bus"));
        if (i == 0 || i == 3) {
            I2CSlave *dev = i2c_slave_new("microduck-register-i2c", 0x20);
            qdev_prop_set_uint32(DEVICE(dev), "kind", i == 0 ? 0 : 2);
            i2c_slave_realize_and_unref(dev, bus, &error_fatal);
            qdev_connect_gpio_out_named(DEVICE(dev), "nirq", 0,
                qdev_get_gpio_in(s->gpio[i == 0 ? 0 : 4], i == 0 ? 3 : 23));
        }
        if (i == 0) {
            I2CSlave *dev = i2c_slave_new("microduck-register-i2c", 0x41);
            qdev_prop_set_uint32(DEVICE(dev), "kind", 1);
            i2c_slave_realize_and_unref(dev, bus, &error_fatal);
        }
        if (i == 3) {
            I2CSlave *imu = i2c_slave_new("microduck-lsm6dsv16x", 0x6a);
            qdev_prop_set_chr(DEVICE(imu), "chardev",
                              qemu_chr_find("head-imu"));
            i2c_slave_realize_and_unref(imu, bus, &error_fatal);
        }
        if (i == 2 && media) {
            I2CSlave *camera = i2c_slave_new("microduck-gc2093", 0x37);
            i2c_slave_realize_and_unref(camera, bus, &error_fatal);
            qdev_connect_gpio_out_named(DEVICE(camera), "streaming", 0,
                qdev_get_gpio_in_named(s->isp, "sensor-stream", 0));
            qdev_connect_gpio_out_named(s->gpio[3], "gpio-out", 22,
                qdev_get_gpio_in_named(DEVICE(camera), "power", 0));
            qdev_connect_gpio_out_named(s->gpio[4], "gpio-out", 17,
                qdev_get_gpio_in_named(DEVICE(camera), "resetn", 0));
        }
    }
    DeviceState *scmi = rk3566_device(s, TYPE_RK3588_SCMI, "scmi");
    qdev_prop_set_string(scmi, "soc", "rk3566");
    s->scmi = RK3588_SCMI(scmi);
    sysbus_realize(SYS_BUS_DEVICE(scmi), &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(), 0x10f000,
                                        sysbus_mmio_get_region(
                                            SYS_BUS_DEVICE(scmi), 0), 1);
    arm_register_psci_smc_handler(rk3566_smc);
    rk3566_storage(s);
    rk3566_map(s, rk3566_device(s, TYPE_DW_MMC, "sdio"), 0xfe2c0000, 99);
    rk3566_map(s, rk3566_device(s, "virtio-mmio", "management"),
               0xffe00000, 257);
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
    DeviceState *nfc = ssi_create_peripheral(ROCKCHIP_SPI(s->spi[1])->spi,
                                             "microduck-st25r100");
    qdev_connect_gpio_out_named(s->gpio[3], "gpio-out", 1,
                                qdev_get_gpio_in_named(nfc, SSI_GPIO_CS, 0));
    qdev_connect_gpio_out_named(s->gpio[3], "gpio-out", 21,
                                qdev_get_gpio_in_named(nfc, "reset", 0));
    qdev_connect_gpio_out_named(nfc, "irq", 0,
                                qdev_get_gpio_in(s->gpio[3], 20));
    DeviceState *tof = qdev_new("microduck-vl53l8cx");
    qdev_prop_set_chr(tof, "chardev", qemu_chr_find("tof-data"));
    ssi_realize_and_unref(tof, ROCKCHIP_SPI(s->spi[3])->spi, &error_fatal);
    qdev_connect_gpio_out_named(s->spi[3], "cs", 0,
                                qdev_get_gpio_in_named(tof, SSI_GPIO_CS, 0));
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
    DeviceState *usb = rk3566_device(s, TYPE_DWC3_UDC, "usb-otg");
    qdev_prop_set_chr(usb, "chardev", serial_hd(4));
    qdev_prop_set_bit(usb, "host-connected", false);
    rk3566_map(s, usb, 0xfcc00000, 169);
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
    if (!ms->kernel_filename) {
        return;
    }
    s->boot.ram_size = ms->ram_size;
    s->boot.loader_start = 0;
    s->boot.board_id = -1;
    s->boot.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    s->boot.modify_dtb = rk3566_boot_dtb;
    arm_load_kernel(&s->cpu[0], ms, &s->boot);
}

static void microduck_get_virtual_clock(Object *obj, Visitor *v,
                                       const char *name, void *opaque,
                                       Error **errp)
{
    uint64_t value = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    visit_type_uint64(v, name, &value, errp);
}

static void microduck_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Pollen MicroDuck beta (Rockchip RK3566)";
    mc->init = microduck_init;
    mc->default_ram_size = GiB;
    mc->default_ram_id = "microduck.ram";
    mc->min_cpus = mc->default_cpus = mc->max_cpus = RK3566_CPUS;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a55");
    object_class_property_add(oc, "virtual-clock-ns", "uint64",
                              microduck_get_virtual_clock, NULL, NULL, NULL);
}

static const TypeInfo microduck_info = {
    .name = TYPE_MICRODUCK_MACHINE,
    .parent = TYPE_MACHINE,
    .interfaces = aarch64_machine_interfaces,
    .instance_size = sizeof(MicroDuckState),
    .class_init = microduck_class_init,
};

static void microduck_register(void)
{
    type_register_static(&microduck_info);
}
type_init(microduck_register)
