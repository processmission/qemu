/*
 * Sony PlayStation 2
 *
 * The Emotion Engine (an R5900 core with its DMAC, INTC, timers, serial
 * port, and VU1 with its interface VIF1), the Graphics Synthesizer, the
 * CDVD S-command interface and the SIF, which stands in for the services
 * of the I/O processor.
 * Of the IOP devices, the EE drives the USB host controller and the ATA
 * interface of the expansion bay directly.
 * There is no BIOS: a synthesized ROM0 carries a ROM directory with
 * ROMVER and empty IOP modules, enough for Linux, and its reset code sets
 * Status.EIE and jumps to the ELF kernel given with -kernel.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/cutils.h"
#include "target/mips/cpu.h"
#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/char/ps2_sio.h"
#include "hw/display/ps2_gs.h"
#include "hw/display/ps2_vu1.h"
#include "hw/dma/ps2_dmac.h"
#include "hw/ide/mmio.h"
#include "hw/intc/ps2_intc.h"
#include "hw/mips/bootloader.h"
#include "hw/mips/mips.h"
#include "hw/misc/ps2_dev9.h"
#include "hw/misc/ps2_scmd.h"
#include "hw/misc/ps2_sif.h"
#include "hw/misc/unimp.h"
#include "hw/timer/ps2_timer.h"
#include "hw/usb/hcd-ohci.h"
#include "system/blockdev.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "elf.h"

#define PS2_CPU_CLOCK_HZ    294912000

#define PS2_RAM_SIZE        (32 * MiB)
#define PS2_TIMER_BASE      0x10000000
#define PS2_GIF_BASE        0x10003000
#define PS2_VIF1_REG_BASE   0x10003c00
#define PS2_VIF1_FIFO_BASE  0x10005000
#define PS2_DMAC_BASE       0x10008000
#define PS2_INTC_BASE       0x1000f000
#define PS2_SIO_BASE        0x1000f100
#define PS2_SIF_BASE        0x1000f200
#define PS2_DMAC_ENABLE_BASE 0x1000f500
#define PS2_VU1_MICRO_BASE  0x11008000
#define PS2_VU1_DATA_BASE   0x1100c000
#define PS2_GS_BASE         0x12000000
#define PS2_IOP_RAM_BASE    0x1c000000
#define PS2_ROM1_BASE       0x1e000000
#define PS2_ROM1_SIZE       (1 * MiB)
#define PS2_ROM0_BASE       0x1fc00000
#define PS2_ROM0_SIZE       (4 * MiB)
#define PS2_CDVD_BASE       0x1f402000
#define PS2_IOP_IO_BASE     0x1f800000
#define PS2_IOP_IO_SIZE     0x10000
#define PS2_DEV9_BASE       0x1f801460
#define PS2_OHCI_BASE       0x1f801600
/* The EE sees the DEV9 SPEED registers of the IOP at 0x14000000. */
#define PS2_SPEED_BASE      0x14000000
#define PS2_IOP_SPEED_BASE  0x10000000

/* IOP interrupts, relayed by the SIF. */
#define PS2_IOP_IRQ_USB     22
#define PS2_IOP_IRQ_ATA0    46

/* Version 2.30, Europe, consumer console, dated 2004-06-14. */
#define PS2_ROMVER          "0230EC20040614\n"

/* Each ROM file is padded to a multiple of 16 bytes. */
#define PS2_ROM_RESET_SIZE  0x100
#define PS2_ROM_ALIGN       16

typedef struct PS2RomDirEntry {
    char name[10];
    uint16_t extinfo_size;
    uint32_t size;
} QEMU_PACKED PS2RomDirEntry;

typedef struct PS2RomLibrary {
    const char *name;
    uint16_t version;
} PS2RomLibrary;

/*
 * The IOP modules of the ROM.  They are empty IRX objects: the SIF does
 * the work of their RPC servers.  Linux reads their export tables to
 * resolve the libraries that its own IRX modules import.
 */
typedef struct PS2RomModule {
    const char *file;
    const char *name;
    PS2RomLibrary exports[7];
} PS2RomModule;

