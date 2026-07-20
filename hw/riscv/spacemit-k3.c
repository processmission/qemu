/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SpacemiT K3 Pico-ITX machine
 *
 * This model implements the RISC-V platform and K3 boot-path subset needed
 * to boot the SDK Linux kernel directly or through U-Boot with generic
 * OpenSBI.  BootROM, SPL, DDR training, and the A100/IME harts are outside
 * this machine's current contract.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/riscv_aplic.h"
#include "hw/intc/riscv_imsic.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/iommu.h"
#include "hw/riscv/machines-qom.h"
#include "hw/riscv/spacemit-k3.h"
#include "hw/sd/sd.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "system/memory.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/system.h"
#include "target/riscv/cpu.h"
#include "target/riscv/cpu-qom.h"
#include "target/riscv/time_helper.h"

#include <libfdt.h>

const MemMapEntry spacemit_k3_memmap[] = {
    [K3_DEV_SRAM]         = { 0xc0800000,      0x80000 },
    [K3_DEV_DDR_TRAINING] = { 0xc08d0000,     0x100 },
    [K3_DEV_IOMMU]        = { 0xc0f00000,     0x1000 },
    [K3_DEV_UART0]        = { 0xd4017000,        0x100 },
    [K3_DEV_SDHCI0]       = { 0xd4280000,        0x200 },
    [K3_DEV_APMU]         = { 0xd4282800,        0x400 },
    [K3_DEV_CIU]          = { 0xd4282c00,        0x400 },
    [K3_DEV_S_IMSIC]      = { 0xe0400000,     0x400000 },
    [K3_DEV_S_APLIC]      = { 0xe0804000,       0x4000 },
    [K3_DEV_M_IMSIC]      = { 0xf1000000,      0x10000 },
    [K3_DEV_M_APLIC]      = { 0xf1800000,       0x4000 },
    [K3_DEV_M_CLINT]      = { 0xf1810000,      0x10000 },
    [K3_DEV_FIRMWARE]     = { 0x100000000,    32 * MiB },
    [K3_DEV_DRAM]         = { 0x102000000,           0 },
};

static RISCVCPU *k3_pico_itx_hart(SpacemitK3SoCState *s, unsigned int hartid)
{
    unsigned int cluster = hartid / K3_PICO_ITX_HARTS_PER_CLUSTER;
    unsigned int index = hartid % K3_PICO_ITX_HARTS_PER_CLUSTER;

    return &s->cpus[cluster].harts[index];
}

static bool k3_pico_itx_validate_cpu(SpacemitK3SoCState *s, Error **errp)
{
    unsigned int hartid;

    for (hartid = 0; hartid < K3_PICO_ITX_NUM_HARTS; hartid++) {
        RISCVCPU *cpu = k3_pico_itx_hart(s, hartid);
        CPUState *cs = CPU(cpu);
        unsigned int expected_cluster =
            hartid / K3_PICO_ITX_HARTS_PER_CLUSTER;

        if (!riscv_has_ext(&cpu->env, RVV)) {
            error_setg(errp,
                       "K3 X100 hart %u requires the V extension",
                       hartid);
            return false;
        }
        if (cpu->cfg.vlenb != 256 / 8) {
            error_setg(errp, "K3 X100 hart %u requires VLEN=256", hartid);
            return false;
        }
        if (!cpu->cfg.ext_smaia || !cpu->cfg.ext_ssaia) {
            error_setg(errp,
                       "K3 X100 hart %u requires Smaia and Ssaia", hartid);
            return false;
        }
        if (!cpu->cfg.ext_sstc) {
            error_setg(errp, "K3 X100 hart %u requires Sstc", hartid);
            return false;
        }
        if (riscv_has_ext(&cpu->env, RVH)) {
            error_setg(errp,
                       "K3 X100 hart %u does not support H",
                       hartid);
            return false;
        }
        if (cs->cluster_index != expected_cluster) {
            error_setg(errp,
                       "K3 hart %u has cluster index %d, expected %u",
                       hartid, cs->cluster_index, expected_cluster);
            return false;
        }
    }

    return true;
}

/*
 * The RISC-V qtest accelerator does not install TCG's profile and feature
 * properties.  Supply the subset exercised by this non-executing accelerator
 * directly; TCG still gets the same values through machine compatibility
 * properties and is checked below.
 */
