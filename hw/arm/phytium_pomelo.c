/*
 * Phytium D2000 Pomelo board model.
 *
 * The machine layout follows the upstream U-Boot Pomelo port at:
 *   https://github.com/u-boot/u-boot/tree/211de43d0f954a00a490220c1aac9db298287c40/board/phytium/pomelo
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
#include "system/kvm.h"
#include "system/numa.h"
#include "system/qtest.h"
#include "system/system.h"
#include "exec/hwaddr.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/fdt.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/phytium_pomelo.h"
#include "hw/char/pl011.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/intc/arm_gicv3_its_common.h"
#include "hw/misc/unimp.h"
#include "hw/net/dwmac4.h"
#include "hw/pci/pci.h"
#include "hw/pci-host/gpex.h"
#include "net/net.h"
#include "qobject/qlist.h"
#include "qom/object.h"
#include "target/arm/cpu.h"
#include "target/arm/cpregs.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "target/arm/internals.h"

#define PHYTIUM_POMELO_NUM_CPUS       8
#define PHYTIUM_POMELO_NUM_IRQS       256
#define PHYTIUM_POMELO_NUM_UARTS      4
#define PHYTIUM_POMELO_NUM_GMACS      2
#define PHYTIUM_POMELO_GTIMER_HZ     50000000
#define PHYTIUM_POMELO_UBOOT_BASE     0x00180000ULL
#define PHYTIUM_POMELO_UBOOT_MAX_SIZE 0x02000000ULL

/* Vendor SMCCC services used by the upstream Pomelo U-Boot board code. */
#define PHYTIUM_POMELO_SMC_GET_RST_SOURCE 0xc2000f01
#define PHYTIUM_POMELO_SMC_INIT_PLL       0xc2000f02
#define PHYTIUM_POMELO_SMC_INIT_PCIE      0xc2000f03
#define PHYTIUM_POMELO_SMC_INIT_MEM       0xc2000f04
#define PHYTIUM_POMELO_SMC_INIT_SEC       0xc2000f05

enum {
    PHYTIUM_POMELO_RAM_LOW,
    PHYTIUM_POMELO_RAM_HIGH,
    PHYTIUM_POMELO_GIC_DIST,
    PHYTIUM_POMELO_GIC_ITS,
    PHYTIUM_POMELO_GIC_REDIST,
    PHYTIUM_POMELO_UART0,
    PHYTIUM_POMELO_UART1,
    PHYTIUM_POMELO_UART2,
    PHYTIUM_POMELO_UART3,
    PHYTIUM_POMELO_PCIE_ECAM,
    PHYTIUM_POMELO_PCIE_PIO,
    PHYTIUM_POMELO_PCIE_MMIO,
    PHYTIUM_POMELO_PCIE_MMIO_HIGH,
    PHYTIUM_POMELO_GMAC0,
    PHYTIUM_POMELO_GMAC1,
    PHYTIUM_POMELO_LOW_PERIPH,
};

static const MemMapEntry phytium_pomelo_memmap[] = {
    [PHYTIUM_POMELO_RAM_LOW] = { 0x80000000ULL, 0x7b000000ULL },
    [PHYTIUM_POMELO_RAM_HIGH] = { 0x2000000000ULL, 0x80000000ULL },
    [PHYTIUM_POMELO_GIC_DIST] = { 0x29a00000ULL, 0x00010000ULL },
    [PHYTIUM_POMELO_GIC_ITS] = { 0x29a20000ULL, 0x00010000ULL },
    [PHYTIUM_POMELO_GIC_REDIST] = { 0x29b00000ULL, 0x00100000ULL },
    [PHYTIUM_POMELO_UART0] = { 0x28001000ULL, 0x00001000ULL },
    [PHYTIUM_POMELO_UART1] = { 0x28000000ULL, 0x00001000ULL },
    [PHYTIUM_POMELO_UART2] = { 0x28002000ULL, 0x00001000ULL },
    [PHYTIUM_POMELO_UART3] = { 0x28003000ULL, 0x00001000ULL },
    [PHYTIUM_POMELO_PCIE_ECAM] = { 0x40000000ULL, 0x10000000ULL },
    [PHYTIUM_POMELO_PCIE_PIO] = { 0x50000000ULL, 0x00f00000ULL },
    [PHYTIUM_POMELO_PCIE_MMIO] = { 0x58000000ULL, 0x28000000ULL },
    [PHYTIUM_POMELO_PCIE_MMIO_HIGH] = { 0x1000000000ULL,
                                       0x1000000000ULL },
    [PHYTIUM_POMELO_GMAC0] = { 0x2820c000ULL, 0x00002000ULL },
    [PHYTIUM_POMELO_GMAC1] = { 0x28210000ULL, 0x00002000ULL },
    [PHYTIUM_POMELO_LOW_PERIPH] = { 0x28000000ULL, 0x01000000ULL },
};

