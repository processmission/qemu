/*
 * Local-only Phytium Pi board model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/block-backend-io.h"
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "system/kvm.h"
#include "system/memory.h"
#include "system/numa.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/system.h"
#include "exec/hwaddr.h"
#include "hw/arm/phytium-pi.h"
#include "hw/arm/bsa.h"
#include "hw/arm/boot.h"
#include "hw/arm/fdt.h"
#include "hw/arm/linux-boot-if.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/pl011.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/sysbus.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/misc/phytium-ddr-ctrl.h"
#include "hw/misc/phytium-scp-mailbox.h"
#include "hw/misc/unimp.h"
#include "hw/net/phytium-xmac.h"
#include "hw/sd/phytium-mci.h"
#include "hw/sd/sd.h"
#include "qobject/qlist.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/cpregs.h"
#include "target/arm/gtimer.h"
#include "target/arm/internals.h"
#include "target/arm/kvm-consts.h"

#define PHYTIUMPI_FIRMWARE_BOOT_CPU_INDEX 2
#define PHYTIUMPI_NUM_SPI_IRQS 256
#define PHYTIUMPI_GTIMER_HZ 50000000
#define PHYTIUMPI_DEFAULT_RAM_SIZE (4 * GiB)
#define PHYTIUMPI_RAM_LOW_BASE 0x80000000ULL
#define PHYTIUMPI_RAM_LOW_SIZE (2 * GiB)
#define PHYTIUMPI_RAM_HIGH_BASE 0x2000000000ULL
#define PHYTIUMPI_RAM_HIGH_SIZE (2 * GiB)
#define PHYTIUMPI_RAM_MAX_SIZE \
    (PHYTIUMPI_RAM_LOW_SIZE + PHYTIUMPI_RAM_HIGH_SIZE)
#define PHYTIUMPI_BOOTROM_SIZE (4 * MiB)
#define PHYTIUMPI_BOOTROM_BASE 0
#define PHYTIUMPI_FIRMWARE_ENTRY_OFFSET 0x40000
#define PHYTIUMPI_FW_RAM_BASE 0x38000000
#define PHYTIUMPI_FW_RAM_SIZE (16 * MiB)
#define PHYTIUMPI_FW_HIGH_RAM_BASE 0x39000000
#define PHYTIUMPI_FW_HIGH_RAM_SIZE (16 * MiB)
#define PHYTIUMPI_FW_SYNC_OFFSET 0xf4000
#define PHYTIUMPI_FW_SYNC_MAGIC 0x54460000
#define PHYTIUMPI_FW_CTRL_BASE 0x30c00000
#define PHYTIUMPI_FW_CTRL_SIZE (1 * MiB)
#define PHYTIUMPI_FW_CTRL_RESET_PTR_OFFSET 0x1000
#define PHYTIUMPI_FW_CTRL_RESET_MAGIC_OFFSET 0x2000
#define PHYTIUMPI_FW_CTRL_RESET_MAGIC 0xffaabbcc
#define PHYTIUMPI_FW_CTRL_UART_OFFSET 0x12e8
#define PHYTIUMPI_FW_CTRL_PTR_OFFSET 0x13f80
#define PHYTIUMPI_FW_CTRL_PTR_VALUE 0
#define PHYTIUMPI_SYS_CTRL_BASE 0x30000000
#define PHYTIUMPI_SYS_CTRL_SIZE 0x1000
#define PHYTIUMPI_SCP_CTRL_BASE 0x32a00000
#define PHYTIUMPI_SCP_CTRL_SIZE 0x1000
#define PHYTIUMPI_SCP_SRAM_BASE 0x32a10000
#define PHYTIUMPI_SCP_SRAM_SIZE 0x2000
#define PHYTIUMPI_RNG_BASE 0x32a36000
#define PHYTIUMPI_RNG_SIZE 0x1000
#define PHYTIUMPI_SCP_MAILBOX_BASE (PHYTIUMPI_SCP_SRAM_BASE + 0x400)
#define PHYTIUMPI_SCP_READY_OFFSET 0x1808
#define PHYTIUMPI_SCP_READY_MAGIC 0x00abcdef
#define PHYTIUMPI_FW_MISC_BASE 0x32b30000
#define PHYTIUMPI_FW_MISC_SIZE 0x10000
#define PHYTIUMPI_DDR_CTRL_BASE 0x32b33000
#define PHYTIUMPI_UBOOT_DDR_RESULT_LOAD_OFFSET 0x183fe8
#define PHYTIUMPI_UBOOT_DDR_RESULT_LOAD 0xf94013a1
#define PHYTIUMPI_UBOOT_DDR_RESULT_CHECK_OFFSET 0x183fec
#define PHYTIUMPI_UBOOT_DDR_RESULT_CHECK 0xb4000401
#define PHYTIUMPI_UBOOT_DDR_RESULT_SKIP 0x14000020
#define PHYTIUMPI_FW_CFG_BASE 0x33000000
#define PHYTIUMPI_FW_CFG_SIZE 0x10000
#define PHYTIUMPI_XMAC_BASE 0x3200c000
#define PHYTIUMPI_XMAC_SIZE 0x2000
#define PHYTIUMPI_XMAC_IRQ 87
#define PHYTIUMPI_PCIE_ECAM_BASE 0x40000000
#define PHYTIUMPI_PCIE_ECAM_SIZE 0x10000000
#define PHYTIUMPI_SMCCC_ARCH_FEATURES 0x80000001
#define PHYTIUMPI_SMCCC_ARCH_SOC_ID 0x80000002
#define PHYTIUMPI_SMCCC_ARCH_WORKAROUND_1 0x80008000
#define PHYTIUMPI_SMCCC_ARCH_WORKAROUND_2 0x80007fff
#define PHYTIUMPI_SMCCC_ARCH_WORKAROUND_3 0x80003fff
#define PHYTIUMPI_SMCCC_RET_SUCCESS 0
#define PHYTIUMPI_SMCCC_RET_NOT_SUPPORTED (-1)
#define FDT_GIC_SPI GIC_FDT_IRQ_TYPE_SPI
#define FDT_GIC_PPI GIC_FDT_IRQ_TYPE_PPI
#define FDT_IRQ_TYPE_LEVEL_HIGH 4
#define FDT_IRQ_TYPE_LEVEL_LOW 8

enum {
    PHYTIUMPI_MEM,
    PHYTIUMPI_MMC0,
    PHYTIUMPI_MMC1,
    PHYTIUMPI_UART_FIRMWARE,
    PHYTIUMPI_UART_CONSOLE,
    PHYTIUMPI_GIC_DIST,
    PHYTIUMPI_GIC_ITS,
    PHYTIUMPI_GIC_CPU,
    PHYTIUMPI_GIC_HYP,
    PHYTIUMPI_GIC_VCPU,
    PHYTIUMPI_GIC_REDIST,
};

typedef struct PhytiumPiFirmwareWindow {
    const char *name;
    hwaddr base;
    uint64_t size;
} PhytiumPiFirmwareWindow;

typedef struct PhytiumPiUart {
    hwaddr base;
    unsigned int irq;
    bool console;
} PhytiumPiUart;

enum {
    PHYTIUMPI_FW_WIN_PAD,
    PHYTIUMPI_FW_WIN_LOW0,
    PHYTIUMPI_FW_WIN_LOW1,
    PHYTIUMPI_FW_WIN_LOW2,
    PHYTIUMPI_FW_WIN_LOW3,
    PHYTIUMPI_FW_WIN_LOW4,
    PHYTIUMPI_FW_WIN_USB2_CLUSTER,
    PHYTIUMPI_FW_WIN_MISC0,
    PHYTIUMPI_FW_WIN_PHY_CFG,
    PHYTIUMPI_FW_WIN_MISC1,
    PHYTIUMPI_FW_WIN_MISC2,
    PHYTIUMPI_FW_WIN_PHY_CFG1,
    PHYTIUMPI_FW_WIN_MISC3,
    PHYTIUMPI_FW_WIN_MISC4,
    PHYTIUMPI_FW_WIN_MISC5,
    PHYTIUMPI_FW_WIN_USB2_LOW,
    PHYTIUMPI_FW_WIN_USB2_HIGH,
    PHYTIUMPI_FW_WIN_MISC8,
    PHYTIUMPI_FW_WIN_MISC9,
    PHYTIUMPI_FW_WIN_MISC10,
    PHYTIUMPI_FW_WIN_MISC11,
    PHYTIUMPI_FW_WIN_MISC12,
    PHYTIUMPI_FW_WIN_MISC13,
    PHYTIUMPI_FW_WIN_MISC14,
    PHYTIUMPI_FW_WIN_MISC15,
    PHYTIUMPI_FW_WIN_COUNT,
};

static const PhytiumPiFirmwareWindow phytiumpi_fw_windows[] = {
    [PHYTIUMPI_FW_WIN_PAD] = {
        "phytium-pi.fw-pad", 0x28100000, 0x1000,
    },
    [PHYTIUMPI_FW_WIN_LOW0] = {
        "phytium-pi.fw-low0", 0x28002000, 0xb000,
    },
    [PHYTIUMPI_FW_WIN_LOW1] = {
        "phytium-pi.fw-low1", 0x28050000, 0x40000,
    },
    [PHYTIUMPI_FW_WIN_LOW2] = {
        "phytium-pi.fw-low2", 0x28010000, 0x10000,
    },
    [PHYTIUMPI_FW_WIN_LOW3] = {
        "phytium-pi.fw-low3", 0x28020000, 0xa000,
    },
    [PHYTIUMPI_FW_WIN_LOW4] = {
        "phytium-pi.fw-low4", 0x2802b000, 0x25000,
    },
    [PHYTIUMPI_FW_WIN_USB2_CLUSTER] = {
        "phytium-pi.fw-usb2-cluster", 0x31800000, 0x200000,
    },
    [PHYTIUMPI_FW_WIN_MISC0] = {
        "phytium-pi.fw-misc0", 0x31a40000, 0x10000,
    },
    [PHYTIUMPI_FW_WIN_PHY_CFG] = {
        "phytium-pi.fw-phy-cfg", 0x31b00000, 0x100000,
    },
    [PHYTIUMPI_FW_WIN_MISC1] = {
        "phytium-pi.fw-misc1", 0x31d00000, 0x1000,
    },
    [PHYTIUMPI_FW_WIN_MISC2] = {
        "phytium-pi.fw-phy-cfg0", 0x32000000, 0x100000,
    },
    [PHYTIUMPI_FW_WIN_PHY_CFG1] = {
        "phytium-pi.fw-phy-cfg1", 0x32100000, 0x400000,
    },
    [PHYTIUMPI_FW_WIN_MISC3] = {
        "phytium-pi.fw-misc3", 0x32500000, 0x1000,
    },
    [PHYTIUMPI_FW_WIN_MISC4] = {
        "phytium-pi.fw-misc4", 0x32f00000, 0x1000,
    },
    [PHYTIUMPI_FW_WIN_MISC5] = {
        "phytium-pi.fw-misc5", 0x31500000, 0x1000,
    },
    [PHYTIUMPI_FW_WIN_USB2_LOW] = {
        "phytium-pi.fw-usb2-low", 0x32800000, 0x80000,
    },
    [PHYTIUMPI_FW_WIN_USB2_HIGH] = {
        "phytium-pi.fw-usb2-high", 0x32880000, 0x80000,
    },
    [PHYTIUMPI_FW_WIN_MISC8] = {
        "phytium-pi.fw-misc8", 0x32900000, 0x1000,
    },
    [PHYTIUMPI_FW_WIN_MISC9] = {
        "phytium-pi.fw-misc9", 0x32b00000, 0x1000,
    },
    [PHYTIUMPI_FW_WIN_MISC10] = {
        "phytium-pi.fw-misc10", 0x32540000, 0x10000,
    },
    [PHYTIUMPI_FW_WIN_MISC11] = {
        "phytium-pi.fw-misc11", 0x32e40000, 0x10000,
    },
    [PHYTIUMPI_FW_WIN_MISC12] = {
        "phytium-pi.fw-misc12", 0x31100000, 0x10000,
    },
    [PHYTIUMPI_FW_WIN_MISC13] = {
        "phytium-pi.fw-misc13", 0x31000000, 0x40000,
    },
    [PHYTIUMPI_FW_WIN_MISC14] = {
        "phytium-pi.fw-misc14", 0x31c40000, 0x10000,
    },
    [PHYTIUMPI_FW_WIN_MISC15] = {
        "phytium-pi.fw-misc15", 0x31a00000, 0x40000,
    },
};

struct PhytiumPiMachineState {
    MachineState parent;
    ARMCPU *cpu[PHYTIUMPI_MAX_CPUS];
    DeviceState *gic;
    MemoryRegion ram_low;
    MemoryRegion ram_high;
    MemoryRegion bootrom;
    MemoryRegion firmware_ram;
    MemoryRegion firmware_high_ram;
    MemoryRegion firmware_ctrl;
    MemoryRegion sys_ctrl;
    MemoryRegion scp_ctrl;
    MemoryRegion scp_sram;
    MemoryRegion firmware_misc;
    MemoryRegion firmware_cfg;
    MemoryRegion firmware_windows[PHYTIUMPI_FW_WIN_COUNT];
    struct arm_boot_info bootinfo;
};

static const MemMapEntry phytiumpi_memmap[] = {
    [PHYTIUMPI_MEM] =          { PHYTIUMPI_RAM_LOW_BASE,
                                 PHYTIUMPI_RAM_LOW_SIZE },
    [PHYTIUMPI_MMC0] =         { 0x28000000, 0x1000 },
    [PHYTIUMPI_MMC1] =         { 0x28001000, 0x1000 },
    [PHYTIUMPI_UART_FIRMWARE] = { 0x2800d000, 0x1000 },
    [PHYTIUMPI_UART_CONSOLE] = { 0x2802a000, 0x1000 },
    [PHYTIUMPI_GIC_DIST] =     { 0x30800000, 0x20000 },
    [PHYTIUMPI_GIC_ITS] =      { 0x30820000, 0x20000 },
    [PHYTIUMPI_GIC_CPU] =      { 0x30840000, 0x10000 },
    [PHYTIUMPI_GIC_HYP] =      { 0x30850000, 0x10000 },
    [PHYTIUMPI_GIC_VCPU] =     { 0x30860000, 0x10000 },
    [PHYTIUMPI_GIC_REDIST] =   { 0x30880000, 0x80000 },
};

static const uint64_t phytiumpi_mpidr[PHYTIUMPI_MAX_CPUS] = {
    0x000, 0x100, 0x200, 0x201,
};

static const char * const phytiumpi_cpu_compat[PHYTIUMPI_MAX_CPUS] = {
    "phytium,ftc664",
    "phytium,ftc664",
    "phytium,ftc310",
    "phytium,ftc310",
};

static const PhytiumPiUart phytiumpi_uarts[] = {
    { 0x2800c000, 83, false },
    { 0x2800d000, 84, true },
    { 0x2800e000, 85, false },
    { 0x2800f000, 86, false },
    { 0x28014000, 92, false },
    { 0x2802a000, 103, false },
    { 0x28032000, 107, false },
};

static const ARMCPRegInfo phytiumpi_impdef_cp_reginfo[] = {
    { .name = "PHYTIUMPI_IMPDEF_C15_C1_0",
      .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 1, .crn = 15, .crm = 1, .opc2 = 0,
      .access = PL3_RW,
      .type = ARM_CP_NOP | ARM_CP_NO_RAW },
    { .name = "PHYTIUMPI_IMPDEF_C11_C8_6",
      .state = ARM_CP_STATE_AA64,
      .opc0 = 3, .opc1 = 1, .crn = 11, .crm = 8, .opc2 = 6,
      .access = PL3_RW,
      .type = ARM_CP_NOP | ARM_CP_NO_RAW },
};

static void phytiumpi_create_cpus(PhytiumPiMachineState *s)
{
    MachineState *ms = MACHINE(s);
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);
    MemoryRegion *sysmem = get_system_memory();

    for (unsigned int n = 0; n < ms->smp.cpus; n++) {
        g_autofree char *name = g_strdup_printf("cpu%u", n);
        Object *cpuobj = object_new(possible_cpus->cpus[n].type);
        CPUState *cs = CPU(cpuobj);

        object_property_add_child(OBJECT(ms), name, cpuobj);
        cs->cpu_index = n;
        numa_cpu_pre_plug(&possible_cpus->cpus[n], DEVICE(cpuobj),
                          &error_fatal);
        object_property_set_int(cpuobj, "mp-affinity",
                                possible_cpus->cpus[n].arch_id,
                                &error_abort);
        object_property_set_int(cpuobj, "cntfrq", PHYTIUMPI_GTIMER_HZ,
                                &error_abort);
        object_property_set_link(cpuobj, "memory", OBJECT(sysmem),
                                 &error_abort);

        if (object_property_find(cpuobj, "reset-cbar")) {
            object_property_set_int(cpuobj, "reset-cbar",
                                    phytiumpi_memmap[PHYTIUMPI_GIC_DIST].base,
                                    &error_abort);
        }
        if (object_property_find(cpuobj, "has_el2")) {
            object_property_set_bool(cpuobj, "has_el2", !kvm_enabled(),
                                     &error_abort);
        }
        if (object_property_find(cpuobj, "has_el3")) {
            object_property_set_bool(cpuobj, "has_el3", !kvm_enabled(),
                                     &error_abort);
        }

        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        define_arm_cp_regs(ARM_CPU(cpuobj), phytiumpi_impdef_cp_reginfo);
        s->cpu[n] = ARM_CPU(cpuobj);
    }
}

static void phytiumpi_enable_psci_conduit(PhytiumPiMachineState *s)
{
    MachineState *ms = MACHINE(s);

    for (unsigned int n = 0; n < ms->smp.cpus; n++) {
        object_property_set_int(OBJECT(s->cpu[n]), "psci-conduit",
                                QEMU_PSCI_CONDUIT_SMC, &error_abort);
        if (n > 0) {
            object_property_set_bool(OBJECT(s->cpu[n]), "start-powered-off",
                                     true, &error_abort);
        }
    }
}

static void
phytiumpi_prepare_nonsecure_linux_interrupts(PhytiumPiMachineState *s)
{
    ARMLinuxBootIf *albif = ARM_LINUX_BOOT_IF(s->gic);
    ARMLinuxBootIfClass *albifc = ARM_LINUX_BOOT_IF_GET_CLASS(albif);

    /*
     * The firmware path bypasses arm_load_kernel(), so run the same GIC
     * Linux-init hook used by direct kernel boot.  This models secure
     * firmware handing interrupt ownership to the NonSecure kernel before
     * U-Boot eventually jumps to Linux.
     */
    if (albifc->arm_linux_init) {
        albifc->arm_linux_init(albif, false);
    }
    device_cold_reset(s->gic);
}

