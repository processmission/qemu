/*
 * STM32G474 microcontroller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/stm32g474.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"

#define STM32G474_NUM_IRQS 102
#define STM32G474_NUM_PRIO_BITS 4
#define STM32G474_HSI16_FREQ_HZ 16000000
#define STM32G474_HSI48_FREQ_HZ 48000000
#define STM32G474_LSI_FREQ_HZ 32000
#define STM32G474_EXTI_9_5_NUM_LINES 5
#define STM32G474_EXTI_15_10_NUM_LINES 6

static const char *const stm32g474_gpio_types[STM32G474_GPIO_NUM_PORTS] = {
    TYPE_STM32G474_GPIO_A,
    TYPE_STM32G474_GPIO_B,
    TYPE_STM32G474_GPIO_CG,
    TYPE_STM32G474_GPIO_CG,
    TYPE_STM32G474_GPIO_CG,
    TYPE_STM32G474_GPIO_CG,
    TYPE_STM32G474_GPIO_CG,
};

static const char *const stm32g474_gpio_names[STM32G474_GPIO_NUM_PORTS] = {
    "gpioa", "gpiob", "gpioc", "gpiod", "gpioe", "gpiof", "gpiog",
};

static const hwaddr stm32g474_gpio_bases[STM32G474_GPIO_NUM_PORTS] = {
    STM32G474_GPIOA_BASE,
    STM32G474_GPIOB_BASE,
    STM32G474_GPIOC_BASE,
    STM32G474_GPIOD_BASE,
    STM32G474_GPIOE_BASE,
    STM32G474_GPIOF_BASE,
    STM32G474_GPIOG_BASE,
};

static const unsigned int
stm32g474_gpio_resets[STM32G474_GPIO_NUM_PORTS] = {
    STM32G474_RCC_RESET_GPIOA,
    STM32G474_RCC_RESET_GPIOB,
    STM32G474_RCC_RESET_GPIOC,
    STM32G474_RCC_RESET_GPIOD,
    STM32G474_RCC_RESET_GPIOE,
    STM32G474_RCC_RESET_GPIOF,
    STM32G474_RCC_RESET_GPIOG,
};

static const hwaddr
stm32g474_fdcan_bases[STM32G474_FDCAN_NUM_CHANNELS] = {
    STM32G474_FDCAN1_BASE,
    STM32G474_FDCAN2_BASE,
    STM32G474_FDCAN3_BASE,
};

static const unsigned int
stm32g474_fdcan_irqs[STM32G474_FDCAN_NUM_CHANNELS]
                    [STM32G474_FDCAN_NUM_IRQS] = {
    { STM32G474_FDCAN1_IT0_IRQ, STM32G474_FDCAN1_IT1_IRQ },
    { STM32G474_FDCAN2_IT0_IRQ, STM32G474_FDCAN2_IT1_IRQ },
    { STM32G474_FDCAN3_IT0_IRQ, STM32G474_FDCAN3_IT1_IRQ },
};

G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_gpio_types) ==
                STM32G474_GPIO_NUM_PORTS);
G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_gpio_names) ==
                STM32G474_GPIO_NUM_PORTS);
G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_gpio_bases) ==
                STM32G474_GPIO_NUM_PORTS);
G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_gpio_resets) ==
                STM32G474_GPIO_NUM_PORTS);
G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_fdcan_bases) ==
                STM32G474_FDCAN_NUM_CHANNELS);
G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_fdcan_irqs) ==
                STM32G474_FDCAN_NUM_CHANNELS);
G_STATIC_ASSERT(ARRAY_SIZE(stm32g474_fdcan_irqs[0]) ==
                STM32G474_FDCAN_NUM_IRQS);

static DeviceState *stm32g474_new_child(Object *parent, const char *name,
                                        const char *type)
{
    DeviceState *child = qdev_new(type);

    object_property_add_child(parent, name, OBJECT(child));
    object_unref(OBJECT(child));
    return child;
}

static void stm32g474_init(Object *obj)
{
    STM32G474State *s = STM32G474(obj);

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);
    s->rcc = stm32g474_new_child(obj, "rcc", TYPE_STM32G474_RCC);
    s->pwr = stm32g474_new_child(obj, "pwr", TYPE_STM32G474_PWR);
    s->flash = stm32g474_new_child(obj, "flash", TYPE_STM32G474_FLASH);
    s->syscfg = stm32g474_new_child(obj, "syscfg",
                                    TYPE_STM32G474_SYSCFG);
    s->exti = stm32g474_new_child(obj, "exti", TYPE_STM32G474_EXTI);
    object_initialize_child(obj, "exti-9-5-or", &s->exti_9_5_or,
                            TYPE_OR_IRQ);
    object_initialize_child(obj, "exti-15-10-or", &s->exti_15_10_or,
                            TYPE_OR_IRQ);
    s->usart1 = stm32g474_new_child(obj, "usart1",
                                    TYPE_STM32G474_USART);
    s->usart2 = stm32g474_new_child(obj, "usart2",
                                    TYPE_STM32G474_USART);
    s->uart4 = stm32g474_new_child(obj, "uart4",
                                   TYPE_STM32G474_UART);
    s->fdcan = stm32g474_new_child(obj, "fdcan",
                                   TYPE_STM32G474_FDCAN);
    s->usbfs = stm32g474_new_child(obj, "usbfs",
                                   TYPE_STM32G474_USBFS);
    for (unsigned int i = 0; i < STM32G474_GPIO_NUM_PORTS; i++) {
        s->gpio[i] = stm32g474_new_child(
            obj, stm32g474_gpio_names[i], stm32g474_gpio_types[i]);
    }

    /* Fixed-frequency clocks do not need migration state. */
    s->hsi16 = clock_new(obj, "hsi16");
    clock_set_hz(s->hsi16, STM32G474_HSI16_FREQ_HZ);
    s->hsi48 = clock_new(obj, "hsi48");
    clock_set_hz(s->hsi48, STM32G474_HSI48_FREQ_HZ);
    s->lsi = clock_new(obj, "lsi");
    clock_set_hz(s->lsi, STM32G474_LSI_FREQ_HZ);
    qdev_alias_clock(s->rcc, "hse-in", DEVICE(obj), "hse");
}