struct PhytiumPomeloMachineState {
    MachineState parent_obj;
    struct arm_boot_info bootinfo;
    ARMCPU *cpu[PHYTIUM_POMELO_NUM_CPUS];
    DeviceState *gic;
    MemoryRegion ram_low;
    MemoryRegion ram_high;
    void *dtb;
    int dtb_size;
};

static uint64_t phytium_pomelo_cpu_mpidr(unsigned int index)
{
    g_assert(index < PHYTIUM_POMELO_NUM_CPUS);
    return ((uint64_t)(index / 2) << 8) | (index % 2);
}

static void phytium_pomelo_create_cpus(PhytiumPomeloMachineState *s,
                                       bool firmware)
{
    MachineState *ms = MACHINE(s);
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible = mc->possible_cpu_arch_ids(ms);
    MemoryRegion *sysmem = get_system_memory();
    unsigned int i;

    for (i = 0; i < ms->smp.cpus; i++) {
        g_autofree char *name = g_strdup_printf("cpu%u", i);
        Object *cpuobj = object_new(possible->cpus[i].type);
        CPUState *cs = CPU(cpuobj);
        ARMCPU *cpu = ARM_CPU(cpuobj);

        object_property_add_child(OBJECT(ms), name, cpuobj);
        cs->cpu_index = i;
        numa_cpu_pre_plug(&possible->cpus[i], DEVICE(cpuobj),
                          &error_fatal);
        object_property_set_int(cpuobj, "mp-affinity",
                                possible->cpus[i].arch_id, &error_abort);
        object_property_set_int(cpuobj, "cntfrq",
                                PHYTIUM_POMELO_GTIMER_HZ, &error_abort);
        object_property_set_link(cpuobj, "memory", OBJECT(sysmem),
                                 &error_abort);
        cpu->midr = 0x701f0663;
        cpu->dtb_compatible = "phytium,ftc663";

        if (!firmware && object_property_find(cpuobj, "has_el3")) {
            object_property_set_bool(cpuobj, "has_el3", false,
                                     &error_abort);
        }
        if (firmware && i != 0 && object_property_find(cpuobj,
                                                        "start-powered-off")) {
            object_property_set_bool(cpuobj, "start-powered-off", true,
                                     &error_abort);
        }

        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        s->cpu[i] = ARM_CPU(cpuobj);
    }
}

static void phytium_pomelo_create_its(PhytiumPomeloMachineState *s)
{
    DeviceState *dev = qdev_new(its_class_name());

    object_property_add_child(OBJECT(s), "gic-its", OBJECT(dev));
    object_property_set_link(OBJECT(dev), "parent-gicv3", OBJECT(s->gic),
                             &error_abort);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0,
                    phytium_pomelo_memmap[PHYTIUM_POMELO_GIC_ITS].base);
}