static void phytiumpi_create_gic(PhytiumPiMachineState *s)
{
    MachineState *ms = MACHINE(s);
    SysBusDevice *gicbusdev;
    QList *redist_region_count;
    uint32_t redist_capacity;

    s->gic = qdev_new(gicv3_class_name());
    qdev_prop_set_uint32(s->gic, "revision", 3);
    qdev_prop_set_uint32(s->gic, "num-cpu", ms->smp.cpus);
    qdev_prop_set_uint32(s->gic, "num-irq",
                         PHYTIUMPI_NUM_SPI_IRQS + GIC_INTERNAL);
    qdev_prop_set_bit(s->gic, "has-security-extensions", true);

    redist_capacity = phytiumpi_memmap[PHYTIUMPI_GIC_REDIST].size /
                      GICV3_REDIST_SIZE;
    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, MIN(ms->smp.cpus, redist_capacity));
    qdev_prop_set_array(s->gic, "redist-region-count", redist_region_count);
    object_property_set_link(OBJECT(s->gic), "sysmem",
                             OBJECT(get_system_memory()), &error_fatal);

    gicbusdev = SYS_BUS_DEVICE(s->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, phytiumpi_memmap[PHYTIUMPI_GIC_DIST].base);
    sysbus_mmio_map(gicbusdev, 1, phytiumpi_memmap[PHYTIUMPI_GIC_REDIST].base);

    for (unsigned int n = 0; n < ms->smp.cpus; n++) {
        DeviceState *cpudev = DEVICE(s->cpu[n]);
        int intidbase = PHYTIUMPI_NUM_SPI_IRQS + n * GIC_INTERNAL;
        static const int timer_irqs[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP] = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC] = ARCH_TIMER_S_EL1_IRQ,
            [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ,
            [GTIMER_S_EL2_PHYS] = ARCH_TIMER_S_EL2_IRQ,
            [GTIMER_S_EL2_VIRT] = ARCH_TIMER_S_EL2_VIRT_IRQ,
        };

        for (int irq = 0; irq < ARRAY_SIZE(timer_irqs); irq++) {
            qdev_connect_gpio_out(cpudev, irq,
                                  qdev_get_gpio_in(s->gic,
                                                   intidbase +
                                                   timer_irqs[irq]));
        }

        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(s->gic,
                                                     intidbase +
                                                     ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(s->gic,
                                                     intidbase +
                                                     VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicbusdev, n, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, n + ms->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, n + 2 * ms->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, n + 3 * ms->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
}

static void phytiumpi_create_uart(PhytiumPiMachineState *s)
{
    for (int i = 0; i < ARRAY_SIZE(phytiumpi_uarts); i++) {
        const PhytiumPiUart *uart = &phytiumpi_uarts[i];
        DeviceState *dev = qdev_new(TYPE_PL011);
        SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
        MemoryRegion *mmio;

        if (uart->console) {
            qdev_prop_set_chr(dev, "chardev", serial_hd(0));
        }
        sysbus_realize_and_unref(sbd, &error_fatal);
        mmio = sysbus_mmio_get_region(sbd, 0);
        memory_region_add_subregion(get_system_memory(), uart->base, mmio);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, uart->irq));
    }
}

