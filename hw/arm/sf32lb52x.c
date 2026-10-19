/*
 * SiFli SF32LB52X SoC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/arm/sf32lb52x.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/misc/unimp.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define SF32LB52X_RCC_CSR        0x20
#define SF32LB52X_RCC_DLL1CR     0x2c
#define SF32LB52X_RCC_DLL2CR     0x30
#define SF32LB52X_RCC_DLL_EN     (1U << 0)
#define SF32LB52X_RCC_DLL_READY  (1U << 31)

#define SF32LB52X_AON_ACR        0x10
#define SF32LB52X_AON_HRC48_REQ  (1U << 0)
#define SF32LB52X_AON_HXT48_REQ  (1U << 1)
#define SF32LB52X_AON_HRC48_RDY  (1U << 30)
#define SF32LB52X_AON_HXT48_RDY  (1U << 31)

#define SF32LB52X_NUM_PRIO_BITS  3

static const hwaddr sf32lb52x_usart_base[SF32LB52X_NUM_USARTS] = {
    SF32LB52X_USART1_BASE,
    SF32LB52X_USART1_BASE + SF32LB52X_USART_STRIDE,
    SF32LB52X_USART1_BASE + 2 * SF32LB52X_USART_STRIDE,
};

static const int sf32lb52x_usart_irq[SF32LB52X_NUM_USARTS] = {
    59, 74, 95,
};

static const int sf32lb52x_i2c_irq[SF32LB52X_NUM_I2C] = {
    61, 76, 93, 78,
};

/*
 * Peripheral windows that are not modeled. Accesses are logged as
 * unimplemented instead of faulting the guest, so that firmware which
 * probes an unmodeled block still reaches the console.
 */
static const struct {
    const char *name;
    hwaddr base;
    hwaddr size;
} sf32lb52x_unimplemented[] = {
    { "sf32lb52x.crypto",  0x5000d000, 0x1000 },
    { "sf32lb52x.mpi1",    0x50041000, 0x1000 },
};

static uint64_t sf32lb52x_syscon_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    SF32LB52XRegBank *bank = opaque;

    return bank->regs[addr / 4];
}

static void sf32lb52x_syscon_write(void *opaque, hwaddr addr,
                                   uint64_t value, unsigned int size)
{
    SF32LB52XRegBank *bank = opaque;

    bank->regs[addr / 4] = value;
}

/*
 * The RCC DLLs report READY as soon as the firmware enables them. The
 * guest polls this bit in a loop without a timeout, so the value must
 * not depend on a timer.
 */
static uint64_t sf32lb52x_rcc_read(void *opaque, hwaddr addr,
                                   unsigned int size)
{
    SF32LB52XRegBank *bank = opaque;
    uint32_t value = bank->regs[addr / 4];

    if ((addr == SF32LB52X_RCC_DLL1CR || addr == SF32LB52X_RCC_DLL2CR) &&
        (value & SF32LB52X_RCC_DLL_EN)) {
        value |= SF32LB52X_RCC_DLL_READY;
    }

    return value;
}

/*
 * The AON block reports the HXT48 and HRC48 oscillators as ready once
 * the firmware requests them; the clock driver polls these bits.
 */
static uint64_t sf32lb52x_aon_read(void *opaque, hwaddr addr,
                                   unsigned int size)
{
    SF32LB52XRegBank *bank = opaque;
    uint32_t value = bank->regs[addr / 4];

    if (addr == SF32LB52X_AON_ACR) {
        if (value & SF32LB52X_AON_HXT48_REQ) {
            value |= SF32LB52X_AON_HXT48_RDY;
        }
        if (value & SF32LB52X_AON_HRC48_REQ) {
            value |= SF32LB52X_AON_HRC48_RDY;
        }
    }

    return value;
}

static const MemoryRegionOps sf32lb52x_syscon_ops = {
    .read = sf32lb52x_syscon_read,
    .write = sf32lb52x_syscon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps sf32lb52x_rcc_ops = {
    .read = sf32lb52x_rcc_read,
    .write = sf32lb52x_syscon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps sf32lb52x_aon_ops = {
    .read = sf32lb52x_aon_read,
    .write = sf32lb52x_syscon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void sf32lb52x_regbank_init(SF32LB52XRegBank *bank, Object *owner,
                                   const char *name,
                                   const MemoryRegionOps *ops,
                                   MemoryRegion *memory, hwaddr base)
{
    memory_region_init_io(&bank->mmio, owner, ops, bank, name,
                          SF32LB52X_SYSCON_SIZE);
    memory_region_add_subregion(memory, base, &bank->mmio);
}

static void sf32lb52x_create_unimplemented_devices(MemoryRegion *memory)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(sf32lb52x_unimplemented); i++) {
        DeviceState *dev = qdev_new(TYPE_UNIMPLEMENTED_DEVICE);

        qdev_prop_set_string(dev, "name", sf32lb52x_unimplemented[i].name);
        qdev_prop_set_uint64(dev, "size", sf32lb52x_unimplemented[i].size);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);
        memory_region_add_subregion_overlap(memory,
                                            sf32lb52x_unimplemented[i].base,
                                            sysbus_mmio_get_region(
                                                SYS_BUS_DEVICE(dev), 0),
                                            -1000);
    }
}

