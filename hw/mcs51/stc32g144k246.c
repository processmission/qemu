/*
 * STC32G144K246 evaluation machine
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/mcs51/stc32g.h"
#include "qom/object.h"

#define TYPE_STC32G144K246_MACHINE \
    MACHINE_TYPE_NAME("stc32g144k246")
OBJECT_DECLARE_SIMPLE_TYPE(Stc32g144k246MachineState,
                           STC32G144K246_MACHINE)

struct Stc32g144k246MachineState {
    MachineState parent_obj;

    Stc32gSoCState soc;
};

static bool stc32g144k246_firmware_is_hex(const char *filename)
{
    const char *suffix = strrchr(filename, '.');

    return suffix && !g_ascii_strcasecmp(suffix, ".hex");
}

static void stc32g144k246_load_firmware(Stc32g144k246MachineState *s,
                                        const char *filename)
{
    bool is_hex = stc32g144k246_firmware_is_hex(filename);
    ssize_t loaded;

    if (is_hex) {
        hwaddr entry = MCS251_RESET_PC;

        loaded = load_targphys_hex_as_range(filename, &entry,
                                            STC32G_FLASH_BASE,
                                            STC32G_FLASH_SIZE, NULL);
    } else {
        loaded = load_image_mr(filename, &s->soc.flash);
    }

    if (loaded < 0) {
        error_report("Unable to load %s firmware image '%s'",
                     is_hex ? "Intel HEX" : "raw", filename);
        exit(EXIT_FAILURE);
    }
}

static void stc32g144k246_machine_init(MachineState *machine)
{
    Stc32g144k246MachineState *s = STC32G144K246_MACHINE(machine);

    object_initialize_child(OBJECT(machine), "soc", &s->soc,
                            TYPE_STC32G_SOC);
    sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal);

    if (machine->firmware) {
        g_autofree char *filename =
            qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);

        if (!filename) {
            error_report("Unable to find firmware image '%s'",
                         machine->firmware);
            exit(EXIT_FAILURE);
        }
        stc32g144k246_load_firmware(s, filename);
    }
}

static void stc32g144k246_machine_class_init(ObjectClass *oc,
                                             const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "STC32G144K246 evaluation machine";
    mc->init = stc32g144k246_machine_init;
    mc->default_cpu_type = TYPE_MCS251_CPU;
    mc->default_cpus = 1;
    mc->default_ram_size = 0;
    mc->min_cpus = 1;
    mc->max_cpus = 1;
    mc->no_floppy = true;
    mc->no_cdrom = true;
    mc->no_parallel = true;
}

static const TypeInfo stc32g144k246_machine_type = {
    .name = TYPE_STC32G144K246_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(Stc32g144k246MachineState),
    .class_init = stc32g144k246_machine_class_init,
};

static void stc32g144k246_machine_register_types(void)
{
    type_register_static(&stc32g144k246_machine_type);
}

type_init(stc32g144k246_machine_register_types)