static DeviceState *phytiumpi_create_mmc_controller(PhytiumPiMachineState *s,
                                                    const char *name,
                                                    unsigned int map,
                                                    unsigned int irq)
{
    DeviceState *dev = qdev_new(TYPE_PHYTIUM_MCI);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    sysbus_realize_and_unref(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, phytiumpi_memmap[map].base);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, irq));

    return dev;
}

static void phytiumpi_create_mmc(PhytiumPiMachineState *s)
{
    DeviceState *mci0;
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;

    mci0 = phytiumpi_create_mmc_controller(s, "mci0", PHYTIUMPI_MMC0, 72);
    phytiumpi_create_mmc_controller(s, "mci1", PHYTIUMPI_MMC1, 73);

    if (di) {
        BusState *bus = qdev_get_child_bus(mci0, "sd-bus");
        DeviceState *card;

        if (!bus) {
            error_report("phytium-pi: MCI controller has no sd-bus");
            exit(EXIT_FAILURE);
        }

        card = qdev_new(TYPE_SD_CARD);
        qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
        qdev_realize_and_unref(card, bus, &error_fatal);
    }
}

static void phytiumpi_create_xmac(PhytiumPiMachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_PHYTIUM_XMAC);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    MemoryRegion *mmio;

    object_property_add_child(OBJECT(s), "xmac0", OBJECT(dev));
    sysbus_realize_and_unref(sbd, &error_fatal);
    mmio = sysbus_mmio_get_region(sbd, 0);
    memory_region_add_subregion_overlap(get_system_memory(),
                                        PHYTIUMPI_XMAC_BASE, mmio, 2);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic,
                                                PHYTIUMPI_XMAC_IRQ));
}