static void phytium_pomelo_create_gic(PhytiumPomeloMachineState *s)
{
    MachineState *ms = MACHINE(s);
    SysBusDevice *gicbusdev;
    QList *redist_region_count;
    unsigned int i;

    s->gic = qdev_new(gicv3_class_name());
    object_property_add_child(OBJECT(s), "gic", OBJECT(s->gic));
    qdev_prop_set_uint32(s->gic, "revision", 3);
    qdev_prop_set_uint32(s->gic, "num-cpu", ms->smp.cpus);
    qdev_prop_set_uint32(s->gic, "num-irq",
                         PHYTIUM_POMELO_NUM_IRQS + GIC_INTERNAL);
    qdev_prop_set_bit(s->gic, "has-security-extensions", true);
    qdev_prop_set_bit(s->gic, "has-lpi", true);

    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, ms->smp.cpus);
    qdev_prop_set_array(s->gic, "redist-region-count", redist_region_count);
    object_property_set_link(OBJECT(s->gic), "sysmem",
                             OBJECT(get_system_memory()), &error_fatal);

    gicbusdev = SYS_BUS_DEVICE(s->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0,
                    phytium_pomelo_memmap[PHYTIUM_POMELO_GIC_DIST].base);
    sysbus_mmio_map(gicbusdev, 1,
                    phytium_pomelo_memmap[PHYTIUM_POMELO_GIC_REDIST].base);

    for (i = 0; i < ms->smp.cpus; i++) {
        DeviceState *cpudev = DEVICE(s->cpu[i]);
        int intidbase = PHYTIUM_POMELO_NUM_IRQS + i * GIC_INTERNAL;
        static const int timer_irq[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP] = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC] = ARCH_TIMER_S_EL1_IRQ,
        };

        for (int irq = 0; irq < ARRAY_SIZE(timer_irq); irq++) {
            qdev_connect_gpio_out(cpudev, irq,
                                  qdev_get_gpio_in(s->gic,
                                                   intidbase + timer_irq[irq]));
        }
        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(s->gic,
                                                     intidbase +
                                                     ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(s->gic,
                                                     intidbase +
                                                     VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicbusdev, i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, i + ms->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, i + 2 * ms->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, i + 3 * ms->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }

    phytium_pomelo_create_its(s);
}

static void phytium_pomelo_create_uarts(PhytiumPomeloMachineState *s)
{
    static const int irq[] = { 38, 39, 40, 41 };
    int i;

    for (i = 0; i < PHYTIUM_POMELO_NUM_UARTS; i++) {
        DeviceState *dev = qdev_new(TYPE_PL011);
        SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
        g_autofree char *name = g_strdup_printf("uart%d", i);

        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        qdev_prop_set_chr(dev, "chardev", serial_hd(i));
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0,
                        phytium_pomelo_memmap[PHYTIUM_POMELO_UART0 + i].base);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, irq[i]));
    }
}

static void phytium_pomelo_create_unimplemented(PhytiumPomeloMachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);

    object_property_add_child(OBJECT(s), "low-peripheral", OBJECT(dev));
    qdev_prop_set_string(dev, "name", "phytium-pomelo.low-peripheral");
    qdev_prop_set_uint64(dev, "size",
                         phytium_pomelo_memmap[PHYTIUM_POMELO_LOW_PERIPH].size);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0,
                            phytium_pomelo_memmap[
                                PHYTIUM_POMELO_LOW_PERIPH].base, -1000);
}

static int phytium_pomelo_pcie_map_irq(PCIDevice *pdev, int pin)
{
    return 60 + pin;
}