static const PS2RomModule ps2_rom_modules[] = {
    { "SYSMEM", "System_Memory_Manager", { { "sysmem", 0x0101 } } },
    { "LOADCORE", "Module_Manager", { { "loadcore", 0x0101 } } },
    { "EXCEPMAN", "Exception_Manager", { { "excepman", 0x0101 } } },
    { "INTRMANP", "Interrupt_Manager", { { "intrman", 0x0102 } } },
    { "SSBUSC", "Ssbus_Service", { { "ssbusc", 0x0101 } } },
    { "DMACMAN", "DMA_Manager", { { "dmacman", 0x0101 } } },
    { "TIMEMANP", "Timer_Manager", { { "timrman", 0x0101 } } },
    { "SYSCLIB", "System_C_lib", { { "sysclib", 0x0101 } } },
    { "HEAPLIB", "Heap_lib", { { "heaplib", 0x0101 } } },
    { "THREADMAN", "Multi_Thread_Manager", {
        { "thbase", 0x0101 }, { "thevent", 0x0101 }, { "thsemap", 0x0101 },
        { "thmsgbx", 0x0101 }, { "thfpool", 0x0101 }, { "thvpool", 0x0101 },
    } },
    { "VBLANK", "Vblank_service", { { "vblank", 0x0101 } } },
    { "IOMAN", "IO/File_Manager", { { "ioman", 0x0101 } } },
    { "MODLOAD", "Moldule_File_loader", { { "modload", 0x0101 } } },
    { "ROMDRV", "ROM_file_driver", { { "romdrv", 0x0101 } } },
    { "ADDDRV", "Add_file_driver", { } },
    { "STDIO", "Stdio", { { "stdio", 0x0102 } } },
    { "SIFMAN", "IOP_SIF_manager", { { "sifman", 0x0101 } } },
    { "SIFCMD", "IOP_SIF_rpc_interface", { { "sifcmd", 0x0101 } } },
    { "LOADFILE", "LoadModuleByEE", { { "loadfile", 0x0101 } } },
    { "CDVDMAN", "cdvd_driver", { { "cdvdman", 0x0101 } } },
    { "LIBSD", "Sound_Device_Library", { { "libsd", 0x0101 } } },
};

#define PS2_IRX_SHT_IOPMOD  0x70000080
#define PS2_IRX_EXPORT_MAGIC 0x41c00000
#define PS2_IRX_SHNUM       4
#define PS2_IRX_STRTAB      "\0.iopmod\0.text\0.shstrtab"

static void ps2_irx_section(uint8_t *shdr, uint32_t name, uint32_t type,
                            uint32_t offset, uint32_t size)
{
    stl_le_p(shdr, name);
    stl_le_p(shdr + 4, type);
    stl_le_p(shdr + 16, offset);
    stl_le_p(shdr + 20, size);
    stl_le_p(shdr + 32, 4);
}