static void phytiumpi_create_overlap_mmio(Object *owner, const char *name,
                                          const char *type, hwaddr base,
                                          unsigned int priority)
{
    DeviceState *dev = qdev_new(type);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    MemoryRegion *mmio;

    object_property_add_child(owner, name, OBJECT(dev));
    sysbus_realize_and_unref(sbd, &error_fatal);
    mmio = sysbus_mmio_get_region(sbd, 0);
    memory_region_add_subregion_overlap(get_system_memory(), base, mmio,
                                        priority);
}

static void phytiumpi_create_scp_mailbox(PhytiumPiMachineState *s)
{
    phytiumpi_create_overlap_mmio(OBJECT(s), "scp-mailbox",
                                  TYPE_PHYTIUM_SCP_MAILBOX,
                                  PHYTIUMPI_SCP_MAILBOX_BASE, 1);
}

static void phytiumpi_create_ddr_ctrl(PhytiumPiMachineState *s)
{
    phytiumpi_create_overlap_mmio(OBJECT(s), "ddr-ctrl",
                                  TYPE_PHYTIUM_DDR_CTRL,
                                  PHYTIUMPI_DDR_CTRL_BASE, 1);
}

static void phytiumpi_create_bootrom(PhytiumPiMachineState *s)
{
    memory_region_init_ram(&s->bootrom, NULL, "phytium-pi.bootrom",
                           PHYTIUMPI_BOOTROM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), PHYTIUMPI_BOOTROM_BASE,
                                &s->bootrom);
}

static void phytiumpi_map_ram(PhytiumPiMachineState *s, MachineState *machine)
{
    uint64_t low_size = MIN(machine->ram_size,
                            (uint64_t)PHYTIUMPI_RAM_LOW_SIZE);
    uint64_t high_size = machine->ram_size - low_size;

    if (low_size) {
        memory_region_init_alias(&s->ram_low, OBJECT(s),
                                 "phytium-pi.ram-low", machine->ram, 0,
                                 low_size);
        memory_region_add_subregion(get_system_memory(),
                                    PHYTIUMPI_RAM_LOW_BASE, &s->ram_low);
    }

    if (high_size) {
        memory_region_init_alias(&s->ram_high, OBJECT(s),
                                 "phytium-pi.ram-high", machine->ram,
                                 PHYTIUMPI_RAM_LOW_SIZE, high_size);
        memory_region_add_subregion(get_system_memory(),
                                    PHYTIUMPI_RAM_HIGH_BASE, &s->ram_high);
    }
}

