/*
 * Pollen MicroDuck beta board
 *
 * Board wiring around the Rockchip RK3566 SoC: RAM, eMMC, virtio-mmio, the
 * I2C and SPI devices on the board and the Linux boot path.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include <libfdt.h>
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/rk3566.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties-system.h"
#include "chardev/char.h"
#include "hw/gpio/rockchip_gpio.h"
#include "hw/i2c/i2c.h"
#include "hw/misc/rockchip_iommu.h"
#include "hw/sd/sd.h"
#include "hw/ssi/ssi.h"
#include "hw/ssi/rockchip_spi.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "system/device_tree.h"
#include "system/qtest.h"
#include "system/system.h"
#include "target/arm/internals.h"

#define TYPE_MICRODUCK_MACHINE MACHINE_TYPE_NAME("microduck-beta")
OBJECT_DECLARE_SIMPLE_TYPE(MicroDuckState, MICRODUCK_MACHINE)

struct MicroDuckState {
    MachineState parent_obj;
    RK3566SoCState soc;
    MemoryRegion ram;
    struct arm_boot_info boot;
};

static DeviceState *microduck_device(MicroDuckState *s, const char *type,
                                     const char *name)
{
    DeviceState *dev = qdev_new(type);

    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    object_unref(OBJECT(dev));
    return dev;
}

static void microduck_map(MicroDuckState *s, DeviceState *dev, hwaddr address,
                          int irq)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, address);
    if (irq >= 0) {
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(DEVICE(&s->soc.gic), irq));
    }
}

static bool rk3566_smc(ARMCPU *cpu)
{
    MicroDuckState *s = MICRODUCK_MACHINE(qdev_get_machine());
    CPUARMState *env = &cpu->env;
    switch ((uint32_t)env->xregs[0]) {
    case RK3588_SCMI_SMC_ID:
        rk3588_scmi_handle_smc(s->soc.scmi);
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


static void microduck_storage(MicroDuckState *s)
{
    DriveInfo *drive = drive_get(IF_SD, 0, 0);

    if (!drive) {
        return;
    }

    DeviceState *card = qdev_new(TYPE_EMMC);

    qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(drive),
                            &error_fatal);
    qdev_realize_and_unref(card, qdev_get_child_bus(s->soc.sdhci, "sd-bus"),
                           &error_fatal);
}

static void microduck_board_devices(MicroDuckState *s, bool media)
{
    microduck_map(s, microduck_device(s, "virtio-mmio", "management"),
                  0xffe00000, 257);

    for (unsigned i = 0; i < RK3566_NUM_I2C; i++) {
        I2CBus *bus = I2C_BUS(qdev_get_child_bus(s->soc.i2c[i], "i2c-bus"));

        if (i == 0 || i == 3) {
            I2CSlave *dev = i2c_slave_new("microduck-register-i2c", 0x20);

            qdev_prop_set_uint32(DEVICE(dev), "kind", i == 0 ? 0 : 2);
            i2c_slave_realize_and_unref(dev, bus, &error_fatal);
            qdev_connect_gpio_out_named(DEVICE(dev), "nirq", 0,
                qdev_get_gpio_in(s->soc.gpio[i == 0 ? 0 : 4],
                                 i == 0 ? 3 : 23));
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
                qdev_get_gpio_in_named(s->soc.isp, "sensor-stream", 0));
            qdev_connect_gpio_out_named(s->soc.gpio[3], "gpio-out", 22,
                qdev_get_gpio_in_named(DEVICE(camera), "power", 0));
            qdev_connect_gpio_out_named(s->soc.gpio[4], "gpio-out", 17,
                qdev_get_gpio_in_named(DEVICE(camera), "resetn", 0));
        }
    }

    DeviceState *nfc = ssi_create_peripheral(ROCKCHIP_SPI(s->soc.spi[1])->spi,
                                             "microduck-st25r100");

    qdev_connect_gpio_out_named(s->soc.gpio[3], "gpio-out", 1,
                                qdev_get_gpio_in_named(nfc, SSI_GPIO_CS, 0));
    qdev_connect_gpio_out_named(s->soc.gpio[3], "gpio-out", 21,
                                qdev_get_gpio_in_named(nfc, "reset", 0));
    qdev_connect_gpio_out_named(nfc, "irq", 0,
                                qdev_get_gpio_in(s->soc.gpio[3], 20));
    DeviceState *tof = qdev_new("microduck-vl53l8cx");

    qdev_prop_set_chr(tof, "chardev", qemu_chr_find("tof-data"));
    ssi_realize_and_unref(tof, ROCKCHIP_SPI(s->soc.spi[3])->spi, &error_fatal);
    qdev_connect_gpio_out_named(s->soc.spi[3], "cs", 0,
                                qdev_get_gpio_in_named(tof, SSI_GPIO_CS, 0));
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

    object_initialize_child(OBJECT(s), "soc", &s->soc, TYPE_RK3566_SOC);
    object_property_set_link(OBJECT(&s->soc), "memory",
                             OBJECT(get_system_memory()), &error_abort);
    qdev_prop_set_string(DEVICE(&s->soc), "camera-chardev", "camera");
    for (unsigned i = 0; i < RK3566_NUM_UARTS; i++) {
        g_autofree char *prop = g_strdup_printf("uart%u-chardev", i);

        qdev_prop_set_chr(DEVICE(&s->soc), prop,
                          serial_hd(i == 2 ? 0 : i == 3 ? 1 : i + 2));
    }
    qdev_prop_set_chr(DEVICE(&s->soc), "usb-chardev", serial_hd(4));
    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    memory_region_init_alias(&s->ram, OBJECT(s), "rk3566.ram", ms->ram, 0,
                             ms->ram_size);
    memory_region_add_subregion(get_system_memory(), 0, &s->ram);

    arm_register_psci_smc_handler(rk3566_smc);
    microduck_storage(s);
    microduck_board_devices(s, media);

    if (!ms->kernel_filename) {
        return;
    }
    s->boot.ram_size = ms->ram_size;
    s->boot.loader_start = 0;
    s->boot.board_id = -1;
    s->boot.psci_conduit = QEMU_PSCI_CONDUIT_SMC;
    s->boot.modify_dtb = rk3566_boot_dtb;
    arm_load_kernel(&s->soc.cpu[0], ms, &s->boot);
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
    mc->min_cpus = mc->default_cpus = mc->max_cpus = RK3566_NUM_CPUS;
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
