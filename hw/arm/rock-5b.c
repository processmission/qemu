/*
 * Radxa ROCK 5B Board emulation
 *
 * Copyright (C) 2026 Process Mission
 *
 * Authors:
 *    Chao Liu <chao.liu@processmission.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/rk3588.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/net/rtl8125.h"
#include "hw/pci-host/rockchip_pcie.h"
#include "hw/sd/sd.h"
#include "hw/ssi/ssi.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "system/kvm.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/system.h"

#define TYPE_ROCK5B_MACHINE MACHINE_TYPE_NAME("rock-5b")
OBJECT_DECLARE_SIMPLE_TYPE(Rock5BMachineState, ROCK5B_MACHINE)

enum {
    ROCK5B_LOW_RAM,
    ROCK5B_HIGH_RAM,
    ROCK5B_FIRMWARE,
};

static const MemMapEntry rock5b_memmap[] = {
    [ROCK5B_LOW_RAM]  = { 0x000000000, 0x0f0000000 },
    [ROCK5B_HIGH_RAM] = { 0x100000000, 0x310000000 },
    [ROCK5B_FIRMWARE] = { 0x00200000, 0x01000000 },
};

#define ROCK5B_ATAGS_ADDR 0x001fe000
#define ROCK5B_FLASH_SIZE (16 * MiB)
#define ROCK5B_DRAM_TYPE_LPDDR4X 8
/* UART2 drives the board's debug header. */
#define ROCK5B_CONSOLE_UART 2

struct Rock5BMachineState {
    MachineState parent_obj;

    RK3588SoCState soc;
    bool rknpu;
    MemoryRegion low_ram;
    MemoryRegion high_ram;
    uint64_t low_ram_size;
    uint64_t high_ram_size;
    /*
     * Physical base of the RAM above 4 GiB.  The RK3588 exposes RAM through
     * a sparse physical map; the board default matches the reference boot
     * images at 0x100000000, while some downstream firmware needs the high
     * bank at 0x400000000.
     */
    uint64_t high_ram_base;
    struct arm_boot_info binfo;
};

/*
 * Compute the Rockchip ATAG checksum for the DDR memory description
 * passed to U-Boot proper. The algorithm must match U-Boot's js_hash().
 */
static uint32_t rock5b_atags_hash(const uint32_t *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t hash = 0x47c6a7e6;

    for (size_t i = 0; i < size; i++) {
        hash ^= (hash << 5) + bytes[i] + (hash >> 2);
    }
    return hash;
}

static void rock5b_create_atags(Rock5BMachineState *s)
{
    uint32_t tags[5 + 48 + 2] = { 0 };
    uint32_t *ddr = tags + 5;
    unsigned int banks = s->high_ram_size ? 2 : 1;

    tags[0] = cpu_to_le32(5);
    tags[1] = cpu_to_le32(0x54410001);
    ddr[0] = cpu_to_le32(48);
    ddr[1] = cpu_to_le32(0x54410052);
    ddr[2] = cpu_to_le32(banks);
    stq_le_p(&ddr[4 + 2 * banks], s->low_ram_size);
    if (s->high_ram_size) {
        stq_le_p(&ddr[6], s->high_ram_base);
        stq_le_p(&ddr[4 + 2 * (banks + 1)], s->high_ram_size);
    }
    ddr[47] = cpu_to_le32(rock5b_atags_hash(ddr, 47 * sizeof(*ddr)));
    rom_add_blob_fixed("rock-5b.atags", tags, sizeof(tags),
                       ROCK5B_ATAGS_ADDR);
}

static void rock5b_reset(void *opaque)
{
    Rock5BMachineState *s = opaque;

    for (unsigned int i = 0; i < RK3588_NUM_CPUS; i++) {
        ARMCPU *cpu = &s->soc.cpu[i];

        cpu_reset(CPU(cpu));
        arm_emulate_firmware_reset(CPU(cpu), 2);
        arm_rebuild_hflags(&cpu->env);
        if (i == 0) {
            cpu_set_pc(CPU(cpu), rock5b_memmap[ROCK5B_FIRMWARE].base);
        }
    }
}

/*
 * -serial slot 0 is the ROCK 5B Type-C USB CDC console implemented by the
 * DWC3 device-mode controller, matching the board's USB-C debug interface.
 * Slot 1 is the UART2 debug header; further options connect to UART1 and
 * UART3 through UART9 in index order.
 */