static void phytiumpi_create_firmware_ram(PhytiumPiMachineState *s)
{
    uint8_t *fw_ram;
    uint8_t *fw_ctrl;
    uint8_t *scp_sram;

    memory_region_init_ram(&s->firmware_ram, NULL, "phytium-pi.firmware-ram",
                           PHYTIUMPI_FW_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), PHYTIUMPI_FW_RAM_BASE,
                                &s->firmware_ram);

    memory_region_init_ram(&s->firmware_high_ram, NULL,
                           "phytium-pi.firmware-high-ram",
                           PHYTIUMPI_FW_HIGH_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), PHYTIUMPI_FW_HIGH_RAM_BASE,
                                &s->firmware_high_ram);

    memory_region_init_ram(&s->firmware_ctrl, NULL, "phytium-pi.firmware-ctrl",
                           PHYTIUMPI_FW_CTRL_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), PHYTIUMPI_FW_CTRL_BASE,
                                &s->firmware_ctrl);

    memory_region_init_ram(&s->sys_ctrl, NULL, "phytium-pi.sys-ctrl",
                           PHYTIUMPI_SYS_CTRL_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), PHYTIUMPI_SYS_CTRL_BASE,
                                &s->sys_ctrl);

    memory_region_init_ram(&s->scp_ctrl, NULL, "phytium-pi.scp-ctrl",
                           PHYTIUMPI_SCP_CTRL_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), PHYTIUMPI_SCP_CTRL_BASE,
                                &s->scp_ctrl);

    memory_region_init_ram(&s->scp_sram, NULL, "phytium-pi.scp-sram",
                           PHYTIUMPI_SCP_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), PHYTIUMPI_SCP_SRAM_BASE,
                                &s->scp_sram);

    phytiumpi_create_scp_mailbox(s);

    memory_region_init_ram(&s->firmware_misc, NULL, "phytium-pi.firmware-misc",
                           PHYTIUMPI_FW_MISC_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), PHYTIUMPI_FW_MISC_BASE,
                                &s->firmware_misc);
    phytiumpi_create_ddr_ctrl(s);

    memory_region_init_ram(&s->firmware_cfg, NULL, "phytium-pi.firmware-cfg",
                           PHYTIUMPI_FW_CFG_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), PHYTIUMPI_FW_CFG_BASE,
                                &s->firmware_cfg);

    for (unsigned int n = 0; n < ARRAY_SIZE(phytiumpi_fw_windows); n++) {
        memory_region_init_ram(&s->firmware_windows[n], NULL,
                               phytiumpi_fw_windows[n].name,
                               phytiumpi_fw_windows[n].size, &error_fatal);
        memory_region_add_subregion(get_system_memory(),
                                    phytiumpi_fw_windows[n].base,
                                    &s->firmware_windows[n]);
    }

    scp_sram = memory_region_get_ram_ptr(&s->scp_sram);
    stl_le_p(scp_sram + PHYTIUMPI_SCP_READY_OFFSET,
             PHYTIUMPI_SCP_READY_MAGIC);

    fw_ram = memory_region_get_ram_ptr(&s->firmware_ram);
    stl_le_p(fw_ram + PHYTIUMPI_FW_SYNC_OFFSET, PHYTIUMPI_FW_SYNC_MAGIC);

    fw_ctrl = memory_region_get_ram_ptr(&s->firmware_ctrl);
    stq_le_p(fw_ctrl + PHYTIUMPI_FW_CTRL_RESET_PTR_OFFSET,
             PHYTIUMPI_FW_CTRL_BASE + PHYTIUMPI_FW_CTRL_RESET_MAGIC_OFFSET);
    stq_le_p(fw_ctrl + PHYTIUMPI_FW_CTRL_RESET_MAGIC_OFFSET,
             PHYTIUMPI_FW_CTRL_RESET_MAGIC);
    stq_le_p(fw_ctrl + PHYTIUMPI_FW_CTRL_UART_OFFSET,
             phytiumpi_memmap[PHYTIUMPI_UART_FIRMWARE].base);
    stq_le_p(fw_ctrl + PHYTIUMPI_FW_CTRL_PTR_OFFSET,
             PHYTIUMPI_FW_CTRL_PTR_VALUE);
}

static void phytiumpi_patch_firmware_ddr_training(uint8_t *fw_ram)
{
    uint32_t load = ldl_le_p(fw_ram + PHYTIUMPI_UBOOT_DDR_RESULT_LOAD_OFFSET);
    uint32_t check = ldl_le_p(fw_ram +
                              PHYTIUMPI_UBOOT_DDR_RESULT_CHECK_OFFSET);

    /*
     * The local model only needs the vendor firmware to advance past DDR
     * training.  When this known U-Boot sequence is present, bypass the
     * post-SMC training error path instead of modelling the full DDR PHY.
     */
    if (load == PHYTIUMPI_UBOOT_DDR_RESULT_LOAD &&
        check == PHYTIUMPI_UBOOT_DDR_RESULT_CHECK) {
        stl_le_p(fw_ram + PHYTIUMPI_UBOOT_DDR_RESULT_CHECK_OFFSET,
                 PHYTIUMPI_UBOOT_DDR_RESULT_SKIP);
    } else if (check != PHYTIUMPI_UBOOT_DDR_RESULT_SKIP) {
        warn_report("phytium-pi: DDR training firmware patch not applied");
    }
}

static bool phytiumpi_is_psci_smc(uint64_t fn)
{
    uint32_t fid = fn;

    switch (fid) {
    case QEMU_SMCCC_VERSION_FUNC_ID:
    case QEMU_PSCI_0_2_FN_PSCI_VERSION:
    case QEMU_PSCI_0_2_FN_CPU_SUSPEND:
    case QEMU_PSCI_0_2_FN_CPU_OFF:
    case QEMU_PSCI_0_2_FN_CPU_ON:
    case QEMU_PSCI_0_2_FN_AFFINITY_INFO:
    case QEMU_PSCI_0_2_FN_MIGRATE:
    case QEMU_PSCI_0_2_FN_MIGRATE_INFO_TYPE:
    case QEMU_PSCI_0_2_FN_SYSTEM_OFF:
    case QEMU_PSCI_0_2_FN_SYSTEM_RESET:
    case QEMU_PSCI_1_0_FN_PSCI_FEATURES:
    case QEMU_PSCI_0_2_FN64_CPU_SUSPEND:
    case QEMU_PSCI_0_2_FN64_CPU_OFF:
    case QEMU_PSCI_0_2_FN64_CPU_ON:
    case QEMU_PSCI_0_2_FN64_AFFINITY_INFO:
    case QEMU_PSCI_0_2_FN64_MIGRATE:
    case QEMU_PSCI_0_1_FN_CPU_SUSPEND:
    case QEMU_PSCI_0_1_FN_CPU_OFF:
    case QEMU_PSCI_0_1_FN_CPU_ON:
    case QEMU_PSCI_0_1_FN_MIGRATE:
        return true;
    default:
        return false;
    }
}

