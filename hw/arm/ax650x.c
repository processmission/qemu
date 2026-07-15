/*
 * Axera AX650X AI Pyramid machine
 *
 * Copyright (c) 2026 Zevorn
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/ax650x-dwmac.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/fdt.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/serial-mm.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/intc/arm_gic.h"
#include "hw/sd/ax650x-sdhci.h"
#include "hw/sd/sd.h"
#include "system/address-spaces.h"
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"

#define TYPE_AX650X_PYRAMID_MACHINE MACHINE_TYPE_NAME("ax650x-pyramid")
OBJECT_DECLARE_SIMPLE_TYPE(AX650XPyramidState, AX650X_PYRAMID_MACHINE)

#define AX650X_NUM_CPUS              8
#define AX650X_NUM_IRQS              256
#define AX650X_NUM_SPIS              (AX650X_NUM_IRQS - GIC_INTERNAL)

#define AX650X_RAM_BASE              0x100000000ULL
#define AX650X_RAM_SIZE              (2 * GiB)

#define AX650X_GIC_DIST_BASE         0x04901000
#define AX650X_GIC_DIST_SIZE         0x1000
#define AX650X_GIC_CPU_BASE          0x04902000
#define AX650X_GIC_CPU_SIZE          0x1000
#define AX650X_GIC_HYP_BASE          0x04904000
#define AX650X_GIC_HYP_SIZE          0x2000
#define AX650X_GIC_VCPU_BASE         0x04906000
#define AX650X_GIC_VCPU_SIZE         0x2000

#define AX650X_UART0_BASE            0x02016000
#define AX650X_UART0_SIZE            0x400
#define AX650X_UART_STD_SIZE         0x20
#define AX650X_UART_USR              0x7c
#define AX650X_UART_UCV              0xf8
#define AX650X_UART_USR_TFNF         BIT(1)
#define AX650X_UART_USR_TFE          BIT(2)
#define AX650X_UART0_IRQ             135
#define AX650X_UART_CLOCK_HZ         200000000
#define AX650X_UART_BAUDBASE         (AX650X_UART_CLOCK_HZ / 16)

#define AX650X_EMMC_BASE             0x28000000
#define AX650X_EMMC_SIZE             0x600
#define AX650X_EMMC_IRQ              93
#define AX650X_EMMC_CLOCK_HZ         200000000

#define AX650X_TIMER_CLOCK_HZ        24000000
#define AX650X_PMU_IRQ_BASE          80
#define AX650X_GIC_MAINT_PPI         9
#define AX650X_GIC_PPI_FLAGS         0xff04

struct AX650XPyramidState {
    MachineState parent_obj;

    ARMCPU *cpus[AX650X_NUM_CPUS];
    DeviceState *gic;
    MemoryRegion gic_cpu_alias;
    MemoryRegion uart0_ext;
    struct arm_boot_info bootinfo;
    void *fdt;
    int fdt_size;
};

static uint64_t ax650x_uart_ext_read(void *opaque, hwaddr offset,
                                     unsigned int size)
{
    hwaddr reg = AX650X_UART_STD_SIZE + offset;

    switch (reg) {
    case AX650X_UART_USR:
        return AX650X_UART_USR_TFNF | AX650X_UART_USR_TFE;
    case AX650X_UART_UCV:
        /*
         * The hardware component revision is not documented.  Zero selects
         * the AXERA driver's defined fallback for a UART without DesignWare
         * additional features.
         */
        return 0;
    default:
        return 0;
    }
}

static void ax650x_uart_ext_write(void *opaque, hwaddr offset,
                                  uint64_t value, unsigned int size)
{
}