static void rock5b_connect_serial(Rock5BMachineState *s)
{
    unsigned int hd = 2;

    qdev_prop_set_chr(DEVICE(&s->soc), "usb3otg0-chardev", serial_hd(0));

    /* UART0 has no external serial connection. */
    for (unsigned int i = 1; i < RK3588_NUM_UARTS; i++) {
        g_autofree char *prop = g_strdup_printf("uart%u-chardev", i);

        qdev_prop_set_chr(DEVICE(&s->soc), prop,
                          serial_hd(i == ROCK5B_CONSOLE_UART ? 1 : hd++));
    }
}

static void rock5b_attach_emmc_card(Rock5BMachineState *s)
{
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BusState *bus;
    DeviceState *card;

    if (!di) {
        return;
    }

    bus = qdev_get_child_bus(s->soc.sdhci, "sd-bus");
    if (!bus) {
        error_report("rock-5b: sdhci has no sd-bus");
        exit(EXIT_FAILURE);
    }

    card = qdev_new(TYPE_EMMC);
    qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                            &error_fatal);
    qdev_realize_and_unref(card, bus, &error_fatal);
}

static void rock5b_attach_sd_card(Rock5BMachineState *s)
{
    DriveInfo *di = drive_get(IF_SD, 0, 2);
    BusState *bus;
    DeviceState *card;

    /*
     * The legacy -drive if=sd,index=2 interface is the board's microSD slot;
     * index 0 is eMMC and index 1 is reserved for SDIO, which is not modeled.
     * With no drive the slot stays empty (no card).
     */
    if (!di) {
        return;
    }

    bus = qdev_get_child_bus(s->soc.sdmmc, "sd-bus");
    if (!bus) {
        error_report("rock-5b: sdmmc has no sd-bus");
        exit(EXIT_FAILURE);
    }

    card = qdev_new(TYPE_SD_CARD);
    qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(di),
                            &error_fatal);
    qdev_realize_and_unref(card, bus, &error_fatal);
}

static void rock5b_attach_nic(Rock5BMachineState *s)
{
    PCIBus *secbus = rockchip_pcie_host_secondary_bus(
        s->soc.pcie[RK3588_PCIE2X1L2]);
    DeviceState *endpoint;

    if (!secbus) {
        error_report("rock-5b: pcie2x1l2 root port has no secondary bus");
        exit(EXIT_FAILURE);
    }

    /*
     * The onboard RTL8125 is always present on the root port's secondary
     * bus (bus 1, device 0, function 0 after guest enumeration).  Bind an
     * explicit model=rtl8125 / rtl8125-pci NIC configuration if supplied.
     */
    endpoint = qdev_new(TYPE_RTL8125_PCI);
    qemu_configure_nic_device(endpoint, false, "rtl8125");
    qdev_realize_and_unref(endpoint, BUS(secbus), &error_fatal);
}

static void rock5b_create_flash(Rock5BMachineState *s)
{
    DriveInfo *di = drive_get(IF_MTD, 0, 0);
    DeviceState *flash = qdev_new("xt25f128");

    object_property_add_child(OBJECT(s), "spi-nor", OBJECT(flash));
    if (di) {
        BlockBackend *blk = blk_by_legacy_dinfo(di);
        int64_t length = blk_getlength(blk);

        if (length != ROCK5B_FLASH_SIZE) {
            error_report("rock-5b: SPI NOR image must be exactly 16 MiB");
            exit(EXIT_FAILURE);
        }
        qdev_prop_set_drive_err(flash, "drive", blk, &error_fatal);
    }
    qdev_realize_and_unref(flash, qdev_get_child_bus(s->soc.sfc, "spi"),
                           &error_fatal);
    qdev_connect_gpio_out_named(s->soc.sfc, "cs", 0,
                                qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0));
}

/*
 * arm_load_dtb() replaces the memory nodes with a single node for low RAM;
 * add the RAM above the peripheral hole.
 */