/* Build an IRX object with an .iopmod section and export tables. */
static uint32_t ps2_build_irx(uint8_t *irx, const PS2RomModule *m)
{
    uint32_t iopmod = sizeof(Elf32_Ehdr);
    uint32_t iopmod_size = 26 + strlen(m->name) + 1;
    uint32_t text = ROUND_UP(iopmod + iopmod_size, 4);
    uint32_t text_size = 0, strtab, shoff;
    uint8_t *p;

    for (const PS2RomLibrary *l = m->exports; l->name; l++) {
        p = irx + text + text_size;
        stl_le_p(p, PS2_IRX_EXPORT_MAGIC);
        stl_le_p(p + 8, l->version);
        strncpy((char *)p + 12, l->name, 8);
        /* A zero terminates the empty table of exported functions. */
        text_size += 24;
    }
    strtab = text + text_size;
    memcpy(irx + strtab, PS2_IRX_STRTAB, sizeof(PS2_IRX_STRTAB));
    shoff = ROUND_UP(strtab + sizeof(PS2_IRX_STRTAB), 4);

    memcpy(irx, ELFMAG, SELFMAG);
    irx[EI_CLASS] = ELFCLASS32;
    irx[EI_DATA] = ELFDATA2LSB;
    irx[EI_VERSION] = EV_CURRENT;
    /* ET_SCE_IOPRELEXEC */
    stw_le_p(irx + 0x10, 0xff80);
    stw_le_p(irx + 0x12, EM_MIPS);
    stl_le_p(irx + 0x14, EV_CURRENT);
    stl_le_p(irx + 0x20, shoff);
    stw_le_p(irx + 0x28, sizeof(Elf32_Ehdr));
    stw_le_p(irx + 0x2a, sizeof(Elf32_Phdr));
    stw_le_p(irx + 0x2e, sizeof(Elf32_Shdr));
    stw_le_p(irx + 0x30, PS2_IRX_SHNUM);
    stw_le_p(irx + 0x32, PS2_IRX_SHNUM - 1);

    p = irx + iopmod;
    stl_le_p(p, 0xffffffff);
    stl_le_p(p + 12, text_size);
    stw_le_p(p + 24, 0x0101);
    strcpy((char *)p + 26, m->name);

    p = irx + shoff + sizeof(Elf32_Shdr);
    ps2_irx_section(p, 1, PS2_IRX_SHT_IOPMOD, iopmod, iopmod_size);
    p += sizeof(Elf32_Shdr);
    ps2_irx_section(p, 9, SHT_PROGBITS, text, text_size);
    p += sizeof(Elf32_Shdr);
    ps2_irx_section(p, 15, SHT_STRTAB, strtab, sizeof(PS2_IRX_STRTAB));

    return shoff + PS2_IRX_SHNUM * sizeof(Elf32_Shdr);
}

/*
 * Lay out ROM0 the way Linux expects it: RESET at offset 0 holds the
 * reset code, ROMDIR follows and describes all files, whose contents
 * follow back to back: RESET, ROMDIR, EXTINFO, ROMVER and the IOP modules.
 */
static void ps2_build_rom0(uint8_t *rom, uint64_t kernel_entry)
{
    PS2RomDirEntry *dir = (PS2RomDirEntry *)(rom + PS2_ROM_RESET_SIZE);
    uint32_t files = 4 + ARRAY_SIZE(ps2_rom_modules);
    /* The files and a terminating entry. */
    uint32_t dir_size = (files + 1) * sizeof(PS2RomDirEntry);
    uint32_t *p = (uint32_t *)rom;
    uint32_t offset, size;

    /* lui t0, 0x1041: CU0, BEV and the R5900 EIE master enable. */
    stl_p(p++, 0x3c081041);
    /* mtc0 t0, $12 */
    stl_p(p++, 0x40886000);
    /* sync.p */
    stl_p(p++, 0x0000040f);
    bl_gen_jump_kernel((void **)&p,
                       false, 0, false, 0, false, 0, false, 0, false, 0,
                       kernel_entry);
    assert((uint8_t *)p <= rom + PS2_ROM_RESET_SIZE);

    pstrcpy(dir[0].name, sizeof(dir[0].name), "RESET");
    stl_le_p(&dir[0].size, PS2_ROM_RESET_SIZE);
    pstrcpy(dir[1].name, sizeof(dir[1].name), "ROMDIR");
    stl_le_p(&dir[1].size, dir_size);
    pstrcpy(dir[2].name, sizeof(dir[2].name), "EXTINFO");
    pstrcpy(dir[3].name, sizeof(dir[3].name), "ROMVER");
    stl_le_p(&dir[3].size, sizeof(PS2_ROMVER));

    offset = PS2_ROM_RESET_SIZE + ROUND_UP(dir_size, PS2_ROM_ALIGN);
    memcpy(rom + offset, PS2_ROMVER, sizeof(PS2_ROMVER));
    offset += ROUND_UP(sizeof(PS2_ROMVER), PS2_ROM_ALIGN);

    for (int i = 0; i < ARRAY_SIZE(ps2_rom_modules); i++) {
        size = ps2_build_irx(rom + offset, &ps2_rom_modules[i]);
        pstrcpy(dir[4 + i].name, sizeof(dir[4 + i].name),
                ps2_rom_modules[i].file);
        stl_le_p(&dir[4 + i].size, size);
        offset += ROUND_UP(size, PS2_ROM_ALIGN);
    }
    assert(offset <= PS2_ROM0_SIZE);
}