static void k3_pico_itx_apply_qtest_cpu_defaults(SpacemitK3SoCState *s)
{
    unsigned int hartid;

    if (!qtest_enabled()) {
        return;
    }

    for (hartid = 0; hartid < K3_PICO_ITX_NUM_HARTS; hartid++) {
        RISCVCPU *cpu = k3_pico_itx_hart(s, hartid);

        cpu->env.misa_ext |= RVS | RVU | RVV;
        cpu->env.misa_ext &= ~RVH;
        cpu->env.priv_ver = PRIV_VERSION_1_13_0;
        cpu->cfg.vlenb = 256 / 8;
        cpu->cfg.ext_zicsr = true;
        cpu->cfg.ext_smaia = true;
        cpu->cfg.ext_ssaia = true;
        cpu->cfg.ext_sstc = true;
        riscv_timer_init(cpu);
    }
}

static void k3_pico_itx_create_aia(SpacemitK3SoCState *s)
{
    unsigned int hartid;

    for (hartid = 0; hartid < K3_PICO_ITX_NUM_HARTS; hartid++) {
        s->m_imsic[hartid] = riscv_imsic_create(
            spacemit_k3_memmap[K3_DEV_M_IMSIC].base +
                hartid * IMSIC_HART_SIZE(0),
            hartid, true, 1, K3_PICO_ITX_IMSIC_NUM_IDS);
    }

    /*
     * K3 assigns a 0x40000-byte interrupt-file stride to each X100 hart.
     * Only the 511-ID S-mode page is modeled.  The VS pages in each stride
     * and the aperture for A100 harts remain unmapped.
     */
    for (hartid = 0; hartid < K3_PICO_ITX_NUM_HARTS; hartid++) {
        s->s_imsic[hartid] = riscv_imsic_create(
            spacemit_k3_memmap[K3_DEV_S_IMSIC].base + hartid * 0x40000,
            hartid, false, 1, K3_PICO_ITX_IMSIC_NUM_IDS);
    }

    /* MSI-mode APLICs have no direct per-hart interrupt contexts. */
    s->m_aplic = riscv_aplic_create(
        spacemit_k3_memmap[K3_DEV_M_APLIC].base,
        spacemit_k3_memmap[K3_DEV_M_APLIC].size,
        0, 0, K3_PICO_ITX_APLIC_NUM_SOURCES,
        K3_PICO_ITX_APLIC_IPRIO_BITS, true, true, NULL);
    s->s_aplic = riscv_aplic_create(
        spacemit_k3_memmap[K3_DEV_S_APLIC].base,
        spacemit_k3_memmap[K3_DEV_S_APLIC].size,
        0, 0, K3_PICO_ITX_APLIC_NUM_SOURCES,
        K3_PICO_ITX_APLIC_IPRIO_BITS, true, false, s->m_aplic);
}

static void spacemit_k3_soc_reset(void *opaque)
{
    SpacemitK3SoCState *s = opaque;

    memset(memory_region_get_ram_ptr(&s->ddr_training), 0,
           spacemit_k3_memmap[K3_DEV_DDR_TRAINING].size);
}

static bool k3_pico_itx_create_iommu(SpacemitK3SoCState *s, Error **errp)
{
    s->iommu = qdev_new(TYPE_RISCV_IOMMU_SYS);
    object_property_add_child(OBJECT(s), "iommu", OBJECT(s->iommu));
    object_property_set_uint(OBJECT(s->iommu), "addr",
                             spacemit_k3_memmap[K3_DEV_IOMMU].base,
                             &error_abort);
    object_property_set_uint(OBJECT(s->iommu), "base-irq",
                             K3_PICO_ITX_IOMMU_IRQ, &error_abort);
    object_property_set_uint(OBJECT(s->iommu), "irq-count", 1,
                             &error_abort);
    object_property_set_uint(OBJECT(s->iommu), "pas-bits", 56,
                             &error_abort);
    object_property_set_link(OBJECT(s->iommu), "irqchip",
                             OBJECT(s->m_aplic), &error_abort);

    return sysbus_realize_and_unref(SYS_BUS_DEVICE(s->iommu), errp);
}