static void rock5b_modify_dtb(const struct arm_boot_info *binfo, void *fdt)
{
    const Rock5BMachineState *s = container_of(binfo, Rock5BMachineState,
                                               binfo);
    hwaddr base = s->high_ram_base;
    g_autofree char *node = NULL;
    uint32_t acells, scells;

    if (!s->high_ram_size) {
        return;
    }

    acells = qemu_fdt_getprop_cell(fdt, "/", "#address-cells", NULL,
                                   &error_fatal);
    scells = qemu_fdt_getprop_cell(fdt, "/", "#size-cells", NULL,
                                   &error_fatal);
    node = g_strdup_printf("/memory@%" HWADDR_PRIx, base);
    qemu_fdt_add_subnode(fdt, node);
    qemu_fdt_setprop_string(fdt, node, "device_type", "memory");
    if (qemu_fdt_setprop_sized_cells(fdt, node, "reg", acells, base,
                                     scells, s->high_ram_size) < 0) {
        error_report("rock-5b: device tree cells cannot describe RAM "
                     "above 4 GiB");
        exit(EXIT_FAILURE);
    }
}

static void rock5b_firmware_boot(Rock5BMachineState *s)
{
    MachineState *ms = MACHINE(s);

    for (unsigned int i = 0; i < RK3588_NUM_CPUS; i++) {
        object_property_set_int(OBJECT(&s->soc.cpu[i]), "psci-conduit",
                                QEMU_PSCI_CONDUIT_SMC, &error_abort);
    }
    rock5b_create_atags(s);

    if (ms->firmware) {
        g_autofree char *filename = qemu_find_file(QEMU_FILE_TYPE_BIOS,
                                                  ms->firmware);
        ssize_t size;

        /* U-Boot starts at NonSecure EL2 after emulated secure firmware. */
        rk3588_soc_prepare_nonsecure_interrupts(&s->soc);

        if (!filename) {
            error_report("rock-5b: cannot find firmware '%s'",
                         ms->firmware);
            exit(EXIT_FAILURE);
        }
        size = load_image_targphys(filename,
                                   rock5b_memmap[ROCK5B_FIRMWARE].base,
                                   rock5b_memmap[ROCK5B_FIRMWARE].size,
                                   &error_fatal);
        if (size <= 0) {
            error_report("rock-5b: firmware image is empty");
            exit(EXIT_FAILURE);
        }
    }
    qemu_register_reset(rock5b_reset, s);
}

/*
 * arm_load_kernel() enters Linux at EL2, keeps the SMC PSCI conduit and
 * registers the CPU reset handlers.
 */
static void rock5b_direct_boot(Rock5BMachineState *s)
{
    s->binfo.ram_size = s->low_ram_size;
    s->binfo.loader_start = 0;
    s->binfo.board_id = -1;
    s->binfo.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    s->binfo.modify_dtb = rock5b_modify_dtb;
    arm_load_kernel(&s->soc.cpu[0], MACHINE(s), &s->binfo);
}