static void phytiumpi_smc_set_ret(ARMCPU *cpu, int32_t ret)
{
    CPUARMState *env = &cpu->env;

    if (is_a64(env)) {
        env->xregs[0] = (uint64_t)(int64_t)ret;
    } else {
        env->regs[0] = ret;
    }
}

static bool phytiumpi_handle_smccc_arch_call(ARMCPU *cpu, uint32_t fid)
{
    CPUARMState *env = &cpu->env;
    uint64_t arg1 = is_a64(env) ? env->xregs[1] : env->regs[1];

    switch (fid) {
    case PHYTIUMPI_SMCCC_ARCH_FEATURES:
        switch ((uint32_t)arg1) {
        case PHYTIUMPI_SMCCC_ARCH_WORKAROUND_1:
            phytiumpi_smc_set_ret(cpu, PHYTIUMPI_SMCCC_RET_SUCCESS);
            break;
        case PHYTIUMPI_SMCCC_ARCH_WORKAROUND_2:
        case PHYTIUMPI_SMCCC_ARCH_WORKAROUND_3:
        default:
            phytiumpi_smc_set_ret(cpu, PHYTIUMPI_SMCCC_RET_NOT_SUPPORTED);
            break;
        }
        return true;
    case PHYTIUMPI_SMCCC_ARCH_SOC_ID:
        phytiumpi_smc_set_ret(cpu, PHYTIUMPI_SMCCC_RET_NOT_SUPPORTED);
        return true;
    case PHYTIUMPI_SMCCC_ARCH_WORKAROUND_1:
        phytiumpi_smc_set_ret(cpu, PHYTIUMPI_SMCCC_RET_SUCCESS);
        return true;
    case PHYTIUMPI_SMCCC_ARCH_WORKAROUND_2:
    case PHYTIUMPI_SMCCC_ARCH_WORKAROUND_3:
        phytiumpi_smc_set_ret(cpu, PHYTIUMPI_SMCCC_RET_NOT_SUPPORTED);
        return true;
    default:
        return false;
    }
}

static bool phytiumpi_smc_handler(ARMCPU *cpu)
{
    CPUARMState *env = &cpu->env;
    uint64_t fn = is_a64(env) ? env->xregs[0] : env->regs[0];
    uint32_t fid = fn;

    if (phytiumpi_handle_smccc_arch_call(cpu, fid)) {
        return true;
    }
    if (!phytiumpi_is_psci_smc(fid)) {
        return false;
    }

    arm_handle_psci_call(cpu);
    return true;
}

static bool phytiumpi_bootrom_prepare(PhytiumPiMachineState *s, Error **errp)
{
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;
    uint8_t *rom = memory_region_get_ram_ptr(&s->bootrom);
    uint8_t *fw_ram = memory_region_get_ram_ptr(&s->firmware_ram);
    int ret;

    if (!blk) {
        error_setg(errp, "phytium-pi firmware boot requires "
                   "-drive if=sd,index=0,file=<phytium-pi-genimage>,format=raw");
        return false;
    }

    ret = blk_pread(blk, 0, PHYTIUMPI_BOOTROM_SIZE, rom, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "failed to read Phytium Pi SD boot area");
        return false;
    }

    memcpy(fw_ram, rom, PHYTIUMPI_BOOTROM_SIZE);
    stl_le_p(fw_ram + PHYTIUMPI_FW_SYNC_OFFSET, PHYTIUMPI_FW_SYNC_MAGIC);
    phytiumpi_patch_firmware_ddr_training(fw_ram);

    return true;
}

typedef struct PhytiumPiFirmwareReset {
    PhytiumPiMachineState *machine;
    ARMCPU *cpu;
    unsigned int index;
} PhytiumPiFirmwareReset;

static void phytiumpi_firmware_cpu_reset(void *opaque)
{
    PhytiumPiFirmwareReset *rst = opaque;
    ARMCPU *cpu = rst->cpu;
    CPUState *cs = CPU(cpu);
    CPUARMState *env = &cpu->env;

    cpu_reset(cs);

    if (rst->index != PHYTIUMPI_FIRMWARE_BOOT_CPU_INDEX) {
        arm_set_cpu_power_state(cpu, PSCI_OFF);
        cs->halted = true;
        cs->exception_index = EXCP_HLT;
        return;
    }

    arm_emulate_firmware_reset(cs, 3);
    cpu_set_pc(cs, PHYTIUMPI_FW_RAM_BASE + PHYTIUMPI_FIRMWARE_ENTRY_OFFSET);
    env->xregs[31] = PHYTIUMPI_FW_RAM_BASE + PHYTIUMPI_FW_RAM_SIZE -
                     0x100 - rst->index * KiB;
    env->sp_el[3] = env->xregs[31];
    arm_rebuild_hflags(env);
}

static void phytiumpi_register_firmware_reset(PhytiumPiMachineState *s)
{
    MachineState *ms = MACHINE(s);

    for (unsigned int n = 0; n < ms->smp.cpus; n++) {
        PhytiumPiFirmwareReset *rst = g_new0(PhytiumPiFirmwareReset, 1);

        rst->machine = s;
        rst->cpu = s->cpu[n];
        rst->index = n;
        qemu_register_reset(phytiumpi_firmware_cpu_reset, rst);
    }
}

static void phytiumpi_create_unimplemented_regions(void)
{
    create_unimplemented_device("phytium-pi.gic-its",
                                phytiumpi_memmap[PHYTIUMPI_GIC_ITS].base,
                                phytiumpi_memmap[PHYTIUMPI_GIC_ITS].size);
    create_unimplemented_device("phytium-pi.gic-cpu",
                                phytiumpi_memmap[PHYTIUMPI_GIC_CPU].base,
                                phytiumpi_memmap[PHYTIUMPI_GIC_CPU].size);
    create_unimplemented_device("phytium-pi.gic-hyp",
                                phytiumpi_memmap[PHYTIUMPI_GIC_HYP].base,
                                phytiumpi_memmap[PHYTIUMPI_GIC_HYP].size);
    create_unimplemented_device("phytium-pi.gic-vcpu",
                                phytiumpi_memmap[PHYTIUMPI_GIC_VCPU].base,
                                phytiumpi_memmap[PHYTIUMPI_GIC_VCPU].size);
    create_unimplemented_device("phytium-pi.pcie-ecam",
                                PHYTIUMPI_PCIE_ECAM_BASE,
                                PHYTIUMPI_PCIE_ECAM_SIZE);
    create_unimplemented_device("phytium-pi.rng",
                                PHYTIUMPI_RNG_BASE, PHYTIUMPI_RNG_SIZE);
}