static void spacemit_k3_soc_realize(DeviceState *dev, Error **errp)
{
    SpacemitK3SoCState *s = SPACEMIT_K3_SOC(dev);
    MachineState *ms = MACHINE(qdev_get_machine());
    MemoryRegion *system_memory = get_system_memory();
    unsigned int cluster;

    memory_region_init_ram(&s->sram, OBJECT(dev), "spacemit.k3.sram",
                           spacemit_k3_memmap[K3_DEV_SRAM].size, errp);
    if (*errp) {
        return;
    }
    memory_region_add_subregion(system_memory,
                                spacemit_k3_memmap[K3_DEV_SRAM].base,
                                &s->sram);

    memory_region_init_ram(&s->ddr_training, OBJECT(dev),
                           "spacemit.k3.ddr-training",
                           spacemit_k3_memmap[K3_DEV_DDR_TRAINING].size,
                           errp);
    if (*errp) {
        return;
    }
    memory_region_add_subregion(
        system_memory, spacemit_k3_memmap[K3_DEV_DDR_TRAINING].base,
        &s->ddr_training);

    memory_region_init_ram(&s->firmware, OBJECT(dev), "spacemit.k3.firmware",
                           spacemit_k3_memmap[K3_DEV_FIRMWARE].size, errp);
    if (*errp) {
        return;
    }
    memory_region_add_subregion(system_memory,
                                spacemit_k3_memmap[K3_DEV_FIRMWARE].base,
                                &s->firmware);

    for (cluster = 0; cluster < K3_PICO_ITX_NUM_CLUSTERS; cluster++) {
        qdev_prop_set_string(DEVICE(&s->cpus[cluster]), "cpu-type",
                             ms->cpu_type);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->cpus[cluster]), errp)) {
            return;
        }
    }

    /* The CPUs must exist below each cluster before the cluster is realized. */
    for (cluster = 0; cluster < K3_PICO_ITX_NUM_CLUSTERS; cluster++) {
        if (!qdev_realize(DEVICE(&s->clusters[cluster]), NULL, errp)) {
            return;
        }
    }

    k3_pico_itx_apply_qtest_cpu_defaults(s);
    if (!k3_pico_itx_validate_cpu(s, errp)) {
        return;
    }

    s->swi = riscv_aclint_swi_create(
        spacemit_k3_memmap[K3_DEV_M_CLINT].base,
        0, K3_PICO_ITX_NUM_HARTS, false);
    s->mtimer = riscv_aclint_mtimer_create(
        spacemit_k3_memmap[K3_DEV_M_CLINT].base + RISCV_ACLINT_SWI_SIZE,
        RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
        0, K3_PICO_ITX_NUM_HARTS,
        RISCV_ACLINT_DEFAULT_MTIMECMP, RISCV_ACLINT_DEFAULT_MTIME,
        K3_PICO_ITX_TIMEBASE_FREQ, true);

    k3_pico_itx_create_aia(s);

    if (!k3_pico_itx_create_iommu(s, errp)) {
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->apmu), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->apmu), 0,
                    spacemit_k3_memmap[K3_DEV_APMU].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ciu), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ciu), 0,
                    spacemit_k3_memmap[K3_DEV_CIU].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sdhci0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sdhci0), 0,
                    spacemit_k3_memmap[K3_DEV_SDHCI0].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sdhci0), 0,
                       qdev_get_gpio_in(s->m_aplic,
                                       K3_PICO_ITX_SDHCI0_IRQ));

    memory_region_init(&s->uart0_mem, OBJECT(dev), "spacemit.k3.uart0",
                       spacemit_k3_memmap[K3_DEV_UART0].size);
    memory_region_add_subregion(system_memory,
                                spacemit_k3_memmap[K3_DEV_UART0].base,
                                &s->uart0_mem);
    s->uart0 = serial_mm_init(
        &s->uart0_mem, 0, 2,
        qdev_get_gpio_in(s->m_aplic, K3_PICO_ITX_UART0_IRQ),
        115200, serial_hd(0), DEVICE_LITTLE_ENDIAN);

    qemu_register_reset(spacemit_k3_soc_reset, s);
}