static void sf32lb52x_soc_instance_init(Object *obj)
{
    SF32LB52XSoCState *s = SF32LB52X_SOC(obj);
    int i;

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);

    for (i = 0; i < SF32LB52X_NUM_USARTS; i++) {
        g_autofree char *name = g_strdup_printf("usart%d-chardev", i + 1);

        object_initialize_child(obj, "usart[*]", &s->usart[i],
                                TYPE_SF32LB_USART);
        object_property_add_alias(obj, name, OBJECT(&s->usart[i]),
                                  "chardev");
    }

    object_initialize_child(obj, "rtc", &s->rtc, TYPE_SF32LB52X_RTC);
    object_initialize_child(obj, "wdt", &s->wdt, TYPE_SF32LB52X_WDT);
    object_initialize_child(obj, "crc", &s->crc, TYPE_SF32LB52X_CRC);
    object_initialize_child(obj, "trng", &s->trng, TYPE_SF32LB52X_TRNG);
    object_initialize_child(obj, "tsen", &s->tsen, TYPE_SF32LB52X_TSEN);
    object_initialize_child(obj, "gpadc", &s->gpadc, TYPE_SF32LB52X_GPADC);
    object_initialize_child(obj, "efuse", &s->efuse, TYPE_SF32LB52X_EFUSE);
    for (i = 0; i < SF32LB52X_NUM_I2C; i++) {
        object_initialize_child(obj, "i2c[*]", &s->i2c[i],
                                TYPE_SF32LB52X_I2C);
    }

    s->sysclk = qdev_init_clock_in(DEVICE(obj), "sysclk", NULL, NULL, 0);
}

static void sf32lb52x_soc_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XSoCState *s = SF32LB52X_SOC(obj);

    memset(s->rcc.regs, 0, sizeof(s->rcc.regs));
    memset(s->aon.regs, 0, sizeof(s->aon.regs));
    memset(s->pmuc.regs, 0, sizeof(s->pmuc.regs));
    memset(s->cfg.regs, 0, sizeof(s->cfg.regs));
    memset(s->pinmux.regs, 0, sizeof(s->pinmux.regs));
    memset(s->gpio.regs, 0, sizeof(s->gpio.regs));
}

/*
 * Realize one of the small system peripherals, map its register window and
 * route its interrupt to the NVIC when it has one.
 */
static bool sf32lb52x_simple_device_realize(DeviceState *soc,
                                            DeviceState *dev, hwaddr base,
                                            bool has_irq, int irq,
                                            Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, base);
    if (has_irq) {
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(DEVICE(&SF32LB52X_SOC(soc)->armv7m),
                                            irq));
    }
    return true;
}