static void stm32g474_realize(DeviceState *dev, Error **errp)
{
    STM32G474State *s = STM32G474(dev);
    MemoryRegion *system_memory = get_system_memory();
    DeviceState *armv7m = DEVICE(&s->armv7m);
    DeviceState *rcc = s->rcc;
    DeviceState *pwr = s->pwr;
    DeviceState *flash = s->flash;
    DeviceState *syscfg = s->syscfg;
    DeviceState *exti = s->exti;
    DeviceState *exti_9_5_or = DEVICE(&s->exti_9_5_or);
    DeviceState *exti_15_10_or = DEVICE(&s->exti_15_10_or);
    DeviceState *usart1 = s->usart1;
    DeviceState *usart2 = s->usart2;
    DeviceState *uart4 = s->uart4;
    DeviceState *fdcan = s->fdcan;
    DeviceState *usbfs = s->usbfs;

    if (!memory_region_init_ram(&s->sram1, OBJECT(dev), "stm32g474.sram1",
                                STM32G474_SRAM1_SIZE, errp)) {
        return;
    }
    if (!memory_region_init_ram(&s->sram2, OBJECT(dev), "stm32g474.sram2",
                                STM32G474_SRAM2_SIZE, errp)) {
        return;
    }
    if (!memory_region_init_ram(&s->ccm_sram, OBJECT(dev),
                                "stm32g474.ccm-sram",
                                STM32G474_CCM_SRAM_SIZE, errp)) {
        return;
    }
    memory_region_init_alias(&s->ccm_sram_alias, OBJECT(dev),
                             "stm32g474.ccm-sram-alias", &s->ccm_sram, 0,
                             STM32G474_CCM_SRAM_SIZE);

    qdev_connect_clock_in(rcc, "hsi16-in", s->hsi16);
    qdev_connect_clock_in(rcc, "hsi48-in", s->hsi48);
    qdev_connect_clock_in(rcc, "lsi-in", s->lsi);
    qdev_connect_clock_in(armv7m, "cpuclk",
                          qdev_get_clock_out(rcc, "hclk"));
    qdev_connect_clock_in(armv7m, "refclk",
                          qdev_get_clock_out(rcc, "cortex-refclk"));
    qdev_connect_clock_in(pwr, "clk", qdev_get_clock_out(rcc, "pwr"));
    qdev_connect_clock_in(flash, "clk", qdev_get_clock_out(rcc, "flash"));
    qdev_connect_clock_in(syscfg, "clk",
                          qdev_get_clock_out(rcc, "syscfg"));
    qdev_connect_clock_in(exti, "clk",
                          qdev_get_clock_out(rcc, "syscfg"));
    qdev_prop_set_chr(uart4, "chardev", serial_hd(0));
    qdev_prop_set_chr(usart2, "chardev", serial_hd(1));
    qdev_prop_set_chr(usart1, "chardev", serial_hd(2));
    qdev_connect_clock_in(usart1, "clk",
                          qdev_get_clock_out(rcc, "usart1"));
    qdev_connect_clock_in(usart2, "clk",
                          qdev_get_clock_out(rcc, "usart2"));
    qdev_connect_clock_in(uart4, "clk",
                          qdev_get_clock_out(rcc, "uart4"));
    qdev_connect_clock_in(fdcan, "kernel-clk",
                          qdev_get_clock_out(rcc, "fdcan"));
    qdev_connect_clock_in(fdcan, "pclk",
                          qdev_get_clock_out(rcc, "pclk1"));
    qdev_connect_clock_in(usbfs, "pclk",
                          qdev_get_clock_out(rcc, "pclk1"));
    qdev_connect_clock_in(usbfs, "usb",
                          qdev_get_clock_out(rcc, "usb"));
    for (unsigned int i = 0; i < STM32G474_GPIO_NUM_PORTS; i++) {
        DeviceState *gpio = s->gpio[i];

        qdev_connect_clock_in(
            gpio, "clk",
            qdev_get_clock_out(rcc, stm32g474_gpio_names[i]));
        qdev_connect_gpio_out_named(
            rcc, "peripheral-reset", stm32g474_gpio_resets[i],
            qdev_get_gpio_in_named(gpio, "reset", 0));
        for (unsigned int pin = 0; pin < STM32G474_GPIO_NUM_PINS; pin++) {
            qdev_connect_gpio_out_named(
                gpio, "pin-out", pin,
                qdev_get_gpio_in_named(
                    syscfg, "gpio-in",
                    i * STM32G474_GPIO_NUM_PINS + pin));
        }
    }
    qdev_connect_gpio_out_named(
        rcc, "peripheral-reset", STM32G474_RCC_RESET_USART1,
        qdev_get_gpio_in_named(usart1, "reset", 0));
    qdev_connect_gpio_out_named(
        rcc, "peripheral-reset", STM32G474_RCC_RESET_USART2,
        qdev_get_gpio_in_named(usart2, "reset", 0));
    qdev_connect_gpio_out_named(
        rcc, "peripheral-reset", STM32G474_RCC_RESET_UART4,
        qdev_get_gpio_in_named(uart4, "reset", 0));
    qdev_connect_gpio_out_named(
        rcc, "peripheral-reset", STM32G474_RCC_RESET_PWR,
        qdev_get_gpio_in_named(pwr, "reset", 0));
    qdev_connect_gpio_out_named(
        rcc, "peripheral-reset", STM32G474_RCC_RESET_FLASH,
        qdev_get_gpio_in_named(flash, "reset", 0));
    qdev_connect_gpio_out_named(
        rcc, "peripheral-reset", STM32G474_RCC_RESET_SYSCFG,
        qdev_get_gpio_in_named(syscfg, "reset", 0));
    qdev_connect_gpio_out_named(
        rcc, "peripheral-reset", STM32G474_RCC_RESET_FDCAN,
        qdev_get_gpio_in_named(fdcan, "reset", 0));
    qdev_connect_gpio_out_named(
        rcc, "peripheral-reset", STM32G474_RCC_RESET_USB,
        qdev_get_gpio_in_named(usbfs, "reset", 0));
    if (!sysbus_realize(SYS_BUS_DEVICE(rcc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(rcc), 0, STM32G474_RCC_BASE);
    for (unsigned int i = 0; i < STM32G474_GPIO_NUM_PORTS; i++) {
        SysBusDevice *gpio = SYS_BUS_DEVICE(s->gpio[i]);

        if (!sysbus_realize(gpio, errp)) {
            return;
        }
        sysbus_mmio_map(gpio, 0, stm32g474_gpio_bases[i]);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(syscfg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(syscfg), 0, STM32G474_SYSCFG_BASE);
    qdev_pass_gpios(syscfg, dev, "gpio-in");
    for (unsigned int i = 0; i < STM32G474_SYSCFG_NUM_LINES; i++) {
        qdev_connect_gpio_out_named(
            syscfg, "exti-out", i,
            qdev_get_gpio_in_named(exti, "line-in", i));
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(exti), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(exti), 0, STM32G474_EXTI_BASE);
    qdev_prop_set_uint16(exti_9_5_or, "num-lines",
                         STM32G474_EXTI_9_5_NUM_LINES);
    if (!qdev_realize(exti_9_5_or, NULL, errp)) {
        return;
    }
    qdev_prop_set_uint16(exti_15_10_or, "num-lines",
                         STM32G474_EXTI_15_10_NUM_LINES);
    if (!qdev_realize(exti_15_10_or, NULL, errp)) {
        return;
    }
    for (unsigned int i = 0; i < STM32G474_EXTI_9_5_NUM_LINES; i++) {
        sysbus_connect_irq(
            SYS_BUS_DEVICE(exti), i + 5,
            qdev_get_gpio_in(exti_9_5_or, i));
    }
    for (unsigned int i = 0; i < STM32G474_EXTI_15_10_NUM_LINES; i++) {
        sysbus_connect_irq(
            SYS_BUS_DEVICE(exti), i + 10,
            qdev_get_gpio_in(exti_15_10_or, i));
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(usart1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(usart1), 0, STM32G474_USART1_BASE);
    if (!sysbus_realize(SYS_BUS_DEVICE(usart2), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(usart2), 0, STM32G474_USART2_BASE);
    if (!sysbus_realize(SYS_BUS_DEVICE(uart4), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(uart4), 0, STM32G474_UART4_BASE);
    if (!sysbus_realize(SYS_BUS_DEVICE(pwr), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(pwr), 0, STM32G474_PWR_BASE);
    if (!sysbus_realize(SYS_BUS_DEVICE(flash), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(flash), 0, STM32G474_FLASH_IF_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(flash), 1, STM32G474_FLASH_BASE);
    memory_region_init_alias(
        &s->flash_alias, OBJECT(dev), "stm32g474.flash-boot-alias",
        sysbus_mmio_get_region(SYS_BUS_DEVICE(flash), 1), 0,
        STM32G474_FLASH_SIZE);
    memory_region_add_subregion(system_memory, 0, &s->flash_alias);
    sysbus_mmio_map(SYS_BUS_DEVICE(flash), 2, STM32G474_FLASH_SIZE_BASE);
    for (unsigned int i = 0;
         i < STM32G474_FDCAN_NUM_CHANNELS; i++) {
        g_autofree char *bus_name = g_strdup_printf("canbus%u", i);

        if (!object_property_set_link(OBJECT(fdcan), bus_name,
                                      OBJECT(s->canbus[i]), errp)) {
            return;
        }
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(fdcan), errp)) {
        return;
    }
    for (unsigned int i = 0;
         i < STM32G474_FDCAN_NUM_CHANNELS; i++) {
        sysbus_mmio_map(SYS_BUS_DEVICE(fdcan), i,
                        stm32g474_fdcan_bases[i]);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(fdcan),
                    STM32G474_FDCAN_NUM_CHANNELS,
                    STM32G474_FDCAN_MRAM_BASE);
    if (!sysbus_realize(SYS_BUS_DEVICE(usbfs), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(usbfs), 0, STM32G474_USBFS_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(usbfs), 1, STM32G474_USBFS_PMA_BASE);

    memory_region_add_subregion(system_memory, STM32G474_SRAM1_BASE,
                                &s->sram1);
    memory_region_add_subregion(system_memory, STM32G474_SRAM2_BASE,
                                &s->sram2);
    memory_region_add_subregion(system_memory, STM32G474_CCM_SRAM_BASE,
                                &s->ccm_sram);
    memory_region_add_subregion(system_memory, STM32G474_CCM_SRAM_ALIAS,
                                &s->ccm_sram_alias);

    qdev_prop_set_uint32(armv7m, "num-irq", STM32G474_NUM_IRQS);
    qdev_prop_set_uint32(armv7m, "num-prio-bits",
                         STM32G474_NUM_PRIO_BITS);
    qdev_prop_set_string(armv7m, "cpu-type",
                         ARM_CPU_TYPE_NAME("cortex-m4"));
    qdev_prop_set_bit(armv7m, "enable-bitband", true);
    if (!object_property_set_link(OBJECT(&s->armv7m), "memory",
                                  OBJECT(system_memory), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }
    for (unsigned int i = 0; i < STM32G474_EXTI_9_5_NUM_LINES; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(exti), i,
                           qdev_get_gpio_in(armv7m,
                                            STM32G474_EXTI0_IRQ + i));
    }
    qdev_connect_gpio_out(
        exti_9_5_or, 0,
        qdev_get_gpio_in(armv7m, STM32G474_EXTI9_5_IRQ));
    qdev_connect_gpio_out(
        exti_15_10_or, 0,
        qdev_get_gpio_in(armv7m, STM32G474_EXTI15_10_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(rcc), 0,
                       qdev_get_gpio_in(armv7m, STM32G474_RCC_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(flash), 0,
                       qdev_get_gpio_in(armv7m, STM32G474_FLASH_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(usart1), 0,
                       qdev_get_gpio_in(armv7m, STM32G474_USART1_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(usart2), 0,
                       qdev_get_gpio_in(armv7m, STM32G474_USART2_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(uart4), 0,
                       qdev_get_gpio_in(armv7m, STM32G474_UART4_IRQ));
    for (unsigned int channel = 0;
         channel < STM32G474_FDCAN_NUM_CHANNELS; channel++) {
        for (unsigned int line = 0;
             line < STM32G474_FDCAN_NUM_IRQS; line++) {
            sysbus_connect_irq(
                SYS_BUS_DEVICE(fdcan),
                channel * STM32G474_FDCAN_NUM_IRQS + line,
                qdev_get_gpio_in(armv7m,
                                 stm32g474_fdcan_irqs[channel][line]));
        }
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(usbfs), 0,
                       qdev_get_gpio_in(armv7m,
                                        STM32G474_USBFS_HP_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(usbfs), 1,
                       qdev_get_gpio_in(armv7m,
                                        STM32G474_USBFS_LP_IRQ));
}

static const Property stm32g474_properties[] = {
    DEFINE_PROP_LINK("canbus0", STM32G474State, canbus[0],
                     TYPE_CAN_BUS, CanBusState *),
    DEFINE_PROP_LINK("canbus1", STM32G474State, canbus[1],
                     TYPE_CAN_BUS, CanBusState *),
    DEFINE_PROP_LINK("canbus2", STM32G474State, canbus[2],
                     TYPE_CAN_BUS, CanBusState *),
};

static void stm32g474_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_props(dc, stm32g474_properties);
    dc->realize = stm32g474_realize;
    dc->user_creatable = false;
}

static const TypeInfo stm32g474_types[] = {
    {
        .name = TYPE_STM32G474,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(STM32G474State),
        .instance_init = stm32g474_init,
        .class_init = stm32g474_class_init,
    },
};

DEFINE_TYPES(stm32g474_types)