static void spacemit_k3_soc_instance_init(Object *obj)
{
    SpacemitK3SoCState *s = SPACEMIT_K3_SOC(obj);
    unsigned int cluster;

    object_initialize_child(obj, "cluster0", &s->clusters[0],
                            TYPE_CPU_CLUSTER);
    object_initialize_child(OBJECT(&s->clusters[0]), "cpus", &s->cpus[0],
                            TYPE_RISCV_HART_ARRAY);
    object_initialize_child(obj, "cluster1", &s->clusters[1],
                            TYPE_CPU_CLUSTER);
    object_initialize_child(OBJECT(&s->clusters[1]), "cpus", &s->cpus[1],
                            TYPE_RISCV_HART_ARRAY);
    object_initialize_child(obj, "apmu", &s->apmu, TYPE_SPACEMIT_K3_APMU);
    object_initialize_child(obj, "ciu", &s->ciu, TYPE_SPACEMIT_K3_CIU);
    object_initialize_child(obj, "sdhci0", &s->sdhci0,
                            TYPE_SPACEMIT_K3_SDHCI);

    for (cluster = 0; cluster < K3_PICO_ITX_NUM_CLUSTERS; cluster++) {
        qdev_prop_set_uint32(DEVICE(&s->clusters[cluster]), "cluster-id",
                            cluster);
        qdev_prop_set_uint32(DEVICE(&s->cpus[cluster]), "num-harts",
                            K3_PICO_ITX_HARTS_PER_CLUSTER);
        qdev_prop_set_uint32(DEVICE(&s->cpus[cluster]), "hartid-base",
                            cluster * K3_PICO_ITX_HARTS_PER_CLUSTER);
        qdev_prop_set_uint64(DEVICE(&s->cpus[cluster]), "resetvec",
                            spacemit_k3_memmap[K3_DEV_SRAM].base);
    }
}

static void spacemit_k3_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = spacemit_k3_soc_realize;
    dc->user_creatable = false;
}

static const TypeInfo spacemit_k3_soc_type_info = {
    .name = TYPE_SPACEMIT_K3_SOC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SpacemitK3SoCState),
    .instance_init = spacemit_k3_soc_instance_init,
    .class_init = spacemit_k3_soc_class_init,
};

static void k3_pico_itx_validate_machine(MachineState *machine)
{
    const CpuTopology *smp = &machine->smp;

    if (machine->ram_size != 2 * GiB) {
        error_report("k3-pico-itx requires exactly 2 GiB of RAM");
        exit(EXIT_FAILURE);
    }
    if (strcmp(machine->cpu_type, TYPE_RISCV_CPU_SPACEMIT_X100)) {
        error_report("k3-pico-itx requires the spacemit-x100 CPU type");
        exit(EXIT_FAILURE);
    }
    if (smp->cpus != K3_PICO_ITX_NUM_HARTS ||
        smp->max_cpus != K3_PICO_ITX_NUM_HARTS ||
        smp->sockets != 1 || smp->clusters != K3_PICO_ITX_NUM_CLUSTERS ||
        smp->cores != K3_PICO_ITX_HARTS_PER_CLUSTER || smp->threads != 1) {
        error_report("k3-pico-itx requires "
                     "cpus=8,sockets=1,clusters=2,cores=4,threads=1,maxcpus=8");
        exit(EXIT_FAILURE);
    }
}