static void sf32lb52x_soc_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();
    SF32LB52XSoCState *s = SF32LB52X_SOC(dev);
    DeviceState *armv7m;
    int i;

    if (!s->memory) {
        error_setg(errp, "sf32lb52x: memory link is required");
        return;
    }

    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "sf32lb52x: sysclk must be connected");
        return;
    }

    if (!memory_region_init_rom(&s->flash, OBJECT(dev), "sf32lb52x.flash",
                                SF32LB52X_FLASH_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(s->memory, SF32LB52X_FLASH_BASE, &s->flash);

    if (!memory_region_init_ram(&s->sram, OBJECT(dev), "sf32lb52x.sram",
                                SF32LB52X_SRAM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(s->memory, SF32LB52X_SRAM_BASE, &s->sram);

    if (!memory_region_init_ram(&s->sram1, OBJECT(dev), "sf32lb52x.sram1",
                                SF32LB52X_SRAM1_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(s->memory, SF32LB52X_SRAM1_BASE, &s->sram1);

    sf32lb52x_regbank_init(&s->rcc, OBJECT(dev), "sf32lb52x.rcc",
                           &sf32lb52x_rcc_ops, s->memory, SF32LB52X_RCC_BASE);
    sf32lb52x_regbank_init(&s->pinmux, OBJECT(dev), "sf32lb52x.pinmux",
                           &sf32lb52x_syscon_ops, s->memory,
                           SF32LB52X_PINMUX_BASE);
    sf32lb52x_regbank_init(&s->cfg, OBJECT(dev), "sf32lb52x.cfg",
                           &sf32lb52x_syscon_ops, s->memory,
                           SF32LB52X_CFG_BASE);
    sf32lb52x_regbank_init(&s->gpio, OBJECT(dev), "sf32lb52x.gpio",
                           &sf32lb52x_syscon_ops, s->memory,
                           SF32LB52X_GPIO_BASE);
    sf32lb52x_regbank_init(&s->aon, OBJECT(dev), "sf32lb52x.aon",
                           &sf32lb52x_aon_ops, s->memory, SF32LB52X_AON_BASE);
    sf32lb52x_regbank_init(&s->pmuc, OBJECT(dev), "sf32lb52x.pmuc",
                           &sf32lb52x_syscon_ops, s->memory,
                           SF32LB52X_PMUC_BASE);

    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", SF32LB52X_NUM_IRQS);
    qdev_prop_set_uint32(armv7m, "num-prio-bits", SF32LB52X_NUM_PRIO_BITS);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m33"));
    qdev_prop_set_uint32(armv7m, "init-svtor", SF32LB52X_CODE_BASE);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(s->memory), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }

    for (i = 0; i < SF32LB52X_NUM_USARTS; i++) {
        SysBusDevice *usart = SYS_BUS_DEVICE(&s->usart[i]);

        if (!sysbus_realize(usart, errp)) {
            return;
        }
        sysbus_mmio_map(usart, 0, sf32lb52x_usart_base[i]);
        sysbus_connect_irq(usart, 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m),
                                            sf32lb52x_usart_irq[i]));
    }

    object_property_set_link(OBJECT(&s->rtc), "cfg", OBJECT(&s->cfg.mmio),
                             &error_abort);

    if (!sf32lb52x_simple_device_realize(dev, DEVICE(&s->rtc),
                                         SF32LB52X_RTC_BASE, true, 49,
                                         errp) ||
        !sf32lb52x_simple_device_realize(dev, DEVICE(&s->wdt),
                                         SF32LB52X_WDT_BASE, true, 26,
                                         errp) ||
        !sf32lb52x_simple_device_realize(dev, DEVICE(&s->crc),
                                         SF32LB52X_CRC_BASE, false, 0,
                                         errp) ||
        !sf32lb52x_simple_device_realize(dev, DEVICE(&s->trng),
                                         SF32LB52X_TRNG_BASE, true, 69,
                                         errp) ||
        !sf32lb52x_simple_device_realize(dev, DEVICE(&s->tsen),
                                         SF32LB52X_TSEN_BASE, false, 0,
                                         errp) ||
        !sf32lb52x_simple_device_realize(dev, DEVICE(&s->gpadc),
                                         SF32LB52X_GPADC_BASE, true, 65,
                                         errp) ||
        !sf32lb52x_simple_device_realize(dev, DEVICE(&s->efuse),
                                         SF32LB52X_EFUSE_BASE, false, 0,
                                         errp)) {
        return;
    }

    for (i = 0; i < SF32LB52X_NUM_I2C; i++) {
        DeviceState *i2c = DEVICE(&s->i2c[i]);
        SysBusDevice *sbd = SYS_BUS_DEVICE(i2c);

        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0,
                        SF32LB52X_I2C1_BASE + i * SF32LB52X_USART_STRIDE);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(DEVICE(&s->armv7m),
                                            sf32lb52x_i2c_irq[i]));
    }

    sf32lb52x_create_unimplemented_devices(s->memory);
}

static const Property sf32lb52x_soc_properties[] = {
    DEFINE_PROP_LINK("memory", SF32LB52XSoCState, memory,
                     TYPE_MEMORY_REGION, MemoryRegion *),
};

static const VMStateDescription vmstate_sf32lb52x_soc = {
    .name = TYPE_SF32LB52X_SOC,
    .unmigratable = true,
};

static void sf32lb52x_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_soc_reset_hold;
    dc->realize = sf32lb52x_soc_realize;
    dc->desc = "SiFli SF32LB52X";
    dc->vmsd = &vmstate_sf32lb52x_soc;
    dc->user_creatable = false;
    device_class_set_props(dc, sf32lb52x_soc_properties);
}

static const TypeInfo sf32lb52x_soc_types[] = {
    {
        .name = TYPE_SF32LB52X_SOC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XSoCState),
        .instance_init = sf32lb52x_soc_instance_init,
        .class_init = sf32lb52x_soc_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_soc_types);