static const MemoryRegionOps ax650x_uart_ext_ops = {
    .read = ax650x_uart_ext_read,
    .write = ax650x_uart_ext_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static uint64_t ax650x_cpu_mpidr(unsigned int cpu)
{
    return (uint64_t)cpu << 8;
}

static void ax650x_create_cpus(AX650XPyramidState *s)
{
    MachineState *machine = MACHINE(s);
    MemoryRegion *sysmem = get_system_memory();

    for (unsigned int i = 0; i < machine->smp.cpus; i++) {
        Object *cpuobj = object_new(machine->cpu_type);
        CPUState *cs = CPU(cpuobj);

        cs->cpu_index = i;
        object_property_set_int(cpuobj, "mp-affinity",
                                ax650x_cpu_mpidr(i), &error_abort);
        object_property_set_int(cpuobj, "cntfrq", AX650X_TIMER_CLOCK_HZ,
                                &error_abort);
        object_property_set_bool(cpuobj, "has_el3", false, &error_abort);
        object_property_set_link(cpuobj, "memory", OBJECT(sysmem),
                                 &error_abort);
        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        s->cpus[i] = ARM_CPU(cpuobj);
        object_unref(cpuobj);
    }
}

static void ax650x_create_gic(AX650XPyramidState *s)
{
    MachineState *machine = MACHINE(s);
    MemoryRegion *sysmem = get_system_memory();
    SysBusDevice *gicbus;

    s->gic = qdev_new(gic_class_name());
    qdev_prop_set_uint32(s->gic, "revision", 2);
    qdev_prop_set_uint32(s->gic, "num-cpu", machine->smp.cpus);
    qdev_prop_set_uint32(s->gic, "num-irq", AX650X_NUM_IRQS);
    qdev_prop_set_bit(s->gic, "has-security-extensions", false);
    qdev_prop_set_bit(s->gic, "has-virtualization-extensions", true);

    gicbus = SYS_BUS_DEVICE(s->gic);
    sysbus_realize_and_unref(gicbus, &error_fatal);
    sysbus_mmio_map(gicbus, 0, AX650X_GIC_DIST_BASE);

    /*
     * QEMU exposes a 0x2000-byte GICC region for GICv2, while AX650X
     * decodes only the architected 0x1000-byte CPU interface window.
     */
    memory_region_init_alias(&s->gic_cpu_alias, OBJECT(s),
                             "ax650x.gic-cpu",
                             sysbus_mmio_get_region(gicbus, 1),
                             0, AX650X_GIC_CPU_SIZE);
    memory_region_add_subregion(sysmem, AX650X_GIC_CPU_BASE,
                                &s->gic_cpu_alias);
    sysbus_mmio_map(gicbus, 2, AX650X_GIC_HYP_BASE);
    sysbus_mmio_map(gicbus, 3, AX650X_GIC_VCPU_BASE);

    for (unsigned int i = 0; i < machine->smp.cpus; i++) {
        DeviceState *cpu = DEVICE(s->cpus[i]);
        int ppi_base = AX650X_NUM_SPIS + i * GIC_INTERNAL;

        qdev_connect_gpio_out(cpu, GTIMER_PHYS,
                              qdev_get_gpio_in(s->gic, ppi_base +
                                               ARCH_TIMER_NS_EL1_IRQ));
        qdev_connect_gpio_out(cpu, GTIMER_VIRT,
                              qdev_get_gpio_in(s->gic, ppi_base +
                                               ARCH_TIMER_VIRT_IRQ));
        qdev_connect_gpio_out(cpu, GTIMER_HYP,
                              qdev_get_gpio_in(s->gic, ppi_base +
                                               ARCH_TIMER_NS_EL2_IRQ));
        qdev_connect_gpio_out(cpu, GTIMER_SEC,
                              qdev_get_gpio_in(s->gic, ppi_base +
                                               ARCH_TIMER_S_EL1_IRQ));

        sysbus_connect_irq(gicbus, i,
                           qdev_get_gpio_in(cpu, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbus, i + machine->smp.cpus,
                           qdev_get_gpio_in(cpu, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbus, i + 2 * machine->smp.cpus,
                           qdev_get_gpio_in(cpu, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbus, i + 3 * machine->smp.cpus,
                           qdev_get_gpio_in(cpu, ARM_CPU_VFIQ));
        sysbus_connect_irq(gicbus, i + 4 * machine->smp.cpus,
                           qdev_get_gpio_in(s->gic,
                                            ppi_base + AX650X_GIC_MAINT_PPI));

        qdev_connect_gpio_out_named(cpu, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(s->gic,
                                                     AX650X_PMU_IRQ_BASE + i));
    }
}

static void ax650x_create_emmc(AX650XPyramidState *s)
{
    DeviceState *host = qdev_new(TYPE_AX650X_SDHCI);
    SysBusDevice *sbd = SYS_BUS_DEVICE(host);
    DriveInfo *dinfo = drive_get(IF_SD, 0, 0);

    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, AX650X_EMMC_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(s->gic, AX650X_EMMC_IRQ));

    if (dinfo) {
        DeviceState *card = qdev_new(TYPE_EMMC);

        qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo),
                                &error_fatal);
        qdev_realize_and_unref(card,
                               BUS(ax650x_sdhci_get_bus(AX650X_SDHCI(host))),
                               &error_fatal);
    }
}

static void ax650x_create_fdt(AX650XPyramidState *s)
{
    MachineState *machine = MACHINE(s);
    uint32_t cpu_phandles[AX650X_NUM_CPUS];
    uint32_t gic_phandle;
    uint32_t uart_baud_phandle;
    uint32_t uart_apb_phandle;
    uint32_t emmc_clock_phandle;
    const char clock_names[] = "baudclk\0apb_pclk";
    const char emmc_clock_names[] = "aclk\0hclk\0cardclk";
    const char uart_path[] = "/soc/ax_uart@2016000";
    const char emmc_path[] = "/soc/sdhc@28000000";

    s->fdt = create_device_tree(&s->fdt_size);
    if (!s->fdt) {
        error_report("ax650x-pyramid: create_device_tree() failed");
        exit(EXIT_FAILURE);
    }

    qemu_fdt_setprop_string(s->fdt, "/", "compatible", "axera,ax650x");
    qemu_fdt_setprop_string(s->fdt, "/", "model",
                            "M5Stack AI Pyramid (AX650X)");
    qemu_fdt_setprop_cell(s->fdt, "/", "#address-cells", 2);
    qemu_fdt_setprop_cell(s->fdt, "/", "#size-cells", 2);

    qemu_fdt_add_subnode(s->fdt, "/aliases");
    qemu_fdt_add_subnode(s->fdt, "/chosen");
    qemu_fdt_setprop_string(s->fdt, "/aliases", "serial0", uart_path);
    qemu_fdt_setprop_string(s->fdt, "/chosen", "stdout-path", uart_path);

    qemu_fdt_add_subnode(s->fdt, "/cpus");
    qemu_fdt_setprop_cell(s->fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(s->fdt, "/cpus", "#size-cells", 0);
    for (int i = machine->smp.cpus - 1; i >= 0; i--) {
        g_autofree char *node = g_strdup_printf("/cpus/cpu@%x",
                                                i << 8);

        qemu_fdt_add_subnode(s->fdt, node);
        qemu_fdt_setprop_string(s->fdt, node, "device_type", "cpu");
        qemu_fdt_setprop_string(s->fdt, node, "compatible",
                                "arm,cortex-a55");
        qemu_fdt_setprop_cell(s->fdt, node, "reg", ax650x_cpu_mpidr(i));
        qemu_fdt_setprop_string(s->fdt, node, "enable-method", "psci");
        cpu_phandles[i] = qemu_fdt_alloc_phandle(s->fdt);
        qemu_fdt_setprop_cell(s->fdt, node, "phandle", cpu_phandles[i]);
    }

    qemu_fdt_add_subnode(s->fdt, "/psci");
    qemu_fdt_setprop_string(s->fdt, "/psci", "compatible", "arm,psci-1.0");
    qemu_fdt_setprop_string(s->fdt, "/psci", "method", "smc");

    qemu_fdt_add_subnode(s->fdt, "/timer");
    qemu_fdt_setprop_string(s->fdt, "/timer", "compatible",
                            "arm,armv8-timer");
    qemu_fdt_setprop_cells(s->fdt, "/timer", "interrupts",
                           GIC_FDT_IRQ_TYPE_PPI,
                           INTID_TO_PPI(ARCH_TIMER_S_EL1_IRQ),
                           AX650X_GIC_PPI_FLAGS,
                           GIC_FDT_IRQ_TYPE_PPI,
                           INTID_TO_PPI(ARCH_TIMER_NS_EL1_IRQ),
                           AX650X_GIC_PPI_FLAGS,
                           GIC_FDT_IRQ_TYPE_PPI,
                           INTID_TO_PPI(ARCH_TIMER_VIRT_IRQ),
                           AX650X_GIC_PPI_FLAGS,
                           GIC_FDT_IRQ_TYPE_PPI,
                           INTID_TO_PPI(ARCH_TIMER_NS_EL2_IRQ),
                           AX650X_GIC_PPI_FLAGS);
    qemu_fdt_setprop_cell(s->fdt, "/timer", "clock-frequency",
                          AX650X_TIMER_CLOCK_HZ);

    qemu_fdt_add_subnode(s->fdt, "/interrupt-controller@4900000");
    qemu_fdt_setprop_string(s->fdt, "/interrupt-controller@4900000",
                            "compatible", "arm,gic-400");
    qemu_fdt_setprop_cell(s->fdt, "/interrupt-controller@4900000",
                          "#interrupt-cells", 3);
    qemu_fdt_setprop_cell(s->fdt, "/interrupt-controller@4900000",
                          "#address-cells", 2);
    qemu_fdt_setprop_cell(s->fdt, "/interrupt-controller@4900000",
                          "#size-cells", 2);
    qemu_fdt_setprop(s->fdt, "/interrupt-controller@4900000",
                     "interrupt-controller", NULL, 0);
    qemu_fdt_setprop(s->fdt, "/interrupt-controller@4900000", "ranges",
                     NULL, 0);
    qemu_fdt_setprop_sized_cells(s->fdt, "/interrupt-controller@4900000",
                                 "reg",
                                 2, AX650X_GIC_DIST_BASE,
                                 2, AX650X_GIC_DIST_SIZE,
                                 2, AX650X_GIC_CPU_BASE,
                                 2, AX650X_GIC_CPU_SIZE,
                                 2, AX650X_GIC_HYP_BASE,
                                 2, AX650X_GIC_HYP_SIZE,
                                 2, AX650X_GIC_VCPU_BASE,
                                 2, AX650X_GIC_VCPU_SIZE);
    qemu_fdt_setprop_cells(s->fdt, "/interrupt-controller@4900000",
                           "interrupts", GIC_FDT_IRQ_TYPE_PPI,
                           AX650X_GIC_MAINT_PPI, AX650X_GIC_PPI_FLAGS);
    gic_phandle = qemu_fdt_alloc_phandle(s->fdt);
    qemu_fdt_setprop_cell(s->fdt, "/interrupt-controller@4900000",
                          "phandle", gic_phandle);
    qemu_fdt_setprop_cell(s->fdt, "/", "interrupt-parent", gic_phandle);

    qemu_fdt_add_subnode(s->fdt, "/pmu");
    qemu_fdt_setprop_string(s->fdt, "/pmu", "compatible",
                            "arm,cortex-a55-pmu");
    qemu_fdt_setprop_cells(s->fdt, "/pmu", "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, 80,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, 81,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, 82,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, 83,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, 84,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, 85,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, 86,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                           GIC_FDT_IRQ_TYPE_SPI, 87,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cells(s->fdt, "/pmu", "interrupt-affinity",
                           cpu_phandles[0], cpu_phandles[1],
                           cpu_phandles[2], cpu_phandles[3],
                           cpu_phandles[4], cpu_phandles[5],
                           cpu_phandles[6], cpu_phandles[7]);

    qemu_fdt_add_subnode(s->fdt, "/uart-baud-clock");
    qemu_fdt_setprop_string(s->fdt, "/uart-baud-clock", "compatible",
                            "fixed-clock");
    qemu_fdt_setprop_cell(s->fdt, "/uart-baud-clock", "#clock-cells", 0);
    qemu_fdt_setprop_cell(s->fdt, "/uart-baud-clock", "clock-frequency",
                          AX650X_UART_CLOCK_HZ);
    uart_baud_phandle = qemu_fdt_alloc_phandle(s->fdt);
    qemu_fdt_setprop_cell(s->fdt, "/uart-baud-clock", "phandle",
                          uart_baud_phandle);

    qemu_fdt_add_subnode(s->fdt, "/uart-apb-clock");
    qemu_fdt_setprop_string(s->fdt, "/uart-apb-clock", "compatible",
                            "fixed-clock");
    qemu_fdt_setprop_cell(s->fdt, "/uart-apb-clock", "#clock-cells", 0);
    qemu_fdt_setprop_cell(s->fdt, "/uart-apb-clock", "clock-frequency",
                          AX650X_UART_CLOCK_HZ);
    uart_apb_phandle = qemu_fdt_alloc_phandle(s->fdt);
    qemu_fdt_setprop_cell(s->fdt, "/uart-apb-clock", "phandle",
                          uart_apb_phandle);

    qemu_fdt_add_subnode(s->fdt, "/emmc-clock");
    qemu_fdt_setprop_string(s->fdt, "/emmc-clock", "compatible",
                            "fixed-clock");
    qemu_fdt_setprop_cell(s->fdt, "/emmc-clock", "#clock-cells", 0);
    qemu_fdt_setprop_cell(s->fdt, "/emmc-clock", "clock-frequency",
                          AX650X_EMMC_CLOCK_HZ);
    emmc_clock_phandle = qemu_fdt_alloc_phandle(s->fdt);
    qemu_fdt_setprop_cell(s->fdt, "/emmc-clock", "phandle",
                          emmc_clock_phandle);

    qemu_fdt_add_subnode(s->fdt, "/soc");
    qemu_fdt_setprop_string(s->fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(s->fdt, "/soc", "#address-cells", 2);
    qemu_fdt_setprop_cell(s->fdt, "/soc", "#size-cells", 2);
    qemu_fdt_setprop(s->fdt, "/soc", "ranges", NULL, 0);
    ax650x_dwmac_create_fdt(s->fdt);

    qemu_fdt_add_subnode(s->fdt, uart_path);
    qemu_fdt_setprop_string(s->fdt, uart_path, "compatible",
                            "axera,ax-apb-uart");
    qemu_fdt_setprop_sized_cells(s->fdt, uart_path, "reg",
                                 2, AX650X_UART0_BASE,
                                 2, AX650X_UART0_SIZE);
    qemu_fdt_setprop_cell(s->fdt, uart_path, "reg-shift", 2);
    qemu_fdt_setprop_cell(s->fdt, uart_path, "reg-io-width", 4);
    qemu_fdt_setprop_cells(s->fdt, uart_path, "clocks",
                           uart_baud_phandle, uart_apb_phandle);
    qemu_fdt_setprop(s->fdt, uart_path, "clock-names", clock_names,
                     sizeof(clock_names));
    qemu_fdt_setprop_cell(s->fdt, uart_path, "default_cpr_reg", 0x425f2);
    qemu_fdt_setprop_cells(s->fdt, uart_path, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, AX650X_UART0_IRQ,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_string(s->fdt, uart_path, "status", "okay");

    qemu_fdt_add_subnode(s->fdt, emmc_path);
    qemu_fdt_setprop_string(s->fdt, emmc_path, "compatible",
                            "axera,sdhc-ax650");
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "#address-cells", 2);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "#size-cells", 2);
    qemu_fdt_setprop_sized_cells(s->fdt, emmc_path, "reg",
                                 2, AX650X_EMMC_BASE,
                                 2, AX650X_EMMC_SIZE);
    qemu_fdt_setprop_cells(s->fdt, emmc_path, "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, AX650X_EMMC_IRQ,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cells(s->fdt, emmc_path, "clocks",
                           emmc_clock_phandle, emmc_clock_phandle,
                           emmc_clock_phandle);
    qemu_fdt_setprop(s->fdt, emmc_path, "clock-names",
                     emmc_clock_names, sizeof(emmc_clock_names));
    qemu_fdt_setprop_cells(s->fdt, emmc_path, "sdhci-caps-mask",
                           0x2, 0x03200000);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "bus-width", 8);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "max-frequency",
                          AX650X_EMMC_CLOCK_HZ);
    qemu_fdt_setprop(s->fdt, emmc_path, "cap-mmc-hw-reset", NULL, 0);
    qemu_fdt_setprop(s->fdt, emmc_path, "cap-mmc-highspeed", NULL, 0);
    qemu_fdt_setprop(s->fdt, emmc_path, "mmc-hs200-1_8v", NULL, 0);
    qemu_fdt_setprop(s->fdt, emmc_path, "mmc-hs400-1_8v", NULL, 0);
    qemu_fdt_setprop(s->fdt, emmc_path, "mmc-hs400-enhanced-strobe",
                     NULL, 0);
    qemu_fdt_setprop(s->fdt, emmc_path, "no-sdio", NULL, 0);
    qemu_fdt_setprop(s->fdt, emmc_path, "no-sd", NULL, 0);
    qemu_fdt_setprop(s->fdt, emmc_path, "non-removable", NULL, 0);
    qemu_fdt_setprop(s->fdt, emmc_path, "disable-wp", NULL, 0);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "axera,phy-cnfg", 0x00cc0000);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "axera,phy-cmdpad-cnfg",
                          0x0449);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "axera,phy-datapad-cnfg",
                          0x0449);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "axera,phy-clkpad-cnfg",
                          0x0440);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "axera,phy-stbpad-cnfg",
                          0x0451);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "axera,phy-rstnpad-cnfg",
                          0x0449);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "axera,phy-commdl-cnfg", 0);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "axera,phy-sdclkdl-cnfg", 1);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "axera,phy-sdclkdl-dc", 0x7f);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "axera,phy-smpldl-cnfg", 0xc);
    qemu_fdt_setprop_cell(s->fdt, emmc_path, "axera,phy-atdl-cnfg", 0xc);
    qemu_fdt_setprop_string(s->fdt, emmc_path, "status", "okay");
}