static void phytiumpi_fdt_add_cpu_nodes(PhytiumPiMachineState *s, void *fdt)
{
    MachineState *ms = MACHINE(s);

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0);

    for (int n = ms->smp.cpus - 1; n >= 0; n--) {
        g_autofree char *nodename = g_strdup_printf("/cpus/cpu@%" PRIx64,
                                                    phytiumpi_mpidr[n]);
        const char * const compat[] = {
            phytiumpi_cpu_compat[n],
            "arm,armv8",
        };

        qemu_fdt_add_subnode(fdt, nodename);
        qemu_fdt_setprop_string(fdt, nodename, "device_type", "cpu");
        qemu_fdt_setprop_string_array(fdt, nodename, "compatible",
                                      (char **)&compat, ARRAY_SIZE(compat));
        qemu_fdt_setprop_u64(fdt, nodename, "reg", phytiumpi_mpidr[n]);
        qemu_fdt_setprop_string(fdt, nodename, "enable-method", "psci");
    }
}

static uint32_t phytiumpi_fdt_add_fixed_clock(void *fdt, const char *name,
                                              uint32_t hz)
{
    g_autofree char *node = g_strdup_printf("/%s", name);
    uint32_t phandle = qemu_fdt_alloc_phandle(fdt);

    qemu_fdt_add_subnode(fdt, node);
    qemu_fdt_setprop_string(fdt, node, "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(fdt, node, "#clock-cells", 0);
    qemu_fdt_setprop_cell(fdt, node, "clock-frequency", hz);
    qemu_fdt_setprop_cell(fdt, node, "phandle", phandle);

    return phandle;
}

static void phytiumpi_fdt_add_gic_node(void *fdt)
{
    const char *gic = "/interrupt-controller@30800000";
    uint32_t phandle = qemu_fdt_alloc_phandle(fdt);

    qemu_fdt_add_subnode(fdt, gic);
    qemu_fdt_setprop_string(fdt, gic, "compatible", "arm,gic-v3");
    qemu_fdt_setprop_cell(fdt, gic, "#interrupt-cells", 3);
    qemu_fdt_setprop_cell(fdt, gic, "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, gic, "#size-cells", 2);
    qemu_fdt_setprop(fdt, gic, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop(fdt, gic, "ranges", NULL, 0);
    qemu_fdt_setprop_sized_cells(fdt, gic, "reg",
                                 2, phytiumpi_memmap[PHYTIUMPI_GIC_DIST].base,
                                 2, phytiumpi_memmap[PHYTIUMPI_GIC_DIST].size,
                                 2,
                                 phytiumpi_memmap[PHYTIUMPI_GIC_REDIST].base,
                                 2,
                                 phytiumpi_memmap[PHYTIUMPI_GIC_REDIST].size);
    qemu_fdt_setprop_cells(fdt, gic, "interrupts",
                           FDT_GIC_PPI, 9, FDT_IRQ_TYPE_LEVEL_LOW);
    qemu_fdt_setprop_cell(fdt, gic, "phandle", phandle);
    qemu_fdt_setprop_cell(fdt, "/", "interrupt-parent", phandle);
}

static void phytiumpi_fdt_add_timer_node(void *fdt)
{
    qemu_fdt_add_subnode(fdt, "/timer");
    qemu_fdt_setprop_string(fdt, "/timer", "compatible", "arm,armv8-timer");
    qemu_fdt_setprop_cells(fdt, "/timer", "interrupts",
                           FDT_GIC_PPI, 13, FDT_IRQ_TYPE_LEVEL_HIGH,
                           FDT_GIC_PPI, 14, FDT_IRQ_TYPE_LEVEL_HIGH,
                           FDT_GIC_PPI, 11, FDT_IRQ_TYPE_LEVEL_HIGH,
                           FDT_GIC_PPI, 10, FDT_IRQ_TYPE_LEVEL_HIGH);
    qemu_fdt_setprop_cell(fdt, "/timer", "clock-frequency",
                          PHYTIUMPI_GTIMER_HZ);
    qemu_fdt_setprop(fdt, "/timer", "always-on", NULL, 0);
}

static void phytiumpi_fdt_add_uart_node(void *fdt, uint32_t clk_phandle)
{
    const char *uart = "/soc/serial@2800d000";
    static const char compat[] = "arm,pl011\0arm,primecell";
    static const char clock_names[] = "uartclk\0apb_pclk";

    qemu_fdt_add_subnode(fdt, "/aliases");
    qemu_fdt_setprop_string(fdt, "/aliases", "serial1", uart);

    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", "serial1:115200n8");

    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 2);
    qemu_fdt_setprop(fdt, "/soc", "ranges", NULL, 0);

    qemu_fdt_add_subnode(fdt, uart);
    qemu_fdt_setprop(fdt, uart, "compatible", compat, sizeof(compat));
    qemu_fdt_setprop_sized_cells(fdt, uart, "reg",
                                 2,
                                 phytiumpi_memmap[PHYTIUMPI_UART_FIRMWARE].base,
                                 2,
                                 phytiumpi_memmap[PHYTIUMPI_UART_FIRMWARE].size);
    qemu_fdt_setprop_cells(fdt, uart, "interrupts",
                           FDT_GIC_SPI, 84, FDT_IRQ_TYPE_LEVEL_HIGH);
    qemu_fdt_setprop_cells(fdt, uart, "clocks", clk_phandle, clk_phandle);
    qemu_fdt_setprop(fdt, uart, "clock-names", clock_names,
                     sizeof(clock_names));
    qemu_fdt_setprop_string(fdt, uart, "status", "okay");
}

static void phytiumpi_fdt_add_mmc_node(void *fdt, uint32_t clk_phandle)
{
    const char *mmc = "/soc/mmc@28000000";
    static const char clock_names[] = "phytium_mci_clk";

    qemu_fdt_setprop_string(fdt, "/aliases", "mmc0", mmc);

    qemu_fdt_add_subnode(fdt, mmc);
    qemu_fdt_setprop_string(fdt, mmc, "compatible", "phytium,mci");
    qemu_fdt_setprop_sized_cells(fdt, mmc, "reg",
                                 2, phytiumpi_memmap[PHYTIUMPI_MMC0].base,
                                 2, phytiumpi_memmap[PHYTIUMPI_MMC0].size);
    qemu_fdt_setprop_cells(fdt, mmc, "interrupts",
                           FDT_GIC_SPI, 72, FDT_IRQ_TYPE_LEVEL_HIGH);
    qemu_fdt_setprop_cell(fdt, mmc, "clocks", clk_phandle);
    qemu_fdt_setprop(fdt, mmc, "clock-names", clock_names,
                     sizeof(clock_names));
    qemu_fdt_setprop_cell(fdt, mmc, "bus-width", 4);
    qemu_fdt_setprop_cell(fdt, mmc, "max-frequency", 50000000);
    qemu_fdt_setprop(fdt, mmc, "cap-sdio-irq", NULL, 0);
    qemu_fdt_setprop(fdt, mmc, "cap-sd-highspeed", NULL, 0);
    qemu_fdt_setprop(fdt, mmc, "sd-uhs-sdr12", NULL, 0);
    qemu_fdt_setprop(fdt, mmc, "sd-uhs-sdr25", NULL, 0);
    qemu_fdt_setprop(fdt, mmc, "sd-uhs-sdr50", NULL, 0);
    qemu_fdt_setprop(fdt, mmc, "no-mmc", NULL, 0);
    qemu_fdt_setprop_string(fdt, mmc, "status", "okay");
}

static void *phytiumpi_get_dtb(const struct arm_boot_info *binfo,
                               int *fdt_size)
{
    PhytiumPiMachineState *s = container_of(binfo, PhytiumPiMachineState,
                                            bootinfo);
    void *fdt = create_device_tree(fdt_size);
    uint32_t clk_50mhz;
    static const char * const root_compat[] = {
        "phytium,pe2204",
        "phytium,pe220x",
    };

    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(EXIT_FAILURE);
    }

    qemu_fdt_setprop_string(fdt, "/", "model", "Phytium Pi Board");
    qemu_fdt_setprop_string_array(fdt, "/", "compatible",
                                  (char **)root_compat,
                                  ARRAY_SIZE(root_compat));
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 2);

    clk_50mhz = phytiumpi_fdt_add_fixed_clock(fdt, "clk50mhz",
                                              PHYTIUMPI_GTIMER_HZ);

    phytiumpi_fdt_add_cpu_nodes(s, fdt);
    phytiumpi_fdt_add_gic_node(fdt);
    phytiumpi_fdt_add_timer_node(fdt);
    phytiumpi_fdt_add_uart_node(fdt, clk_50mhz);
    phytiumpi_fdt_add_mmc_node(fdt, clk_50mhz);

    return fdt;
}