static void rock5b_init(MachineState *ms)
{
    Rock5BMachineState *s = ROCK5B_MACHINE(ms);

    if (ms->ram_size < 128 * MiB || ms->ram_size > 16 * GiB) {
        error_report("rock-5b: memory size must be between "
                     "128 MiB and 16 GiB");
        exit(EXIT_FAILURE);
    }

    if (kvm_enabled()) {
        error_report("rock-5b: KVM is not supported");
        exit(EXIT_FAILURE);
    }

    if (ms->kernel_filename) {
        if (ms->firmware) {
            error_report("rock-5b: -bios and -kernel cannot be combined");
            exit(EXIT_FAILURE);
        }
        if (!ms->dtb) {
            error_report("rock-5b: direct Linux boot requires -dtb");
            exit(EXIT_FAILURE);
        }
    } else {
        if (ms->dtb) {
            error_report("rock-5b: -dtb requires -kernel; U-Boot proper "
                         "passes the device tree from the FIT");
            exit(EXIT_FAILURE);
        }
        if (!ms->firmware && !qtest_enabled()) {
            error_report("rock-5b: supply U-Boot proper with -bios or "
                         "a Linux kernel with -kernel");
            exit(EXIT_FAILURE);
        }
    }

    /* Initialize the SoC object. */
    object_initialize_child(OBJECT(s), "soc", &s->soc, TYPE_RK3588_SOC);
    s->soc.pcie_config[RK3588_PCIE2X1L2] = (RK3588PCIEConfig) {
        .enabled = true,
        .link_up = true,
        .domain = 0,
        .bus_nr = 0,
    };
    qdev_prop_set_uint32(DEVICE(&s->soc), "num-cpus", ms->smp.cpus);
    qdev_prop_set_uint64(DEVICE(&s->soc), "ram-size", ms->ram_size);
    qdev_prop_set_bit(DEVICE(&s->soc), "rknpu", s->rknpu);
    qdev_prop_set_uint32(DEVICE(&s->soc), "dram-type",
                         ROCK5B_DRAM_TYPE_LPDDR4X);
    object_property_set_link(OBJECT(&s->soc), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    rock5b_connect_serial(s);
    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);
    rock5b_attach_nic(s);
    rock5b_attach_emmc_card(s);
    rock5b_attach_sd_card(s);

    if (s->high_ram_base == 0) {
        s->high_ram_base = 0x100000000ULL;
    }
    s->low_ram_size = MIN(ms->ram_size,
                          rock5b_memmap[ROCK5B_LOW_RAM].size);
    s->high_ram_size = ms->ram_size - s->low_ram_size;
    memory_region_init_alias(&s->low_ram, OBJECT(s), "rock-5b.low-ram",
                             ms->ram, 0, s->low_ram_size);
    memory_region_add_subregion(get_system_memory(),
                                rock5b_memmap[ROCK5B_LOW_RAM].base,
                                &s->low_ram);
    if (s->high_ram_size) {
        if (s->high_ram_base < rock5b_memmap[ROCK5B_LOW_RAM].size &&
            s->high_ram_base + s->high_ram_size >
            rock5b_memmap[ROCK5B_LOW_RAM].base) {
            error_report("rock-5b: high RAM at 0x%" PRIx64 " overlaps the "
                         "low RAM window", s->high_ram_base);
            exit(EXIT_FAILURE);
        }
        memory_region_init_alias(&s->high_ram, OBJECT(s),
                                 "rock-5b.high-ram", ms->ram,
                                 s->low_ram_size, s->high_ram_size);
        memory_region_add_subregion(get_system_memory(),
                                    s->high_ram_base, &s->high_ram);
    }

    rock5b_create_flash(s);

    if (ms->kernel_filename) {
        rock5b_direct_boot(s);
    } else {
        rock5b_firmware_boot(s);
    }
}

static void rock5b_get_high_ram_base(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    uint64_t value = ROCK5B_MACHINE(obj)->high_ram_base;

    visit_type_uint64(v, name, &value, errp);
}

static void rock5b_set_high_ram_base(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    Rock5BMachineState *s = ROCK5B_MACHINE(obj);
    uint64_t value;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    s->high_ram_base = value;
}

static bool rock5b_get_rknpu(Object *obj, Error **errp)
{
    return ROCK5B_MACHINE(obj)->rknpu;
}

static void rock5b_set_rknpu(Object *obj, bool value, Error **errp)
{
    ROCK5B_MACHINE(obj)->rknpu = value;
}

static void rock5b_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    /*
     * The board reference firmware uses 0x100000000; some downstream
     * firmware requires the high bank at 0x400000000 and selects it with
     * -machine rock-5b,high-ram-base=0x400000000.
     */
    object_class_property_add(oc, "high-ram-base", "uint64",
                              rock5b_get_high_ram_base,
                              rock5b_set_high_ram_base, NULL, NULL);
    object_class_property_set_description(oc, "high-ram-base",
        "Physical base of the RAM above the 4 GiB peripheral hole "
        "(default 0x100000000)");
    object_class_property_add_bool(oc, "rknpu", rock5b_get_rknpu,
                                   rock5b_set_rknpu);
    object_class_property_set_description(oc, "rknpu",
                                          "Enable RK3588 RKNN/RKNPU "
                                          "accelerator cores");
    mc->desc = "Radxa ROCK 5B";
    mc->init = rock5b_init;
    mc->default_ram_size = 2 * GiB;
    mc->default_ram_id = "rock-5b.ram";
    mc->min_cpus = RK3588_NUM_CPUS;
    mc->default_cpus = RK3588_NUM_CPUS;
    mc->max_cpus = RK3588_NUM_CPUS;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a55");
}

static const TypeInfo rock5b_typeinfo = {
    .name = TYPE_ROCK5B_MACHINE,
    .parent = TYPE_MACHINE,
    .interfaces = aarch64_machine_interfaces,
    .instance_size = sizeof(Rock5BMachineState),
    .class_init = rock5b_class_init,
};

static void rock5b_register_types(void)
{
    type_register_static(&rock5b_typeinfo);
}

type_init(rock5b_register_types)