static void phytium_pomelo_create_pcie(PhytiumPomeloMachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_GPEX_HOST);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    MemoryRegion *ecam_alias;
    MemoryRegion *mmio_alias;
    MemoryRegion *mmio_high_alias;
    MemoryRegion *ecam_reg;
    MemoryRegion *mmio_reg;
    int i;

    object_property_add_child(OBJECT(s), "pcie", OBJECT(dev));
    qdev_prop_set_uint64(dev, PCI_HOST_ECAM_BASE,
        phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_ECAM].base);
    qdev_prop_set_uint64(dev, PCI_HOST_ECAM_SIZE,
        phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_ECAM].size);
    qdev_prop_set_uint64(dev, PCI_HOST_PIO_BASE,
        phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_PIO].base);
    qdev_prop_set_uint64(dev, PCI_HOST_PIO_SIZE,
        phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_PIO].size);
    qdev_prop_set_uint64(dev, PCI_HOST_BELOW_4G_MMIO_BASE,
        phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_MMIO].base);
    qdev_prop_set_uint64(dev, PCI_HOST_BELOW_4G_MMIO_SIZE,
        phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_MMIO].size);
    qdev_prop_set_uint64(dev, PCI_HOST_ABOVE_4G_MMIO_BASE,
        phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_MMIO_HIGH].base);
    qdev_prop_set_uint64(dev, PCI_HOST_ABOVE_4G_MMIO_SIZE,
        phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_MMIO_HIGH].size);
    sysbus_realize_and_unref(sbd, &error_fatal);
    pci_bus_map_irqs(PCI_HOST_BRIDGE(dev)->bus, phytium_pomelo_pcie_map_irq);

    ecam_reg = sysbus_mmio_get_region(sbd, 0);
    ecam_alias = g_new0(MemoryRegion, 1);
    memory_region_init_alias(ecam_alias, OBJECT(dev), "pomelo-pcie-ecam",
                             ecam_reg, 0,
                             phytium_pomelo_memmap[
                                 PHYTIUM_POMELO_PCIE_ECAM].size);
    memory_region_add_subregion(get_system_memory(),
        phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_ECAM].base, ecam_alias);

    mmio_reg = sysbus_mmio_get_region(sbd, 1);
    mmio_alias = g_new0(MemoryRegion, 1);
    memory_region_init_alias(mmio_alias, OBJECT(dev), "pomelo-pcie-mmio",
                             mmio_reg,
                             phytium_pomelo_memmap[
                                 PHYTIUM_POMELO_PCIE_MMIO].base,
                             phytium_pomelo_memmap[
                                 PHYTIUM_POMELO_PCIE_MMIO].size);
    memory_region_add_subregion(get_system_memory(),
        phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_MMIO].base, mmio_alias);

    mmio_high_alias = g_new0(MemoryRegion, 1);
    memory_region_init_alias(mmio_high_alias, OBJECT(dev),
                             "pomelo-pcie-mmio-high", mmio_reg,
                             phytium_pomelo_memmap[
                                 PHYTIUM_POMELO_PCIE_MMIO_HIGH].base,
                             phytium_pomelo_memmap[
                                 PHYTIUM_POMELO_PCIE_MMIO_HIGH].size);
    memory_region_add_subregion(get_system_memory(),
        phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_MMIO_HIGH].base,
        mmio_high_alias);

    sysbus_mmio_map(sbd, 2,
                    phytium_pomelo_memmap[PHYTIUM_POMELO_PCIE_PIO].base);
    for (i = 0; i < PCI_NUM_PINS; i++) {
        sysbus_connect_irq(sbd, i, qdev_get_gpio_in(s->gic, 60 + i));
        gpex_set_irq_num(GPEX_HOST(dev), i, 60 + i);
    }
}

static void phytium_pomelo_create_gmacs(PhytiumPomeloMachineState *s)
{
    static const int irq[] = { 81, 82 };
    int i;

    for (i = 0; i < PHYTIUM_POMELO_NUM_GMACS; i++) {
        DeviceState *dev = qdev_new(TYPE_DWMAC4);
        SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
        g_autofree char *name = g_strdup_printf("gmac%d", i);

        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        qemu_configure_nic_device(dev, true, name);
        qdev_prop_set_uint8(dev, "dma-width", 40);
        qdev_prop_set_uint8(dev, "phy-addr", i);
        sysbus_realize_and_unref(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0,
                        phytium_pomelo_memmap[PHYTIUM_POMELO_GMAC0 + i].base);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, irq[i]));
    }
}

static void phytium_pomelo_create_ram(PhytiumPomeloMachineState *s)
{
    MachineState *ms = MACHINE(s);
    uint64_t low = MIN(ms->ram_size,
                       phytium_pomelo_memmap[PHYTIUM_POMELO_RAM_LOW].size);
    uint64_t high = ms->ram_size - low;

    memory_region_init_alias(&s->ram_low, OBJECT(s), "pomelo.ram-low",
                             ms->ram, 0, low);
    memory_region_add_subregion(get_system_memory(),
        phytium_pomelo_memmap[PHYTIUM_POMELO_RAM_LOW].base, &s->ram_low);
    if (high) {
        memory_region_init_alias(&s->ram_high, OBJECT(s), "pomelo.ram-high",
                                 ms->ram, low, high);
        memory_region_add_subregion(get_system_memory(),
            phytium_pomelo_memmap[PHYTIUM_POMELO_RAM_HIGH].base,
            &s->ram_high);
    }
}

static void phytium_pomelo_add_memory_node(void *fdt, hwaddr base,
                                           uint64_t size)
{
    uint32_t acells = qemu_fdt_getprop_cell(fdt, "/", "#address-cells",
                                            NULL, &error_fatal);
    uint32_t scells = qemu_fdt_getprop_cell(fdt, "/", "#size-cells",
                                            NULL, &error_fatal);
    g_autofree char *name = g_strdup_printf("/memory@%" PRIx64, base);

    qemu_fdt_add_subnode(fdt, name);
    qemu_fdt_setprop_string(fdt, name, "device_type", "memory");
    qemu_fdt_setprop_sized_cells(fdt, name, "reg",
                                 acells, base, scells, size);
}

