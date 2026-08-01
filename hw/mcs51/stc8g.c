/*
 * STC8G1K08A SoC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/adc/stc8g_adc.h"
#include "hw/char/stc8g_uart.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/qdev-clock.h"
#include "hw/gpio/stc8g_gpio.h"
#include "hw/intc/stc8g_intc.h"
#include "hw/i2c/stc8g_i2c.h"
#include "hw/mcs51/stc8g.h"
#include "hw/misc/stc8g_mdu.h"
#include "hw/misc/stc8g_lvd.h"
#include "hw/misc/stc8g_sysctrl.h"
#include "hw/nvram/stc8g_iap.h"
#include "hw/ssi/stc8g_spi.h"
#include "hw/timer/stc8g_pca.h"
#include "hw/timer/stc32g_timer.h"
#include "hw/watchdog/stc8g_wdt.h"
#include "system/address-spaces.h"
#include "system/system.h"

#define STC8G_SFR_PHYS_ADDR(addr) \
    (MCS51_SFR_PHYS_BASE + (addr) - MCS251_SFR_BASE)
#define STC8G_XFR_PHYS_ADDR(addr) \
    (MCS51_XFR_PHYS_BASE + (addr) - MCS51_XFR_VIRT_BASE)

#define STC8G_SFR_TCON 0x88u
#define STC8G_SFR_SCON 0x98u
#define STC8G_SFR_IE2 0xafu
#define STC8G_SFR_IP2 0xb5u
#define STC8G_SFR_IP2H 0xb6u
#define STC8G_SFR_P3 0xb0u
#define STC8G_SFR_SPSTAT 0xcdu
#define STC8G_SFR_SPCTL 0xceu
#define STC8G_SFR_SPDAT 0xcfu
#define STC8G_SFR_AUXINTIF 0xefu
#define STC8G_SFR_ADC_CONTR 0xbcu
#define STC8G_SFR_ADC_RES 0xbdu
#define STC8G_SFR_ADC_RESL 0xbeu
#define STC8G_SFR_ADCCFG 0xdeu
#define STC8G_SFR_CCON 0xd8u
#define STC8G_SFR_CMOD 0xd9u
#define STC8G_SFR_CCAPM0 0xdau
#define STC8G_SFR_CL 0xe9u
#define STC8G_SFR_CCAP0L 0xeau
#define STC8G_SFR_PCA_PWM0 0xf2u
#define STC8G_SFR_CH 0xf9u
#define STC8G_SFR_CCAP0H 0xfau
#define STC8G_SFR_WDT_CONTR 0xc1u
#define STC8G_SFR_IAP_DATA 0xc2u
#define STC8G_SFR_IAP_TPS 0xf5u
#define STC8G_SFR_RSTCFG 0xffu
#define STC8G_XFR_P3PU 0xfe13u
#define STC8G_XFR_I2C 0xfe80u
#define STC8G_XFR_SYSCTRL 0xfe00u
#define STC8G_XFR_ADCTIM 0xfea8u
#define STC8G_XFR_MDU 0xfcf0u

static void stc8g_soc_realize(DeviceState *dev, Error **errp)
{
    Stc8gSoCState *s = STC8G_SOC(dev);
    MemoryRegion *sysmem = get_system_memory();
    unsigned i;

    if (!memory_region_init_rom(&s->flash, OBJECT(s), "stc8g.flash",
                                STC8G_FLASH_SIZE, errp)) {
        return;
    }
    memset(memory_region_get_ram_ptr(&s->flash), 0xff,
           STC8G_FLASH_SIZE);
    if (!memory_region_init_ram(&s->idata, OBJECT(s), "stc8g.idata",
                                STC8G_IDATA_SIZE, errp)) {
        return;
    }
    if (!memory_region_init_ram(&s->xdata, OBJECT(s), "stc8g.xdata",
                                STC8G_XDATA_SIZE, errp)) {
        return;
    }

    if (!qdev_realize(DEVICE(&s->cpu), NULL, errp)) {
        return;
    }
    object_property_set_link(OBJECT(s->gpio), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->adc), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->sysctrl), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->timer), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->intc), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->lvd), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->pca), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->uart), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    object_property_set_link(OBJECT(s->wdt), "cpu", OBJECT(&s->cpu),
                             &error_abort);
    qdev_prop_set_chr(s->uart, "chardev", serial_hd(0));
    qdev_connect_clock_in(s->adc, "sysclk",
                          qdev_get_clock_out(s->sysctrl, "sysclk"));
    qdev_connect_clock_in(s->i2c, "sysclk",
                          qdev_get_clock_out(s->sysctrl, "sysclk"));
    qdev_connect_clock_in(s->iap, "sysclk",
                          qdev_get_clock_out(s->sysctrl, "sysclk"));
    qdev_connect_clock_in(s->mdu, "sysclk",
                          qdev_get_clock_out(s->sysctrl, "sysclk"));
    qdev_connect_clock_in(s->pca, "sysclk",
                          qdev_get_clock_out(s->sysctrl, "sysclk"));
    qdev_connect_clock_in(s->spi, "sysclk",
                          qdev_get_clock_out(s->sysctrl, "sysclk"));
    qdev_connect_clock_in(s->timer, "sysclk",
                          qdev_get_clock_out(s->sysctrl, "sysclk"));
    qdev_connect_clock_in(s->wdt, "sysclk",
                          qdev_get_clock_out(s->sysctrl, "sysclk"));
    if (!sysbus_realize(SYS_BUS_DEVICE(s->gpio), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->sysctrl), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->adc), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->timer), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->i2c), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->iap), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->intc), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->lvd), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->mdu), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->pca), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->spi), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->uart), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(s->wdt), errp)) {
        return;
    }

    memory_region_add_subregion(sysmem, STC8G_FLASH_BASE, &s->flash);
    memory_region_add_subregion(sysmem, STC8G_IDATA_BASE, &s->idata);
    memory_region_add_subregion(sysmem, STC8G_XDATA_BASE, &s->xdata);
    memory_region_add_subregion(sysmem, MCS51_SFR_PHYS_BASE, &s->cpu.sfr);
    memory_region_add_subregion(sysmem, MCS51_DISABLED_PHYS_BASE,
                                &s->cpu.disabled);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->iap), STC8G_IAP_MMIO_EEPROM,
                    STC8G_EEPROM_BASE);

    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->timer), 0,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_TCON), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->adc), STC8G_ADC_MMIO_CONTR,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_ADC_CONTR), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->adc), STC8G_ADC_MMIO_RES,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_ADC_RES), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->adc), STC8G_ADC_MMIO_RESL,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_ADC_RESL), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->adc), STC8G_ADC_MMIO_CFG,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_ADCCFG), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->adc), STC8G_ADC_MMIO_TIM,
                            STC8G_XFR_PHYS_ADDR(STC8G_XFR_ADCTIM), 1);
    for (i = 0; i < STC8G_I2C_MMIO_REGS; i++) {
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->i2c), i,
                                STC8G_XFR_PHYS_ADDR(STC8G_XFR_I2C + i), 1);
    }
    for (i = 0; i < STC8G_IAP_MMIO_REGS; i++) {
        static const uint8_t iap_sfrs[] = {
            STC8G_SFR_IAP_DATA, STC8G_SFR_IAP_DATA + 1,
            STC8G_SFR_IAP_DATA + 2, STC8G_SFR_IAP_DATA + 3,
            STC8G_SFR_IAP_DATA + 4, STC8G_SFR_IAP_DATA + 5,
            STC8G_SFR_IAP_TPS,
        };

        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->iap), i,
                                STC8G_SFR_PHYS_ADDR(iap_sfrs[i]), 1);
    }
    for (i = 0; i < STC8G_SYSCTRL_MMIO_REGS; i++) {
        static const uint16_t sysctrl_regs[] = {
            STC8G_XFR_SYSCTRL, STC8G_XFR_SYSCTRL + 1,
            STC8G_XFR_SYSCTRL + 2, STC8G_XFR_SYSCTRL + 3,
            STC8G_XFR_SYSCTRL + 4, STC8G_XFR_SYSCTRL + 5,
            STC8G_XFR_SYSCTRL + 6, 0x009d, 0x009e, 0x009f,
        };
        uint64_t address = i < 7 ? STC8G_XFR_PHYS_ADDR(sysctrl_regs[i]) :
                           STC8G_SFR_PHYS_ADDR(sysctrl_regs[i]);

        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->sysctrl), i, address, 1);
    }
    for (i = 0; i < STC8G_MDU_MMIO_REGS; i++) {
        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->mdu), i,
                                STC8G_XFR_PHYS_ADDR(STC8G_XFR_MDU + i), 1);
    }
    for (i = 0; i < STC8G_PCA_MMIO_REGS; i++) {
        static const uint8_t pca_sfrs[] = {
            STC8G_SFR_CCON, STC8G_SFR_CMOD, STC8G_SFR_CCAPM0,
            STC8G_SFR_CCAPM0 + 1, STC8G_SFR_CCAPM0 + 2,
            STC8G_SFR_CL, STC8G_SFR_CCAP0L, STC8G_SFR_CCAP0L + 1,
            STC8G_SFR_CCAP0L + 2, STC8G_SFR_PCA_PWM0,
            STC8G_SFR_PCA_PWM0 + 1, STC8G_SFR_PCA_PWM0 + 2,
            STC8G_SFR_CH, STC8G_SFR_CCAP0H, STC8G_SFR_CCAP0H + 1,
            STC8G_SFR_CCAP0H + 2,
        };

        sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->pca), i,
                                STC8G_SFR_PHYS_ADDR(pca_sfrs[i]), 1);
    }
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->intc), STC8G_INTC_MMIO_IE2,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_IE2), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->intc), STC8G_INTC_MMIO_IP2,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_IP2), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->intc), STC8G_INTC_MMIO_IP2H,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_IP2H), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->intc),
                            STC8G_INTC_MMIO_AUXINTIF,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_AUXINTIF), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->spi), STC8G_SPI_MMIO_STAT,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_SPSTAT), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->spi), STC8G_SPI_MMIO_CTL,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_SPCTL), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->spi), STC8G_SPI_MMIO_DATA,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_SPDAT), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->uart), 0,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_SCON), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->wdt), STC8G_WDT_MMIO_CONTR,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_WDT_CONTR), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->lvd), STC8G_LVD_MMIO_RSTCFG,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_RSTCFG), 1);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(s->gpio), 0,
                            STC8G_SFR_PHYS_ADDR(STC8G_SFR_P3), 1);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gpio), 1,
                    STC8G_XFR_PHYS_ADDR(STC8G_XFR_P3PU));

    for (i = 0; i < 4; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s->timer), i,
                           qdev_get_gpio_in(DEVICE(&s->cpu), i));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(s->uart), 0,
                       qdev_get_gpio_in(DEVICE(&s->cpu),
                                        MCS251_IRQ_UART1));
    for (i = 0; i < STC8G_INTC_NUM_SOURCES; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(s->intc), i,
                           qdev_get_gpio_in(DEVICE(&s->cpu),
                                            MCS251_IRQ_ADC + i));
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(s->adc), 0,
                       qdev_get_gpio_in_named(s->intc, "irq-in",
                                               STC8G_INTC_ADC));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->spi), 0,
                       qdev_get_gpio_in_named(s->intc, "irq-in",
                                               STC8G_INTC_SPI));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->i2c), 0,
                       qdev_get_gpio_in_named(s->intc, "irq-in",
                                               STC8G_INTC_I2C));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->lvd), 0,
                       qdev_get_gpio_in_named(s->intc, "irq-in",
                                               STC8G_INTC_LVD));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->pca), 0,
                       qdev_get_gpio_in_named(s->intc, "irq-in",
                                               STC8G_INTC_PCA));
    qdev_connect_gpio_out_named(s->timer, "pca-clock", 0,
        qdev_get_gpio_in_named(s->pca, "timer0-overflow", 0));
    for (i = 0; i < 2; i++) {
        qdev_connect_gpio_out_named(s->gpio, "int-line", i,
            qdev_get_gpio_in_named(s->timer, "gate", i));
        qdev_connect_gpio_out_named(s->gpio, "counter-line", i,
            qdev_get_gpio_in_named(s->timer, "counter", i));
    }
}

static void stc8g_soc_init(Object *obj)
{
    Stc8gSoCState *s = STC8G_SOC(obj);

    object_initialize_child(obj, "cpu", &s->cpu, TYPE_MCS51_CPU);
    s->adc = qdev_new(TYPE_STC8G_ADC);
    object_property_add_child(obj, "adc", OBJECT(s->adc));
    s->gpio = qdev_new(TYPE_STC8G_GPIO);
    object_property_add_child(obj, "gpio", OBJECT(s->gpio));
    s->i2c = qdev_new(TYPE_STC8G_I2C);
    object_property_add_child(obj, "i2c", OBJECT(s->i2c));
    s->iap = qdev_new(TYPE_STC8G_IAP);
    object_property_add_child(obj, "iap", OBJECT(s->iap));
    s->intc = qdev_new(TYPE_STC8G_INTC);
    object_property_add_child(obj, "intc", OBJECT(s->intc));
    s->lvd = qdev_new(TYPE_STC8G_LVD);
    object_property_add_child(obj, "lvd", OBJECT(s->lvd));
    s->mdu = qdev_new(TYPE_STC8G_MDU);
    object_property_add_child(obj, "mdu", OBJECT(s->mdu));
    s->pca = qdev_new(TYPE_STC8G_PCA);
    object_property_add_child(obj, "pca", OBJECT(s->pca));
    s->spi = qdev_new(TYPE_STC8G_SPI);
    object_property_add_child(obj, "spi", OBJECT(s->spi));
    s->sysctrl = qdev_new(TYPE_STC8G_SYSCTRL);
    object_property_add_child(obj, "sysctrl", OBJECT(s->sysctrl));
    s->timer = qdev_new(TYPE_STC8G_TIMER);
    object_property_add_child(obj, "timer", OBJECT(s->timer));
    s->uart = qdev_new(TYPE_STC8G_UART);
    object_property_add_child(obj, "uart1", OBJECT(s->uart));
    s->wdt = qdev_new(TYPE_STC8G_WDT);
    object_property_add_child(obj, "wdt", OBJECT(s->wdt));
}

static void stc8g_soc_reset(DeviceState *dev)
{
    Stc8gSoCState *s = STC8G_SOC(dev);

    cpu_reset(CPU(&s->cpu));
}

static void stc8g_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = stc8g_soc_realize;
    device_class_set_legacy_reset(dc, stc8g_soc_reset);
}

static const TypeInfo stc8g_soc_type = {
    .name = TYPE_STC8G_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Stc8gSoCState),
    .instance_init = stc8g_soc_init,
    .class_init = stc8g_soc_class_init,
};

static void stc8g_soc_register_types(void)
{
    type_register_static(&stc8g_soc_type);
}

type_init(stc8g_soc_register_types)