static void k3_pico_itx_validate_fdt(K3PicoITXState *s)
{
    MachineState *machine = MACHINE(s);
    const char expected_compat[] = "spacemit,k3-pico-itx";
    const fdt32_t *prop;
    int len;
    int offset;
    unsigned int hartid;
    uint64_t base;
    uint64_t size;

    if (fdt_node_check_compatible(machine->fdt, 0, expected_compat)) {
        error_report("k3-pico-itx DTB requires root compatible '%s'",
                     expected_compat);
        exit(EXIT_FAILURE);
    }

    offset = fdt_path_offset(machine->fdt, "/cpus");
    prop = offset >= 0 ?
        fdt_getprop(machine->fdt, offset, "timebase-frequency", &len) : NULL;
    if (!prop || len != sizeof(*prop) ||
        fdt32_to_cpu(*prop) != K3_PICO_ITX_TIMEBASE_FREQ) {
        error_report("k3-pico-itx DTB requires a 24 MHz timebase");
        exit(EXIT_FAILURE);
    }

    for (hartid = 0; hartid < K3_PICO_ITX_NUM_HARTS; hartid++) {
        g_autofree char *node = g_strdup_printf("/cpus/cpu@%x", hartid);

        if (fdt_path_offset(machine->fdt, node) < 0) {
            error_report("k3-pico-itx DTB is missing X100 hart %u", hartid);
            exit(EXIT_FAILURE);
        }
        riscv_isa_write_fdt(k3_pico_itx_hart(&s->soc, hartid),
                            machine->fdt, node);
    }
    if (fdt_path_offset(machine->fdt, "/cpus/cpu@8") >= 0) {
        error_report("k3-pico-itx DTB must not expose A100 harts");
        exit(EXIT_FAILURE);
    }

    offset = fdt_path_offset(machine->fdt, "/memory@102000000");
    prop = offset >= 0 ? fdt_getprop(machine->fdt, offset, "reg", &len) : NULL;
    if (!prop || len != 4 * sizeof(*prop)) {
        error_report("k3-pico-itx DTB requires the 64-bit Linux memory node");
        exit(EXIT_FAILURE);
    }
    base = ((uint64_t)fdt32_to_cpu(prop[0]) << 32) | fdt32_to_cpu(prop[1]);
    size = ((uint64_t)fdt32_to_cpu(prop[2]) << 32) | fdt32_to_cpu(prop[3]);
    if (base != spacemit_k3_memmap[K3_DEV_DRAM].base ||
        size != machine->ram_size) {
        error_report("k3-pico-itx DTB memory must be 2 GiB at 0x102000000");
        exit(EXIT_FAILURE);
    }

    if (fdt_path_offset(machine->fdt, "/chosen") < 0) {
        qemu_fdt_add_subnode(machine->fdt, "/chosen");
    }
}

static void k3_pico_itx_check_loaded_payloads(MachineState *machine,
                                              RISCVBootInfo *info)
{
    hwaddr dram_base = spacemit_k3_memmap[K3_DEV_DRAM].base;
    hwaddr dram_end = dram_base + machine->ram_size;

    if (info->kernel_size &&
        (info->image_low_addr < dram_base ||
         info->image_high_addr > dram_end ||
         info->image_high_addr < info->image_low_addr)) {
        error_report("K3 -kernel payload does not fit in the DRAM window");
        exit(EXIT_FAILURE);
    }
    if (info->initrd_size &&
        (info->initrd_start < dram_base ||
         info->initrd_start + info->initrd_size > dram_end ||
         info->initrd_start + info->initrd_size < info->initrd_start)) {
        error_report("K3 -initrd payload does not fit in the DRAM window");
        exit(EXIT_FAILURE);
    }
}

static void k3_pico_itx_attach_sd_card(K3PicoITXState *s)
{
    DriveInfo *dinfo = drive_get(IF_SD, 0, 0);
    DeviceState *card;

    if (!dinfo) {
        return;
    }

    card = qdev_new(TYPE_SD_CARD);
    qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo),
                            &error_fatal);
    qdev_realize_and_unref(card, s->soc.sdhci0.sd_bus, &error_fatal);
}