static void *phytium_pomelo_get_dtb(const struct arm_boot_info *info,
                                    int *fdt_size)
{
    PhytiumPomeloMachineState *s = container_of(info,
                                                PhytiumPomeloMachineState,
                                                bootinfo);
    MachineState *ms = MACHINE(s);
    void *fdt = create_device_tree(fdt_size);
    uint32_t phandle;
    uint32_t its_phandle;
    uint32_t clock_phandle;
    int i;
    static const char * const root_compat[] = {
        "phytium,d2000-pomelo", "phytium,d2000",
    };
    static const char * const cpu_compat[] = {
        "phytium,ftc663", "arm,armv8",
    };
    static const char * const uart_compat[] = {
        "arm,pl011", "arm,primecell",
    };
    static const char * const gmac_compat[] = {
        "phytium,dwmac", "snps,dwmac-4.20a",
    };

    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(EXIT_FAILURE);
    }
    qemu_fdt_setprop_string(fdt, "/", "model", "Phytium Pomelo Board");
    qemu_fdt_setprop_string_array(fdt, "/", "compatible",
                                  (char **)root_compat,
                                  ARRAY_SIZE(root_compat));
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 2);

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0);
    for (i = 0; i < ms->smp.cpus; i++) {
        g_autofree char *name = g_strdup_printf("/cpus/cpu@%" PRIx64,
                                                phytium_pomelo_cpu_mpidr(i));
        qemu_fdt_add_subnode(fdt, name);
        qemu_fdt_setprop_string(fdt, name, "device_type", "cpu");
        qemu_fdt_setprop_string_array(fdt, name, "compatible",
                                      (char **)cpu_compat,
                                      ARRAY_SIZE(cpu_compat));
        qemu_fdt_setprop_u64(fdt, name, "reg",
                             phytium_pomelo_cpu_mpidr(i));
        qemu_fdt_setprop_string(fdt, name, "enable-method", "psci");
    }

    phytium_pomelo_add_memory_node(
        fdt, phytium_pomelo_memmap[PHYTIUM_POMELO_RAM_LOW].base,
        MIN(ms->ram_size,
            phytium_pomelo_memmap[PHYTIUM_POMELO_RAM_LOW].size));
    if (ms->ram_size > phytium_pomelo_memmap[PHYTIUM_POMELO_RAM_LOW].size) {
        phytium_pomelo_add_memory_node(
            fdt, phytium_pomelo_memmap[PHYTIUM_POMELO_RAM_HIGH].base,
            ms->ram_size - phytium_pomelo_memmap[
                PHYTIUM_POMELO_RAM_LOW].size);
    }

    phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_add_subnode(fdt, "/intc@29a00000");
    qemu_fdt_setprop_string(fdt, "/intc@29a00000", "compatible",
                            "arm,gic-v3");
    qemu_fdt_setprop_cell(fdt, "/intc@29a00000", "#interrupt-cells", 3);
    qemu_fdt_setprop_cell(fdt, "/intc@29a00000", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/intc@29a00000", "#size-cells", 2);
    qemu_fdt_setprop(fdt, "/intc@29a00000", "interrupt-controller",
                     NULL, 0);
    qemu_fdt_setprop_sized_cells(fdt, "/intc@29a00000", "reg",
        2, phytium_pomelo_memmap[PHYTIUM_POMELO_GIC_DIST].base,
        2, phytium_pomelo_memmap[PHYTIUM_POMELO_GIC_DIST].size,
        2, phytium_pomelo_memmap[PHYTIUM_POMELO_GIC_REDIST].base,
        2, phytium_pomelo_memmap[PHYTIUM_POMELO_GIC_REDIST].size);
    qemu_fdt_setprop_cell(fdt, "/intc@29a00000", "phandle", phandle);
    qemu_fdt_setprop_cell(fdt, "/", "interrupt-parent", phandle);
    its_phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_add_subnode(fdt, "/intc@29a00000/its@29a20000");
    qemu_fdt_setprop_string(fdt, "/intc@29a00000/its@29a20000", "compatible",
                            "arm,gic-v3-its");
    qemu_fdt_setprop_sized_cells(fdt, "/intc@29a00000/its@29a20000", "reg",
        2, phytium_pomelo_memmap[PHYTIUM_POMELO_GIC_ITS].base,
        2, phytium_pomelo_memmap[PHYTIUM_POMELO_GIC_ITS].size);
    qemu_fdt_setprop(fdt, "/intc@29a00000/its@29a20000", "msi-controller",
                     NULL, 0);
    qemu_fdt_setprop_cell(fdt, "/intc@29a00000/its@29a20000", "#msi-cells",
                          1);
    qemu_fdt_setprop_cell(fdt, "/intc@29a00000/its@29a20000", "phandle",
                          its_phandle);

    qemu_fdt_add_subnode(fdt, "/timer");
    qemu_fdt_setprop_string(fdt, "/timer", "compatible",
                            "arm,armv8-timer");
    qemu_fdt_setprop_cells(fdt, "/timer", "interrupts",
        GIC_FDT_IRQ_TYPE_PPI, 13, GIC_FDT_IRQ_FLAGS_LEVEL_HI,
        GIC_FDT_IRQ_TYPE_PPI, 14, GIC_FDT_IRQ_FLAGS_LEVEL_HI,
        GIC_FDT_IRQ_TYPE_PPI, 11, GIC_FDT_IRQ_FLAGS_LEVEL_HI,
        GIC_FDT_IRQ_TYPE_PPI, 10, GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(fdt, "/timer", "clock-frequency",
                          PHYTIUM_POMELO_GTIMER_HZ);

    qemu_fdt_add_subnode(fdt, "/aliases");
    qemu_fdt_add_subnode(fdt, "/clk48mhz");
    clock_phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_setprop_string(fdt, "/clk48mhz", "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(fdt, "/clk48mhz", "#clock-cells", 0);
    qemu_fdt_setprop_cell(fdt, "/clk48mhz", "clock-frequency", 48000000);
    qemu_fdt_setprop_string(fdt, "/clk48mhz", "clock-output-names",
                            "sysclk_48mhz");
    qemu_fdt_setprop_cell(fdt, "/clk48mhz", "phandle", clock_phandle);
    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 2);
    qemu_fdt_setprop(fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_add_subnode(fdt, "/soc/serial@28001000");
    qemu_fdt_setprop_string_array(fdt, "/soc/serial@28001000", "compatible",
                                  (char **)uart_compat,
                                  ARRAY_SIZE(uart_compat));
    qemu_fdt_setprop_sized_cells(fdt, "/soc/serial@28001000", "reg",
        2, 0x28001000, 2, 0x1000);
    qemu_fdt_setprop_cells(fdt, "/soc/serial@28001000", "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, 39,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_cell(fdt, "/soc/serial@28001000", "clocks",
                          clock_phandle);
    qemu_fdt_setprop_string(fdt, "/aliases", "serial0",
                            "/soc/serial@28001000");
    qemu_fdt_setprop_string(fdt, "/aliases", "ethernet0",
                            "/soc/ethernet@2820c000");
    qemu_fdt_setprop_string(fdt, "/aliases", "ethernet1",
                            "/soc/ethernet@28210000");

    qemu_fdt_add_subnode(fdt, "/pcie@40000000");
    qemu_fdt_setprop_string(fdt, "/pcie@40000000", "compatible",
                            "pci-host-ecam-generic");
    qemu_fdt_setprop_string(fdt, "/pcie@40000000", "device_type", "pci");
    qemu_fdt_setprop_cell(fdt, "/pcie@40000000", "#address-cells", 3);
    qemu_fdt_setprop_cell(fdt, "/pcie@40000000", "#size-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/pcie@40000000", "#interrupt-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/pcie@40000000", "linux,pci-domain", 0);
    qemu_fdt_setprop(fdt, "/pcie@40000000", "dma-coherent", NULL, 0);
    qemu_fdt_setprop_cells(fdt, "/pcie@40000000", "msi-map",
                           0, its_phandle, 0, 0x10000);
    qemu_fdt_setprop_sized_cells(fdt, "/pcie@40000000", "reg",
        2, 0x40000000, 2, 0x10000000);
    qemu_fdt_setprop_cells(fdt, "/pcie@40000000", "bus-range", 0, 0xff);
    qemu_fdt_setprop_cells(fdt, "/pcie@40000000", "ranges",
        0x01000000, 0, 0, 0, 0x50000000, 0, 0x00f00000,
        0x02000000, 0, 0x58000000, 0, 0x58000000, 0, 0x28000000,
        0x43000000, 0x10, 0, 0x10, 0, 0x10, 0);

    {
        uint32_t irq_map[4 * 4 * 10] = { 0 };
        uint32_t *entry = irq_map;
        int devfn;
        int pin;

        for (devfn = 0; devfn <= 0x18; devfn += 0x8) {
            for (pin = 0; pin < PCI_NUM_PINS; pin++) {
                uint32_t map[] = {
                    devfn << 8, 0, 0, pin + 1,
                    phandle, 0, 0, GIC_FDT_IRQ_TYPE_SPI,
                    60 + ((pin + PCI_SLOT(devfn)) % PCI_NUM_PINS),
                    GIC_FDT_IRQ_FLAGS_LEVEL_HI,
                };

                for (i = 0; i < ARRAY_SIZE(map); i++) {
                    entry[i] = cpu_to_be32(map[i]);
                }
                entry += ARRAY_SIZE(map);
            }
        }
        qemu_fdt_setprop(fdt, "/pcie@40000000", "interrupt-map", irq_map,
                         sizeof(irq_map));
        qemu_fdt_setprop_cells(fdt, "/pcie@40000000", "interrupt-map-mask",
                               cpu_to_be16(PCI_DEVFN(3, 0)), 0, 0, 0x7);
    }

    qemu_fdt_add_subnode(fdt, "/soc/ethernet@2820c000");
    qemu_fdt_setprop_string_array(fdt, "/soc/ethernet@2820c000",
                                  "compatible", (char **)gmac_compat,
                                  ARRAY_SIZE(gmac_compat));
    qemu_fdt_setprop_sized_cells(fdt, "/soc/ethernet@2820c000", "reg",
        2, 0x2820c000, 2, 0x2000);
    qemu_fdt_setprop_string(fdt, "/soc/ethernet@2820c000", "phy-mode",
                            "rgmii");
    qemu_fdt_setprop_cells(fdt, "/soc/ethernet@2820c000", "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, 81,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_string(fdt, "/soc/ethernet@2820c000", "status",
                            "okay");

    qemu_fdt_add_subnode(fdt, "/soc/ethernet@28210000");
    qemu_fdt_setprop_string_array(fdt, "/soc/ethernet@28210000",
                                  "compatible", (char **)gmac_compat,
                                  ARRAY_SIZE(gmac_compat));
    qemu_fdt_setprop_sized_cells(fdt, "/soc/ethernet@28210000", "reg",
        2, 0x28210000, 2, 0x2000);
    qemu_fdt_setprop_string(fdt, "/soc/ethernet@28210000", "phy-mode",
                            "rgmii");
    qemu_fdt_setprop_cells(fdt, "/soc/ethernet@28210000", "interrupts",
                           GIC_FDT_IRQ_TYPE_SPI, 82,
                           GIC_FDT_IRQ_FLAGS_LEVEL_HI);
    qemu_fdt_setprop_string(fdt, "/soc/ethernet@28210000", "status",
                            "okay");

    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path",
                            "serial0:115200n8");
    return fdt;
}

static void phytium_pomelo_load_uboot(PhytiumPomeloMachineState *s)
{
    MachineState *ms = MACHINE(s);
    ssize_t size;

    size = load_image_targphys(ms->firmware, PHYTIUM_POMELO_UBOOT_BASE,
                               PHYTIUM_POMELO_UBOOT_MAX_SIZE, NULL);
    if (size < 0) {
        error_report("could not load upstream Pomelo U-Boot '%s'",
                     ms->firmware);
        exit(1);
    }
    arm_emulate_firmware_reset(CPU(s->cpu[0]), 2);
    cpu_set_pc(CPU(s->cpu[0]), PHYTIUM_POMELO_UBOOT_BASE);
}

static bool phytium_pomelo_smc_handler(ARMCPU *cpu)
{
    CPUARMState *env = &cpu->env;
    uint64_t fn = is_a64(env) ? env->xregs[0] : env->regs[0];
    uint64_t ret;

    switch ((uint32_t)fn) {
    case PHYTIUM_POMELO_SMC_GET_RST_SOURCE:
        ret = 1; /* CPU_RESET_POWER_ON */
        break;
    case PHYTIUM_POMELO_SMC_INIT_PLL:
    case PHYTIUM_POMELO_SMC_INIT_PCIE:
    case PHYTIUM_POMELO_SMC_INIT_MEM:
    case PHYTIUM_POMELO_SMC_INIT_SEC:
        ret = 0;
        break;
    default:
        return false;
    }

    if (is_a64(env)) {
        env->xregs[0] = ret;
    } else {
        env->regs[0] = ret;
    }
    return true;
}

static void phytium_pomelo_init(MachineState *machine)
{
    PhytiumPomeloMachineState *s = PHYTIUM_POMELO_MACHINE(machine);
    bool firmware = !!machine->firmware;

    if (kvm_enabled()) {
        error_report("phytium-pomelo: KVM is not supported");
        exit(1);
    }
    arm_register_psci_smc_handler(phytium_pomelo_smc_handler);
    if (machine->smp.cpus > PHYTIUM_POMELO_NUM_CPUS) {
        error_report("phytium-pomelo supports at most %d CPUs",
                     PHYTIUM_POMELO_NUM_CPUS);
        exit(1);
    }
    if (machine->ram_size > phytium_pomelo_memmap[
            PHYTIUM_POMELO_RAM_LOW].size + phytium_pomelo_memmap[
            PHYTIUM_POMELO_RAM_HIGH].size) {
        error_report("phytium-pomelo RAM size is too large");
        exit(1);
    }

    phytium_pomelo_create_ram(s);
    phytium_pomelo_create_cpus(s, firmware);
    phytium_pomelo_create_gic(s);
    phytium_pomelo_create_uarts(s);
    phytium_pomelo_create_unimplemented(s);
    phytium_pomelo_create_pcie(s);
    phytium_pomelo_create_gmacs(s);

    s->bootinfo = (struct arm_boot_info) {
        .loader_start = phytium_pomelo_memmap[PHYTIUM_POMELO_RAM_LOW].base,
        .board_id = -1,
        .ram_size = machine->ram_size,
        .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
        .get_dtb = phytium_pomelo_get_dtb,
    };

    if (qtest_enabled()) {
        return;
    }
    if (firmware) {
        phytium_pomelo_load_uboot(s);
        return;
    }
    if (!machine->kernel_filename) {
        error_report("phytium-pomelo requires -kernel or upstream U-Boot "
                     "via -bios");
        exit(1);
    }
    arm_load_kernel(s->cpu[0], machine, &s->bootinfo);
}

static const CPUArchIdList *phytium_pomelo_possible_cpu_arch_ids(
    MachineState *ms)
{
    if (!ms->possible_cpus) {
        ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                      sizeof(CPUArchId) * ms->smp.max_cpus);
        ms->possible_cpus->len = ms->smp.max_cpus;
        for (int i = 0; i < ms->possible_cpus->len; i++) {
            CPUArchId *slot = &ms->possible_cpus->cpus[i];
            slot->type = ARM_CPU_TYPE_NAME("cortex-a72");
            slot->arch_id = phytium_pomelo_cpu_mpidr(i);
            slot->props.has_cluster_id = true;
            slot->props.cluster_id = i / 2;
            slot->props.has_core_id = true;
            slot->props.core_id = i % 2;
            slot->props.has_thread_id = true;
            slot->props.thread_id = 0;
        }
    }
    return ms->possible_cpus;
}

static void phytium_pomelo_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Phytium Pomelo board (D2000)";
    mc->init = phytium_pomelo_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a72");
    mc->max_cpus = PHYTIUM_POMELO_NUM_CPUS;
    mc->default_cpus = PHYTIUM_POMELO_NUM_CPUS;
    mc->default_ram_size = 3 * GiB;
    mc->default_ram_id = "phytium-pomelo.ram";
    mc->possible_cpu_arch_ids = phytium_pomelo_possible_cpu_arch_ids;
    mc->alias = "phytium-d2000";
}

static const TypeInfo phytium_pomelo_machine_info = {
    .name = TYPE_PHYTIUM_POMELO_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(PhytiumPomeloMachineState),
    .class_init = phytium_pomelo_class_init,
    .interfaces = aarch64_machine_interfaces,
};

static void phytium_pomelo_register_types(void)
{
    type_register_static(&phytium_pomelo_machine_info);
}

type_init(phytium_pomelo_register_types);