static void phytiumpi_init(MachineState *machine)
{
    PhytiumPiMachineState *s = PHYTIUMPI_MACHINE(machine);

    if (machine->smp.cpus > PHYTIUMPI_MAX_CPUS ||
        machine->smp.max_cpus > PHYTIUMPI_MAX_CPUS) {
        error_report("phytium-pi: at most %u CPUs are supported",
                     PHYTIUMPI_MAX_CPUS);
        exit(EXIT_FAILURE);
    }

    if (machine->ram_size > PHYTIUMPI_RAM_MAX_SIZE) {
        g_autofree char *sz = size_to_str(PHYTIUMPI_RAM_MAX_SIZE);
        error_report("phytium-pi: RAM size must not exceed %s", sz);
        exit(EXIT_FAILURE);
    }

    phytiumpi_create_cpus(s);
    phytiumpi_create_bootrom(s);
    phytiumpi_create_firmware_ram(s);
    phytiumpi_map_ram(s, machine);
    phytiumpi_create_gic(s);
    phytiumpi_create_unimplemented_regions();
    phytiumpi_create_uart(s);
    phytiumpi_create_mmc(s);
    phytiumpi_create_xmac(s);

    s->bootinfo = (struct arm_boot_info) {
        .loader_start = phytiumpi_memmap[PHYTIUMPI_MEM].base,
        .board_id = -1,
        .ram_size = machine->ram_size,
        .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
        .get_dtb = phytiumpi_get_dtb,
    };

    if (qtest_enabled()) {
        return;
    }

    if (machine->kernel_filename) {
        phytiumpi_enable_psci_conduit(s);
        arm_load_kernel(s->cpu[0], machine, &s->bootinfo);
    } else {
        Error *local_err = NULL;

        if (machine->smp.cpus <= PHYTIUMPI_FIRMWARE_BOOT_CPU_INDEX) {
            error_report("phytium-pi firmware boot requires at least %u CPUs",
                         PHYTIUMPI_FIRMWARE_BOOT_CPU_INDEX + 1);
            exit(EXIT_FAILURE);
        }
        if (!phytiumpi_bootrom_prepare(s, &local_err)) {
            error_report_err(local_err);
            exit(EXIT_FAILURE);
        }
        phytiumpi_prepare_nonsecure_linux_interrupts(s);
        arm_register_psci_smc_handler(phytiumpi_smc_handler);
        phytiumpi_register_firmware_reset(s);
    }
}

static const CPUArchIdList *phytiumpi_possible_cpu_arch_ids(MachineState *ms)
{
    unsigned int max_cpus = ms->smp.max_cpus;

    if (ms->possible_cpus) {
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;

    for (unsigned int n = 0; n < max_cpus; n++) {
        ms->possible_cpus->cpus[n].type = ms->cpu_type;
        ms->possible_cpus->cpus[n].arch_id = phytiumpi_mpidr[n];
        ms->possible_cpus->cpus[n].props.has_core_id = true;
        ms->possible_cpus->cpus[n].props.core_id = n;
    }

    return ms->possible_cpus;
}

static CpuInstanceProperties
phytiumpi_cpu_index_to_props(MachineState *ms, unsigned cpu_index)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

static void phytiumpi_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Phytium Pi board (Phytium E2000Q/PE2204)";
    mc->init = phytiumpi_init;
    mc->max_cpus = PHYTIUMPI_MAX_CPUS;
    mc->default_cpus = PHYTIUMPI_MAX_CPUS;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a72");
    mc->default_ram_size = PHYTIUMPI_DEFAULT_RAM_SIZE;
    mc->default_ram_id = "phytium-pi.ram";
    mc->possible_cpu_arch_ids = phytiumpi_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = phytiumpi_cpu_index_to_props;
}

static const TypeInfo phytiumpi_machine_typeinfo = {
    .name = TYPE_PHYTIUMPI_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(PhytiumPiMachineState),
    .class_init = phytiumpi_machine_class_init,
    .interfaces = aarch64_machine_interfaces,
};

static void phytiumpi_machine_init_register_types(void)
{
    type_register_static(&phytiumpi_machine_typeinfo);
}

type_init(phytiumpi_machine_init_register_types)