static void k3_pico_itx_machine_init(MachineState *machine)
{
    K3PicoITXState *s = K3_PICO_ITX_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    RISCVBootInfo boot_info;
    hwaddr firmware_load_addr = spacemit_k3_memmap[K3_DEV_FIRMWARE].base;
    hwaddr firmware_end_addr = firmware_load_addr;
    hwaddr fdt_load_addr = 0;
    uint64_t kernel_entry = 0;

    k3_pico_itx_validate_machine(machine);

    memory_region_add_subregion(system_memory,
                                spacemit_k3_memmap[K3_DEV_DRAM].base,
                                machine->ram);
    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);
    k3_pico_itx_attach_sd_card(s);

    riscv_boot_info_init(&boot_info, &s->soc.cpus[0]);

    if (machine->kernel_filename &&
        machine->firmware && !strcmp(machine->firmware, "none")) {
        error_report("k3-pico-itx direct Linux boot requires OpenSBI firmware");
        exit(EXIT_FAILURE);
    }

    firmware_end_addr = riscv_find_and_load_firmware(
        machine, &boot_info,
        riscv_default_firmware_name(&s->soc.cpus[0]),
        &firmware_load_addr, NULL);
    if (firmware_load_addr < spacemit_k3_memmap[K3_DEV_FIRMWARE].base ||
        firmware_end_addr > spacemit_k3_memmap[K3_DEV_DRAM].base ||
        firmware_end_addr < firmware_load_addr) {
        error_report("K3 firmware must fit in 0x100000000..0x102000000");
        exit(EXIT_FAILURE);
    }

    if (machine->kernel_filename && !machine->dtb) {
        error_report("k3-pico-itx direct Linux boot requires -dtb");
        exit(EXIT_FAILURE);
    }
    if (machine->dtb) {
        machine->fdt = load_device_tree(machine->dtb, &s->fdt_size);
        if (!machine->fdt) {
            error_report("failed to load k3-pico-itx device tree");
            exit(EXIT_FAILURE);
        }
        k3_pico_itx_validate_fdt(s);
    }

    if (machine->kernel_filename) {
        hwaddr kernel_start_addr = MAX(
            riscv_calc_kernel_start_addr(&boot_info, firmware_end_addr),
            spacemit_k3_memmap[K3_DEV_DRAM].base);

        riscv_load_kernel(machine, &boot_info, kernel_start_addr, true, NULL);
        k3_pico_itx_check_loaded_payloads(machine, &boot_info);
        kernel_entry = boot_info.image_low_addr;
    }

    if (machine->fdt) {
        fdt_load_addr = riscv_compute_fdt_addr(
            spacemit_k3_memmap[K3_DEV_DRAM].base, machine->ram_size,
            machine, &boot_info);
        if (fdt_load_addr < spacemit_k3_memmap[K3_DEV_DRAM].base ||
            fdt_load_addr + fdt_totalsize(machine->fdt) >
                spacemit_k3_memmap[K3_DEV_DRAM].base + machine->ram_size) {
            error_report("K3 DTB does not fit in the DRAM window");
            exit(EXIT_FAILURE);
        }
        riscv_load_fdt(fdt_load_addr, machine->fdt);
    }

    riscv_setup_rom_reset_vec(machine, &s->soc.cpus[0], firmware_load_addr,
                              spacemit_k3_memmap[K3_DEV_SRAM].base,
                              spacemit_k3_memmap[K3_DEV_SRAM].size,
                              kernel_entry, fdt_load_addr);
}

static void k3_pico_itx_machine_instance_init(Object *obj)
{
    K3PicoITXState *s = K3_PICO_ITX_MACHINE(obj);
    MachineState *machine = MACHINE(obj);

    machine->smp.cpus = K3_PICO_ITX_NUM_HARTS;
    machine->smp.max_cpus = K3_PICO_ITX_NUM_HARTS;
    machine->smp.sockets = 1;
    machine->smp.clusters = K3_PICO_ITX_NUM_CLUSTERS;
    machine->smp.cores = K3_PICO_ITX_HARTS_PER_CLUSTER;
    machine->smp.threads = 1;

    object_initialize_child(obj, "soc", &s->soc, TYPE_SPACEMIT_K3_SOC);
}

static GlobalProperty k3_pico_itx_cpu_defaults[] = {
    { TYPE_RISCV_CPU_SPACEMIT_X100, "h", "false", .optional = true },
};

static void k3_pico_itx_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    static const char *const valid_cpu_types[] = {
        TYPE_RISCV_CPU_SPACEMIT_X100,
        NULL,
    };

    mc->desc = "SpacemiT K3 Pico-ITX (X100 subset)";
    mc->init = k3_pico_itx_machine_init;
    mc->min_cpus = K3_PICO_ITX_NUM_HARTS;
    mc->max_cpus = K3_PICO_ITX_NUM_HARTS;
    mc->default_cpus = K3_PICO_ITX_NUM_HARTS;
    mc->default_cpu_type = TYPE_RISCV_CPU_SPACEMIT_X100;
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = 2 * GiB;
    mc->default_ram_id = "spacemit.k3.ram";
    mc->auto_create_sdcard = true;
    mc->smp_props.clusters_supported = true;
    compat_props_add(mc->compat_props, k3_pico_itx_cpu_defaults,
                     G_N_ELEMENTS(k3_pico_itx_cpu_defaults));
}

static const TypeInfo k3_pico_itx_machine_type_info = {
    .name = TYPE_K3_PICO_ITX_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(K3PicoITXState),
    .instance_init = k3_pico_itx_machine_instance_init,
    .class_init = k3_pico_itx_machine_class_init,
    .interfaces = riscv64_machine_interfaces,
};

static void spacemit_k3_register_types(void)
{
    type_register_static(&spacemit_k3_soc_type_info);
    type_register_static(&k3_pico_itx_machine_type_info);
}

type_init(spacemit_k3_register_types)
