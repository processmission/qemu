/*
 * NXP S32K5 board emulation
 *
 * This model starts with the S32K566 Cortex-R52 side of the S32K5XXCVB
 * board. It is intentionally minimal and targets Zephyr boot smoke tests
 * rather than full automotive SoC coverage.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qobject/qlist.h"
#include "system/address-spaces.h"
#include "target/arm/cpu.h"
#include "system/system.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/nxp_lpuart.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/arm_gicv3.h"
#include "hw/misc/nxp_s32_mc_me.h"
#include "hw/misc/unimp.h"

#define TYPE_S32K5_MACHINE MACHINE_TYPE_NAME("s32k5")
#define TYPE_S32K566_CVB_R52_MACHINE MACHINE_TYPE_NAME("s32k566-cvb-r52")

#define S32K5_CPU_MAX 2
#define S32K5_NUM_SPIS 320

#define S32K5_CODE_MRAM_BASE 0x08000000
#define S32K5_CODE_MRAM_SIZE (32 * MiB + 0x100)
#define S32K5_SRAM0_BASE     0x21000000
#define S32K5_SRAM1_BASE     0x21080000
#define S32K5_SRAM2_BASE     0x21100000
#define S32K5_SHARED_SRAM_SIZE (512 * KiB)
#define S32K5_CPE_SRAM_BASE  0x22000000
#define S32K5_CPE_SRAM_SIZE  (1 * MiB)
#define S32K5_MC_ME_BASE     0x40498000
#define S32K5_LPUART0_BASE   0x400e0000
#define S32K5_LPUART0_IRQ    144
#define S32K5_GIC_DIST_BASE  0x43000000
#define S32K5_GIC_REDIST_BASE 0x43100000
#define S32K5_ARCH_TIMER_FREQ 4000000

struct S32K5MachineState {
    MachineState parent;

    struct arm_boot_info bootinfo;
    Object *cpu[S32K5_CPU_MAX];
    GICv3State gic;
    NXPS32MCMEState mc_me;
    NXPLPUARTState lpuart0;
    MemoryRegion code_mram;
    MemoryRegion sram0;
    MemoryRegion sram1;
    MemoryRegion sram2;
};

OBJECT_DECLARE_SIMPLE_TYPE(S32K5MachineState, S32K5_MACHINE)

static void s32k5_create_unimplemented_devices(void)
{
    static const struct {
        const char *name;
        hwaddr base;
        hwaddr size;
    } regions[] = {
        { "s32k5.clock-0", 0x42110000, 0x4000 },
        { "s32k5.clock-1", 0x42118000, 0x4000 },
        { "s32k5.clock-2", 0x4211c000, 0x4000 },
        { "s32k5.clock-3", 0x41074000, 0x4000 },
        { "s32k5.clock-4", 0x41078000, 0x4000 },
        { "s32k5.clock-5", 0x40094000, 0x4000 },
        { "s32k5.clock-6", 0x40098000, 0x4000 },
        { "s32k5.clock-7", 0x402fc000, 0x4000 },
        { "s32k5.clock-8", 0x404b8000, 0x4000 },
        { "s32k5.clock-9", 0x40b38000, 0x4000 },
        { "s32k5.clock-10", 0x40b3c000, 0x4000 },
        { "s32k5.clock-11", 0x40b40000, 0x4000 },
        { "s32k5.clock-12", 0x42120000, 0x4000 },
        { "s32k5.swt-startup", 0x404a8000, 0x4000 },
        { "s32k5.siul2-0", 0x40014000, 0x10000 },
        { "s32k5.siul2-1", 0x40204000, 0x10000 },
        { "s32k5.siul2-2", 0x4208c000, 0x10000 },
        { "s32k5.siul2-3", 0x40610000, 0x10000 },
        { "s32k5.siul2-4", 0x40804000, 0x10000 },
        { "s32k5.xspi0", 0x40ba4000, 0x8000 },
        { "s32k5.edma", 0x40410000, 0x4000 },
    };

    for (int i = 0; i < ARRAY_SIZE(regions); i++) {
        create_unimplemented_device(regions[i].name, regions[i].base,
                                    regions[i].size);
    }
}

static void s32k5_create_gic(S32K5MachineState *sms, MemoryRegion *sysmem)
{
    MachineState *machine = MACHINE(sms);
    DeviceState *gicdev;
    QList *redist_region_count;

    object_initialize_child(OBJECT(sms), "gic", &sms->gic, TYPE_ARM_GICV3);
    gicdev = DEVICE(&sms->gic);
    qdev_prop_set_uint32(gicdev, "num-cpu", machine->smp.cpus);
    qdev_prop_set_uint32(gicdev, "num-irq", S32K5_NUM_SPIS + GIC_INTERNAL);
    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, machine->smp.cpus);
    qdev_prop_set_array(gicdev, "redist-region-count", redist_region_count);
    object_property_set_link(OBJECT(&sms->gic), "sysmem",
                             OBJECT(sysmem), &error_fatal);
    sysbus_realize(SYS_BUS_DEVICE(&sms->gic), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&sms->gic), 0, S32K5_GIC_DIST_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(&sms->gic), 1, S32K5_GIC_REDIST_BASE);

    for (int i = 0; i < machine->smp.cpus; i++) {
        DeviceState *cpudev = DEVICE(sms->cpu[i]);
        SysBusDevice *gicsbd = SYS_BUS_DEVICE(&sms->gic);
        int intidbase = S32K5_NUM_SPIS + i * GIC_INTERNAL;
        const int timer_irq[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,
        };

        for (int irq = 0; irq < ARRAY_SIZE(timer_irq); irq++) {
            qdev_connect_gpio_out(cpudev, irq,
                                  qdev_get_gpio_in(gicdev,
                                                   intidbase + timer_irq[irq]));
        }

        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(gicdev,
                                                     intidbase + ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(gicdev,
                                                     intidbase + VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicsbd, i,
                           qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicsbd, i + machine->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicsbd, i + 2 * machine->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicsbd, i + 3 * machine->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
}

static void s32k5_common_init(MachineState *machine)
{
    S32K5MachineState *sms = S32K5_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    SysBusDevice *sbd;

    memory_region_init_ram(&sms->code_mram, NULL, "s32k5.code-mram",
                           S32K5_CODE_MRAM_SIZE, &error_fatal);
    memory_region_set_readonly(&sms->code_mram, true);
    memory_region_add_subregion(sysmem, S32K5_CODE_MRAM_BASE,
                                &sms->code_mram);

    memory_region_init_ram(&sms->sram0, NULL, "s32k5.sram0",
                           S32K5_SHARED_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, S32K5_SRAM0_BASE, &sms->sram0);

    memory_region_init_ram(&sms->sram1, NULL, "s32k5.sram1",
                           S32K5_SHARED_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, S32K5_SRAM1_BASE, &sms->sram1);

    memory_region_init_ram(&sms->sram2, NULL, "s32k5.sram2",
                           S32K5_SHARED_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, S32K5_SRAM2_BASE, &sms->sram2);

    memory_region_add_subregion(sysmem, S32K5_CPE_SRAM_BASE, machine->ram);

    assert(machine->smp.cpus <= S32K5_CPU_MAX);
    for (int i = 0; i < machine->smp.cpus; i++) {
        sms->cpu[i] = object_new(machine->cpu_type);
        object_property_set_link(sms->cpu[i], "memory", OBJECT(sysmem),
                                 &error_abort);
        object_property_set_int(sms->cpu[i], "reset-cbar",
                                S32K5_GIC_DIST_BASE, &error_abort);
        object_property_set_int(sms->cpu[i], "cntfrq",
                                S32K5_ARCH_TIMER_FREQ, &error_abort);
        if (i > 0) {
            object_property_set_bool(sms->cpu[i], "start-powered-off", true,
                                     &error_abort);
        }
        qdev_realize(DEVICE(sms->cpu[i]), NULL, &error_fatal);
        object_unref(sms->cpu[i]);
    }

    s32k5_create_gic(sms, sysmem);

    object_initialize_child(OBJECT(sms), "mc-me", &sms->mc_me,
                            TYPE_NXP_S32_MC_ME);
    sbd = SYS_BUS_DEVICE(&sms->mc_me);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S32K5_MC_ME_BASE);

    object_initialize_child(OBJECT(sms), "lpuart0", &sms->lpuart0,
                            TYPE_NXP_LPUART);
    qdev_prop_set_chr(DEVICE(&sms->lpuart0), "chardev", serial_hd(0));
    sbd = SYS_BUS_DEVICE(&sms->lpuart0);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, S32K5_LPUART0_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(DEVICE(&sms->gic), S32K5_LPUART0_IRQ));

    s32k5_create_unimplemented_devices();

    sms->bootinfo.ram_size = S32K5_CPE_SRAM_BASE + S32K5_CPE_SRAM_SIZE -
                             S32K5_CODE_MRAM_BASE;
    sms->bootinfo.board_id = -1;
    sms->bootinfo.loader_start = S32K5_CODE_MRAM_BASE;
    arm_load_kernel(ARM_CPU(sms->cpu[0]), machine, &sms->bootinfo);
}

static void s32k566_cvb_r52_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-r52"),
        NULL
    };

    mc->desc = "NXP S32K566 CVB Cortex-R52";
    mc->init = s32k5_common_init;
    mc->default_cpus = 1;
    mc->min_cpus = 1;
    mc->max_cpus = S32K5_CPU_MAX;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-r52");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = S32K5_CPE_SRAM_SIZE;
    mc->default_ram_id = "s32k5.cpe-sram";
}

static const TypeInfo s32k5_machine_types[] = {
    {
        .name = TYPE_S32K5_MACHINE,
        .parent = TYPE_MACHINE,
        .abstract = true,
        .instance_size = sizeof(S32K5MachineState),
    }, {
        .name = TYPE_S32K566_CVB_R52_MACHINE,
        .parent = TYPE_S32K5_MACHINE,
        .class_init = s32k566_cvb_r52_class_init,
        .interfaces = arm_machine_interfaces,
    },
};

DEFINE_TYPES(s32k5_machine_types);