static void *ax650x_get_dtb(const struct arm_boot_info *binfo, int *fdt_size)
{
    AX650XPyramidState *s = container_of(binfo, AX650XPyramidState,
                                         bootinfo);

    *fdt_size = s->fdt_size;
    return s->fdt;
}

static void ax650x_pyramid_init(MachineState *machine)
{
    AX650XPyramidState *s = AX650X_PYRAMID_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();

    if (machine->ram_size != AX650X_RAM_SIZE) {
        error_report("ax650x-pyramid: RAM size must be exactly 2 GiB");
        exit(EXIT_FAILURE);
    }

    memory_region_add_subregion(sysmem, AX650X_RAM_BASE, machine->ram);
    ax650x_create_cpus(s);
    ax650x_create_gic(s);
    ax650x_create_emmc(s);
    ax650x_dwmac_create(s->gic);

    serial_mm_init(sysmem, AX650X_UART0_BASE, 2,
                   qdev_get_gpio_in(s->gic, AX650X_UART0_IRQ),
                   AX650X_UART_BAUDBASE, serial_hd(0), DEVICE_LITTLE_ENDIAN);
    memory_region_init_io(&s->uart0_ext, OBJECT(s), &ax650x_uart_ext_ops, s,
                          "ax650x.uart0-ext",
                          AX650X_UART0_SIZE - AX650X_UART_STD_SIZE);
    memory_region_add_subregion(sysmem,
                                AX650X_UART0_BASE + AX650X_UART_STD_SIZE,
                                &s->uart0_ext);

    ax650x_create_fdt(s);

    s->bootinfo.ram_size = machine->ram_size;
    s->bootinfo.board_id = -1;
    s->bootinfo.loader_start = AX650X_RAM_BASE;
    s->bootinfo.get_dtb = ax650x_get_dtb;
    s->bootinfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    arm_load_kernel(s->cpus[0], machine, &s->bootinfo);
}

static void ax650x_pyramid_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a55"),
        NULL,
    };

    mc->desc = "M5Stack AI Pyramid (Axera AX650X)";
    mc->init = ax650x_pyramid_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a55");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_cpus = AX650X_NUM_CPUS;
    mc->min_cpus = AX650X_NUM_CPUS;
    mc->max_cpus = AX650X_NUM_CPUS;
    mc->default_ram_size = AX650X_RAM_SIZE;
    mc->default_ram_id = "ax650x.ram";
    mc->no_cdrom = true;
    mc->no_floppy = true;
}

static const TypeInfo ax650x_pyramid_info = {
    .name = TYPE_AX650X_PYRAMID_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(AX650XPyramidState),
    .class_init = ax650x_pyramid_class_init,
    .interfaces = aarch64_machine_interfaces,
};

static void ax650x_pyramid_machine_register_types(void)
{
    type_register_static(&ax650x_pyramid_info);
}

type_init(ax650x_pyramid_machine_register_types)