static uint64_t ps2_load_kernel(MachineState *machine)
{
    uint64_t entry;
    ssize_t size;

    if (!machine->kernel_filename) {
        error_report("the PS2 machine needs a kernel, given with -kernel");
        exit(1);
    }
    if (machine->initrd_filename) {
        warn_report("-initrd is ignored: the kernel has no way to find it");
    }

    size = load_elf(machine->kernel_filename, NULL, cpu_mips_kseg0_to_phys,
                    NULL, &entry, NULL, NULL, NULL, ELFDATA2LSB, EM_MIPS,
                    1, 0);
    if (size < 0) {
        error_report("could not load kernel '%s': %s",
                     machine->kernel_filename, load_elf_strerror(size));
        exit(1);
    }
    /* The kernel is a 32-bit ELF linked in KSEG0. */
    return (int32_t)entry;
}

static void ps2_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *iop_ram = g_new(MemoryRegion, 1);
    MemoryRegion *rom0 = g_new(MemoryRegion, 1);
    MemoryRegion *rom1 = g_new(MemoryRegion, 1);
    MemoryRegion *iop_mem = g_new(MemoryRegion, 1);
    MemoryRegion *iop_alias = g_new(MemoryRegion, 3);
    DeviceState *intc, *dmac, *gs, *sif, *dev;
    CPUMIPSState *env;
    uint64_t kernel_entry;
    Clock *cpuclk;
    MIPSCPU *cpu;

    if (machine->ram_size != PS2_RAM_SIZE) {
        error_report("the PS2 has 32 MiB of RAM");
        exit(1);
    }

    cpuclk = clock_new(OBJECT(machine), "cpu-refclk");
    clock_set_hz(cpuclk, PS2_CPU_CLOCK_HZ);
    cpu = mips_cpu_create_with_clock(machine->cpu_type, cpuclk, false);
    env = &cpu->env;
    cpu_mips_irq_init_cpu(cpu);
    cpu_mips_clock_init(cpu);

    /* Unmodelled EE and IOP registers read as zero and ignore writes. */
    create_unimplemented_device("ps2.io", 0x10000000, 0x10000000);

    memory_region_add_subregion(sysmem, 0, machine->ram);
    memory_region_init_ram(iop_ram, NULL, "ps2.iop-ram", PS2_IOP_RAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, PS2_IOP_RAM_BASE, iop_ram);

    kernel_entry = ps2_load_kernel(machine);
    memory_region_init_rom(rom0, NULL, "ps2.rom0", PS2_ROM0_SIZE,
                           &error_fatal);
    ps2_build_rom0(memory_region_get_ram_ptr(rom0), kernel_entry);
    memory_region_add_subregion(sysmem, PS2_ROM0_BASE, rom0);
    memory_region_init_rom(rom1, NULL, "ps2.rom1", PS2_ROM1_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, PS2_ROM1_BASE, rom1);

    intc = sysbus_create_simple(TYPE_PS2_INTC, PS2_INTC_BASE, env->irq[2]);

    dev = sysbus_create_simple(TYPE_PS2_TIMER, PS2_TIMER_BASE, NULL);
    for (int i = 0; i < PS2_TIMER_NUM; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i,
                           qdev_get_gpio_in(intc, PS2_INTC_TIMER0 + i));
    }

    dev = qdev_new(TYPE_PS2_SIO);
    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, PS2_SIO_BASE);

    dmac = qdev_new(TYPE_PS2_DMAC);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dmac), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dmac), 0, PS2_DMAC_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(dmac), 1, PS2_DMAC_ENABLE_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dmac), 0, env->irq[3]);

    gs = qdev_new(TYPE_PS2_GS);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gs), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gs), 0, PS2_GS_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(gs), 1, PS2_GIF_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(gs), 0,
                       qdev_get_gpio_in(intc, PS2_INTC_GS));
    sysbus_connect_irq(SYS_BUS_DEVICE(gs), 1,
                       qdev_get_gpio_in(intc, PS2_INTC_VBLANK_START));
    sysbus_connect_irq(SYS_BUS_DEVICE(gs), 2,
                       qdev_get_gpio_in(intc, PS2_INTC_VBLANK_END));
    ps2_dmac_set_channel_ops(PS2_DMAC(dmac), PS2_DMAC_GIF,
                             ps2_gs_get_dma_ops(), gs);

    dev = qdev_new(TYPE_PS2_VU1);
    object_property_set_link(OBJECT(dev), "dmac", OBJECT(dmac), &error_fatal);
    object_property_set_link(OBJECT(dev), "gs", OBJECT(gs), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, PS2_VIF1_REG_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, PS2_VIF1_FIFO_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 2, PS2_VU1_MICRO_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 3, PS2_VU1_DATA_BASE);

    sysbus_create_simple(TYPE_PS2_SCMD, PS2_CDVD_BASE, NULL);

    /*
     * The IOP view of memory: its RAM, its I/O registers, which the EE can
     * access directly too, and the DEV9 SPEED registers.
     */
    memory_region_init(iop_mem, NULL, "ps2.iop", 4 * GiB);
    memory_region_init_alias(&iop_alias[0], NULL, "ps2.iop-ram", iop_ram, 0,
                             PS2_IOP_RAM_SIZE);
    memory_region_add_subregion(iop_mem, 0, &iop_alias[0]);
    memory_region_init_alias(&iop_alias[1], NULL, "ps2.iop-io", sysmem,
                             PS2_IOP_IO_BASE, PS2_IOP_IO_SIZE);
    memory_region_add_subregion(iop_mem, PS2_IOP_IO_BASE, &iop_alias[1]);
    memory_region_init_alias(&iop_alias[2], NULL, "ps2.iop-speed", sysmem,
                             PS2_SPEED_BASE, PS2_SPEED_SIZE);
    memory_region_add_subregion(iop_mem, PS2_IOP_SPEED_BASE, &iop_alias[2]);

    sif = qdev_new(TYPE_PS2_SIF);
    object_property_set_link(OBJECT(sif), "dmac", OBJECT(dmac),
                             &error_fatal);
    object_property_set_link(OBJECT(sif), "iop-memory", OBJECT(iop_mem),
                             &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sif), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(sif), 0, PS2_SIF_BASE);

    /*
     * The USB host controller of the IOP.  It gets IOP addresses, which are
     * where the EE sees the IOP RAM after adding PS2_IOP_RAM_BASE.
     */
    dev = qdev_new(TYPE_SYSBUS_OHCI);
    qdev_prop_set_uint32(dev, "num-ports", 2);
    qdev_prop_set_uint64(dev, "dma-offset", PS2_IOP_RAM_BASE);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, PS2_OHCI_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in_named(sif, "iop-irq",
                                              PS2_IOP_IRQ_USB));

    /*
     * The expansion bay with SPEED and its ATA interface, whose 16-bit
     * registers are 2 bytes apart, with the device control register apart.
     */
    dev = sysbus_create_simple(TYPE_PS2_DEV9, PS2_DEV9_BASE, NULL);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, PS2_SPEED_BASE);

    dev = qdev_new(TYPE_MMIO_IDE);
    qdev_prop_set_uint32(dev, "shift", 1);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 0,
                            PS2_SPEED_BASE + PS2_SPEED_ATA_BASE, 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(dev), 1,
                            PS2_SPEED_BASE + PS2_SPEED_ATA_CTL, 2);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in_named(sif, "iop-irq",
                                              PS2_IOP_IRQ_ATA0));
    mmio_ide_init_drives(dev, drive_get(IF_IDE, 0, 0), NULL);
}

static void ps2_machine_init(MachineClass *mc)
{
    mc->desc = "Sony PlayStation 2";
    mc->init = ps2_init;
    mc->default_cpu_type = MIPS_CPU_TYPE_NAME("R5900");
    mc->default_ram_size = PS2_RAM_SIZE;
    mc->default_ram_id = "ps2.ram";
    mc->block_default_type = IF_IDE;
}

DEFINE_MACHINE("ps2", ps2_machine_init)
