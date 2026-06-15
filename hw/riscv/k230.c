/*
 * QEMU RISC-V Virt Board Compatible with Kendryte K230 SDK
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Provides a board compatible with the Kendryte K230 SDK
 *
 * K230 Technical Reference Manual V0.3.1 (2024-11-18):
 * https://github.com/revyos/external-docs/blob/master/K230/en-us/K230_Technical_Reference_Manual_V0.3.1_20241118.pdf
 *
 * For more information, see <https://www.kendryte.com/en/proDetail/230>
 */

#include "qemu/osdep.h"
#include "cpu-qom.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "system/device_tree.h"
#include "system/system.h"
#include "system/memory.h"
#include "system/block-backend.h"
#include "system/blockdev.h"
#include "target/riscv/cpu.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/riscv/k230.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/machines-qom.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/unimp.h"
#include "hw/sd/sd.h"
#include "hw/usb/usb.h"
#include "net/net.h"

/* Align K230_SDK k230_canmv_defconfig */
#define K230_DIRECT_OPENSBI_ADDR 0x8000000
#define K230_DIRECT_KERNEL_ADDR  0x8200000
#define K230_DIRECT_DTB_ADDR     0xa000000

#define K230_NOC_QOS_BASE          0x91302000
#define K230_NOC_QOS_SIZE          0x1000

static const MemMapEntry memmap[] = {
    [K230_DEV_DDRC] =         { 0x00000000,  0x80000000 },
    [K230_DEV_KPU_L2_CACHE] = { 0x80000000,  0x00200000 },
    [K230_DEV_SRAM] =         { 0x80200000,  0x00200000 },
    [K230_DEV_KPU_CFG] =      { 0x80400000,  0x00000800 },
    [K230_DEV_FFT] =          { 0x80400800,  0x00000400 },
    [K230_DEV_AI_2D_ENGINE] = { 0x80400C00,  0x00000800 },
    [K230_DEV_GSDMA] =        { 0x80800000,  0x00004000 },
    [K230_DEV_DMA] =          { 0x80804000,  0x00004000 },
    [K230_DEV_DECOMP_GZIP] =  { 0x80808000,  0x00004000 },
    [K230_DEV_NON_AI_2D] =    { 0x8080C000,  0x00004000 },
    [K230_DEV_ISP] =          { 0x90000000,  0x00008000 },
    [K230_DEV_DEWARP] =       { 0x90008000,  0x00001000 },
    [K230_DEV_RX_CSI] =       { 0x90009000,  0x00002000 },
    [K230_DEV_H264] =         { 0x90400000,  0x00010000 },
    [K230_DEV_2P5D] =         { 0x90800000,  0x00040000 },
    [K230_DEV_VO] =           { 0x90840000,  0x00010000 },
    [K230_DEV_VO_CFG] =       { 0x90850000,  0x00001000 },
    [K230_DEV_3D_ENGINE] =    { 0x90A00000,  0x00000800 },
    [K230_DEV_PMU] =          { 0x91000000,  0x00000C00 },
    [K230_DEV_RTC] =          { 0x91000C00,  0x00000400 },
    [K230_DEV_CMU] =          { 0x91100000,  0x00001000 },
    [K230_DEV_RMU] =          { 0x91101000,  0x00001000 },
    [K230_DEV_BOOT] =         { 0x91102000,  0x00001000 },
    [K230_DEV_PWR] =          { 0x91103000,  0x00001000 },
    [K230_DEV_MAILBOX] =      { 0x91104000,  0x00001000 },
    [K230_DEV_IOMUX] =        { 0x91105000,  0x00000800 },
    [K230_DEV_TIMER] =        { 0x91105800,  0x00000800 },
    [K230_DEV_WDT0] =         { 0x91106000,  0x00000800 },
    [K230_DEV_WDT1] =         { 0x91106800,  0x00000800 },
    [K230_DEV_TS] =           { 0x91107000,  0x00000800 },
    [K230_DEV_HDI] =          { 0x91107800,  0x00000800 },
    [K230_DEV_STC] =          { 0x91108000,  0x00000800 },
    [K230_DEV_BOOTROM] =      { 0x91200000,  0x00010000 },
    [K230_DEV_SECURITY] =     { 0x91210000,  0x00008000 },
    [K230_DEV_UART0] =        { 0x91400000,  0x00001000 },
    [K230_DEV_UART1] =        { 0x91401000,  0x00001000 },
    [K230_DEV_UART2] =        { 0x91402000,  0x00001000 },
    [K230_DEV_UART3] =        { 0x91403000,  0x00001000 },
    [K230_DEV_UART4] =        { 0x91404000,  0x00001000 },
    [K230_DEV_I2C0] =         { 0x91405000,  0x00001000 },
    [K230_DEV_I2C1] =         { 0x91406000,  0x00001000 },
    [K230_DEV_I2C2] =         { 0x91407000,  0x00001000 },
    [K230_DEV_I2C3] =         { 0x91408000,  0x00001000 },
    [K230_DEV_I2C4] =         { 0x91409000,  0x00001000 },
    [K230_DEV_PWM] =          { 0x9140A000,  0x00001000 },
    [K230_DEV_GPIO0] =        { 0x9140B000,  0x00001000 },
    [K230_DEV_GPIO1] =        { 0x9140C000,  0x00001000 },
    [K230_DEV_ADC] =          { 0x9140D000,  0x00001000 },
    [K230_DEV_CODEC] =        { 0x9140E000,  0x00001000 },
    [K230_DEV_I2S] =          { 0x9140F000,  0x00001000 },
    [K230_DEV_USB0] =         { 0x91500000,  0x00010000 },
    [K230_DEV_USB1] =         { 0x91540000,  0x00010000 },
    [K230_DEV_SD0] =          { 0x91580000,  0x00001000 },
    [K230_DEV_SD1] =          { 0x91581000,  0x00001000 },
    [K230_DEV_QSPI0] =        { 0x91582000,  0x00001000 },
    [K230_DEV_QSPI1] =        { 0x91583000,  0x00001000 },
    [K230_DEV_SPI] =          { 0x91584000,  0x00001000 },
    [K230_DEV_HI_SYS_CFG] =   { 0x91585000,  0x00000400 },
    [K230_DEV_DDRC_CFG] =     { 0x98000000,  0x02000000 },
    [K230_DEV_FLASH] =        { 0xC0000000,  0x08000000 },
    [K230_DEV_PLIC] =         { 0xF00000000, 0x00400000 },
    [K230_DEV_CLINT] =        { 0xF04000000, 0x00400000 },
};

static void k230_soc_init(Object *obj)
{
    K230SoCState *s = RISCV_K230_SOC(obj);
    RISCVHartArrayState *cpu0 = &s->c908_cpu;

    object_initialize_child(obj, "c908-cpu", cpu0, TYPE_RISCV_HART_ARRAY);
    object_initialize_child(obj, "k230-wdt0", &s->wdt[0], TYPE_K230_WDT);
    object_initialize_child(obj, "k230-wdt1", &s->wdt[1], TYPE_K230_WDT);
    object_initialize_child(obj, "k230-sdhci0", &s->sdhci[0],
                            TYPE_K230_SDHCI);
    object_initialize_child(obj, "k230-sdhci1", &s->sdhci[1],
                            TYPE_K230_SDHCI);
    object_initialize_child(obj, "k230-gsdma", &s->gsdma, TYPE_K230_GSDMA);
    object_initialize_child(obj, "k230-pdma", &s->pdma, TYPE_K230_PDMA);
    object_initialize_child(obj, "k230-ugzip", &s->ugzip, TYPE_K230_UGZIP);
    object_initialize_child(obj, "k230-hi-sys-cfg", &s->hi_sys_cfg,
                            TYPE_K230_HI_SYS_CFG);
    object_initialize_child(obj, "k230-hardlock", &s->hardlock,
                            TYPE_K230_HARDLOCK);
    object_initialize_child(obj, "k230-tsensor", &s->tsensor,
                            TYPE_K230_TSENSOR);
    object_initialize_child(obj, "k230-gpio0", &s->gpio[0], TYPE_K230_GPIO);
    object_initialize_child(obj, "k230-gpio1", &s->gpio[1], TYPE_K230_GPIO);
    object_initialize_child(obj, "k230-iomux", &s->iomux, TYPE_K230_IOMUX);
    for (int i = 0; i < K230_I2C_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("k230-i2c%d", i);

        object_initialize_child(obj, name, &s->i2c[i], TYPE_K230_I2C);
    }
    object_initialize_child(obj, "k230-adc", &s->adc, TYPE_K230_ADC);
    object_initialize_child(obj, "k230-pwm", &s->pwm, TYPE_K230_PWM);
    object_initialize_child(obj, "k230-timer", &s->timer, TYPE_K230_TIMER);
    object_initialize_child(obj, "k230-sysctl-boot", &s->sysctl_boot,
                            TYPE_K230_SYSCTL_BOOT);
    object_initialize_child(obj, "k230-sysctl-power", &s->sysctl_power,
                            TYPE_K230_SYSCTL_POWER);
    object_initialize_child(obj, "k230-rtc", &s->rtc, TYPE_K230_RTC);
    object_initialize_child(obj, "k230-security", &s->security,
                            TYPE_K230_SECURITY);
    for (int i = 0; i < K230_SPI_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("k230-spi%d", i);

        object_initialize_child(obj, name, &s->spi[i], TYPE_K230_SPI);
    }
    for (int i = 0; i < K230_REGS_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("k230-regs%d", i);

        object_initialize_child(obj, name, &s->regs[i], TYPE_K230_REGS);
    }
    object_initialize_child(obj, "k230-usb0", &s->usb[0], TYPE_DWC2_USB);
    object_initialize_child(obj, "k230-usb1", &s->usb[1], TYPE_DWC2_USB);
    object_property_add_const_link(OBJECT(&s->usb[0]), "dma-mr",
                                   OBJECT(get_system_memory()));
    object_property_add_const_link(OBJECT(&s->usb[1]), "dma-mr",
                                   OBJECT(get_system_memory()));
    s->ugzip.gsdma = &s->gsdma;

    qdev_prop_set_uint32(DEVICE(cpu0), "hartid-base", 0);
    qdev_prop_set_string(DEVICE(cpu0), "cpu-type", TYPE_RISCV_CPU_THEAD_C908);
    qdev_prop_set_uint64(DEVICE(cpu0), "resetvec",
                         memmap[K230_DEV_BOOTROM].base);
}

static DeviceState *k230_create_plic(int base_hartid, int hartid_count)
{
    g_autofree char *plic_hart_config = NULL;

    /* Per-socket PLIC hart topology configuration string */
    plic_hart_config = riscv_plic_hart_config_string(hartid_count);

    /* Per-socket PLIC */
    return sifive_plic_create(memmap[K230_DEV_PLIC].base,
                              plic_hart_config, hartid_count, base_hartid,
                              K230_PLIC_NUM_SOURCES,
                              K230_PLIC_NUM_PRIORITIES,
                              K230_PLIC_PRIORITY_BASE, K230_PLIC_PENDING_BASE,
                              K230_PLIC_ENABLE_BASE, K230_PLIC_ENABLE_STRIDE,
                              K230_PLIC_CONTEXT_BASE,
                              K230_PLIC_CONTEXT_STRIDE,
                              memmap[K230_DEV_PLIC].size);
}

static void k230_create_uart(MemoryRegion *sys_mem, DeviceState *plic,
                             int index)
{
    int uart_dev = K230_DEV_UART0 + index;
    g_autofree char *name = g_strdup_printf("uart%d", index);

    /* Cover the non-16550 part of the SDK's 0x1000 UART window. */
    create_unimplemented_device(name, memmap[uart_dev].base,
                                memmap[uart_dev].size);

    serial_mm_init(sys_mem, memmap[uart_dev].base, 2,
                   qdev_get_gpio_in(plic, K230_UART0_IRQ + index),
                   399193, serial_hd(index), DEVICE_LITTLE_ENDIAN);
}

static void k230_create_sdhci(K230SoCState *s, int index, int irq)
{
    int sd_dev = K230_DEV_SD0 + index;
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->sdhci[index]);

    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, memmap[sd_dev].base);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(s->c908_plic), irq));
}

static void k230_create_usb(K230SoCState *s, int index, int irq)
{
    int usb_dev = K230_DEV_USB0 + index;
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->usb[index]);

    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, memmap[usb_dev].base);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(DEVICE(s->c908_plic), irq));
}

static void k230_create_usb_nic(K230SoCState *s)
{
    DeviceState *dev = qemu_create_nic_device("usb-rtl8152", true,
                                              "r8152_eth");

    if (!dev) {
        return;
    }

    qdev_prop_set_string(dev, "port", "1");
    usb_realize_and_unref(USB_DEVICE(dev), &s->usb[1].bus, &error_fatal);
}

static void k230_create_i2c(K230SoCState *s, int index)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->i2c[index]);
    int i2c_dev = K230_DEV_I2C0 + index;

    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, memmap[i2c_dev].base);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(DEVICE(s->c908_plic),
                                        K230_I2C0_IRQ + index));
}

static bool k230_create_spi(K230SoCState *s, int index,
                            int spi_dev, int irq_base, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->spi[index]);

    if (!sysbus_realize(sbd, errp)) {
        return false;
    }

    sysbus_mmio_map(sbd, 0, memmap[spi_dev].base);
    for (int i = 0; i < K230_SPI_IRQ_COUNT; i++) {
        sysbus_connect_irq(sbd, i,
                           qdev_get_gpio_in(DEVICE(s->c908_plic),
                                            irq_base + i));
    }

    return true;
}

static bool k230_create_regs(K230SoCState *s, int index,
                             hwaddr base, hwaddr size, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->regs[index]);

    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "size", size);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }

    sysbus_mmio_map(sbd, 0, base);
    return true;
}

static void k230_create_flash_xip(K230SoCState *s, DeviceState *dev,
                                  MemoryRegion *sys_mem)
{
    hwaddr size = memmap[K230_DEV_FLASH].size;
    K230SpiState *spi = &s->spi[K230_SPI_SPI0];
    uint8_t *storage;

    memory_region_init_rom(&s->flash_xip, OBJECT(dev), "k230.flash-xip",
                           size, &error_fatal);
    storage = memory_region_get_ram_ptr(&s->flash_xip);
    memset(storage, 0xff, size);

    if (spi->blk) {
        int64_t length = blk_getlength(spi->blk);

        if (length > 0) {
            int64_t read_len = MIN(length, (int64_t)size);

            if (blk_pread(spi->blk, 0, read_len, storage, 0) < 0) {
                error_report("failed to read K230 SPI flash image");
            }
        }
    }

    memory_region_add_subregion(sys_mem, memmap[K230_DEV_FLASH].base,
                                &s->flash_xip);
}

static void k230_soc_realize(DeviceState *dev, Error **errp)
{
    K230SoCState *s = RISCV_K230_SOC(dev);
    MemoryRegion *sys_mem = get_system_memory();
    static const int sd_irqs[] = { K230_SD0_IRQ, K230_SD1_IRQ };
    static const int usb_irqs[] = { K230_USB0_IRQ, K230_USB1_IRQ };
    int c908_cpus;

    sysbus_realize(SYS_BUS_DEVICE(&s->c908_cpu), &error_fatal);

    c908_cpus = s->c908_cpu.num_harts;

    /* SRAM */
    memory_region_init_ram(&s->sram, OBJECT(dev), "sram",
                           memmap[K230_DEV_SRAM].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[K230_DEV_SRAM].base,
                                &s->sram);

    /* BootROM */
    memory_region_init_rom(&s->bootrom, OBJECT(dev), "bootrom",
                           memmap[K230_DEV_BOOTROM].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[K230_DEV_BOOTROM].base,
                                &s->bootrom);

    /* PLIC */
    s->c908_plic = k230_create_plic(C908_CPU_HARTID, c908_cpus);

    /* CLINT */
    riscv_aclint_swi_create(memmap[K230_DEV_CLINT].base,
                            C908_CPU_HARTID, c908_cpus, false);
    riscv_aclint_mtimer_create(memmap[K230_DEV_CLINT].base + 0x4000,
                               RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
                               C908_CPU_HARTID, c908_cpus,
                               RISCV_ACLINT_DEFAULT_MTIMECMP,
                               RISCV_ACLINT_DEFAULT_MTIME,
                               RISCV_ACLINT_DEFAULT_TIMEBASE_FREQ, true);

    /* UART */
    for (int i = 0; i < K230_UART_COUNT; i++) {
        k230_create_uart(sys_mem, DEVICE(s->c908_plic), i);
    }

    /* Watchdog */
    for (int i = 0; i < 2; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->wdt[i]), errp)) {
            return;
        }
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->wdt[0]), 0, memmap[K230_DEV_WDT0].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->wdt[0]), 0,
                       qdev_get_gpio_in(DEVICE(s->c908_plic), K230_WDT0_IRQ));

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->wdt[1]), 0, memmap[K230_DEV_WDT1].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->wdt[1]), 0,
                       qdev_get_gpio_in(DEVICE(s->c908_plic), K230_WDT1_IRQ));

    /* GSDMA/PDMA/UGZIP blocks used by SDK U-Boot and Linux DT. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gsdma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gsdma), 0,
                    memmap[K230_DEV_GSDMA].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pdma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pdma), 0, memmap[K230_DEV_DMA].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ugzip), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ugzip), 0,
                    memmap[K230_DEV_DECOMP_GZIP].base);

    /* SDHCI */
    for (int i = 0; i < K230_SDHCI_COUNT; i++) {
        k230_create_sdhci(s, i, sd_irqs[i]);
    }

    for (int i = 0; i < 2; i++) {
        k230_create_usb(s, i, usb_irqs[i]);
    }
    k230_create_usb_nic(s);

    /* High-speed system config bits used by the SDK SDHCI driver. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->hi_sys_cfg), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->hi_sys_cfg), 0,
                    memmap[K230_DEV_HI_SYS_CFG].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->hardlock), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->hardlock), 0,
                    memmap[K230_DEV_MAILBOX].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->tsensor), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->tsensor), 0, memmap[K230_DEV_TS].base);

    for (int i = 0; i < 2; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio[i]), errp)) {
            return;
        }
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio[0]), 0,
                    memmap[K230_DEV_GPIO0].base);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio[1]), 0,
                    memmap[K230_DEV_GPIO1].base);
    for (int bank = 0; bank < 2; bank++) {
        for (int line = 0; line < K230_GPIO_IRQ_COUNT; line++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpio[bank]), line,
                qdev_get_gpio_in(DEVICE(s->c908_plic),
                                 K230_GPIO0_IRQ +
                                 bank * K230_GPIO_IRQ_COUNT + line));
        }
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->iomux), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->iomux), 0,
                    memmap[K230_DEV_IOMUX].base);

    for (int i = 0; i < K230_I2C_COUNT; i++) {
        k230_create_i2c(s, i);
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->adc), 0, memmap[K230_DEV_ADC].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pwm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pwm), 0, memmap[K230_DEV_PWM].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->timer), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->timer), 0,
                    memmap[K230_DEV_TIMER].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sysctl_boot), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysctl_boot), 0,
                    memmap[K230_DEV_BOOT].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sysctl_power), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysctl_power), 0,
                    memmap[K230_DEV_PWR].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rtc), 0, memmap[K230_DEV_RTC].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 0,
                       qdev_get_gpio_in(DEVICE(s->c908_plic),
                                        K230_PMU_IRQ));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->security), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->security), 0,
                    memmap[K230_DEV_SECURITY].base);

    if (!k230_create_regs(s, K230_REGS_PMU, memmap[K230_DEV_PMU].base,
                          memmap[K230_DEV_PMU].size, errp)) {
        return;
    }
    if (!k230_create_regs(s, K230_REGS_CMU, memmap[K230_DEV_CMU].base,
                          memmap[K230_DEV_CMU].size, errp)) {
        return;
    }
    if (!k230_create_regs(s, K230_REGS_RMU, memmap[K230_DEV_RMU].base,
                          memmap[K230_DEV_RMU].size, errp)) {
        return;
    }
    if (!k230_create_regs(s, K230_REGS_HDI, memmap[K230_DEV_HDI].base,
                          memmap[K230_DEV_HDI].size, errp)) {
        return;
    }
    if (!k230_create_regs(s, K230_REGS_STC, memmap[K230_DEV_STC].base,
                          memmap[K230_DEV_STC].size, errp)) {
        return;
    }
    if (!k230_create_regs(s, K230_REGS_NOC_QOS, K230_NOC_QOS_BASE,
                          K230_NOC_QOS_SIZE, errp)) {
        return;
    }
    if (!k230_create_regs(s, K230_REGS_CODEC, memmap[K230_DEV_CODEC].base,
                          memmap[K230_DEV_CODEC].size, errp)) {
        return;
    }
    if (!k230_create_regs(s, K230_REGS_I2S, memmap[K230_DEV_I2S].base,
                          memmap[K230_DEV_I2S].size, errp)) {
        return;
    }
    if (!k230_create_regs(s, K230_REGS_DDRC_CFG,
                          memmap[K230_DEV_DDRC_CFG].base,
                          memmap[K230_DEV_DDRC_CFG].size, errp)) {
        return;
    }

    if (!k230_create_spi(s, K230_SPI_QSPI0, K230_DEV_QSPI0,
                         K230_QSPI0_IRQ, errp)) {
        return;
    }
    if (!k230_create_spi(s, K230_SPI_QSPI1, K230_DEV_QSPI1,
                         K230_QSPI1_IRQ, errp)) {
        return;
    }
    if (!k230_create_spi(s, K230_SPI_SPI0, K230_DEV_SPI, K230_SPI_IRQ,
                         errp)) {
        return;
    }

    k230_create_flash_xip(s, dev, sys_mem);

    /* unimplemented devices */
    create_unimplemented_device("kpu.l2-cache",
                                memmap[K230_DEV_KPU_L2_CACHE].base,
                                memmap[K230_DEV_KPU_L2_CACHE].size);

    create_unimplemented_device("kpu_cfg", memmap[K230_DEV_KPU_CFG].base,
                                memmap[K230_DEV_KPU_CFG].size);

    create_unimplemented_device("fft", memmap[K230_DEV_FFT].base,
                                memmap[K230_DEV_FFT].size);

    create_unimplemented_device("2d-engine.ai",
                                memmap[K230_DEV_AI_2D_ENGINE].base,
                                memmap[K230_DEV_AI_2D_ENGINE].size);

    create_unimplemented_device("2d-engine.non-ai",
                                memmap[K230_DEV_NON_AI_2D].base,
                                memmap[K230_DEV_NON_AI_2D].size);

    create_unimplemented_device("isp", memmap[K230_DEV_ISP].base,
                                memmap[K230_DEV_ISP].size);

    create_unimplemented_device("dewarp", memmap[K230_DEV_DEWARP].base,
                                memmap[K230_DEV_DEWARP].size);

    create_unimplemented_device("rx-csi", memmap[K230_DEV_RX_CSI].base,
                                memmap[K230_DEV_RX_CSI].size);

    create_unimplemented_device("vpu", memmap[K230_DEV_H264].base,
                                memmap[K230_DEV_H264].size);

    create_unimplemented_device("gpu", memmap[K230_DEV_2P5D].base,
                                memmap[K230_DEV_2P5D].size);

    create_unimplemented_device("vo", memmap[K230_DEV_VO].base,
                                memmap[K230_DEV_VO].size);

    create_unimplemented_device("vo_cfg", memmap[K230_DEV_VO_CFG].base,
                                memmap[K230_DEV_VO_CFG].size);

    create_unimplemented_device("3d-engine", memmap[K230_DEV_3D_ENGINE].base,
                                memmap[K230_DEV_3D_ENGINE].size);

}

static void k230_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_soc_realize;
}

static const TypeInfo k230_soc_type_info = {
    .name = TYPE_RISCV_K230_SOC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(K230SoCState),
    .instance_init = k230_soc_init,
    .class_init = k230_soc_class_init,
};

static void k230_soc_register_types(void)
{
    type_register_static(&k230_soc_type_info);
}

type_init(k230_soc_register_types)

static void k230_direct_boot(K230MachineState *s, MachineState *machine)
{
    const char *firmware_name = riscv_default_firmware_name(&s->soc.c908_cpu);
    RISCVBootInfo boot_info = {0};
    hwaddr start_addr = K230_DIRECT_OPENSBI_ADDR;
    hwaddr firmware_end_addr = 0;
    hwaddr kernel_entry = 0;
    int fdt_size = 0;

    if (machine->firmware && !strcmp(machine->firmware, "none")) {
        error_report("K230 direct boot requires OpenSBI firmware; omit "
                     "-bios none or pass OpenSBI with -bios");
        exit(EXIT_FAILURE);
    }

    if (!machine->dtb) {
        error_report("K230 direct boot requires -dtb");
        exit(EXIT_FAILURE);
    }

    machine->fdt = load_device_tree(machine->dtb, &fdt_size);
    if (!machine->fdt) {
        error_report("load_device_tree() failed");
        exit(EXIT_FAILURE);
    }

    qemu_fdt_add_path(machine->fdt, "/chosen");

    riscv_boot_info_init(&boot_info, &s->soc.c908_cpu);
    riscv_load_kernel(machine, &boot_info, K230_DIRECT_KERNEL_ADDR, true, NULL);
    kernel_entry = boot_info.image_low_addr;

    riscv_load_fdt(K230_DIRECT_DTB_ADDR, machine->fdt);

    firmware_end_addr = riscv_find_and_load_firmware(machine, &boot_info,
                                                     firmware_name,
                                                     &start_addr, NULL);
    if (firmware_end_addr > K230_DIRECT_KERNEL_ADDR) {
        error_report("K230 firmware overlaps kernel address 0x%x",
                     K230_DIRECT_KERNEL_ADDR);
        exit(EXIT_FAILURE);
    }

    riscv_setup_rom_reset_vec(machine, &s->soc.c908_cpu, start_addr,
                              memmap[K230_DEV_BOOTROM].base,
                              memmap[K230_DEV_BOOTROM].size, kernel_entry,
                              K230_DIRECT_DTB_ADDR);
}

static void k230_firmware_boot(K230MachineState *s, MachineState *machine)
{
    const char *firmware_name = riscv_default_firmware_name(&s->soc.c908_cpu);
    hwaddr start_addr = memmap[K230_DEV_DDRC].base;
    RISCVBootInfo boot_info = {0};

    if (machine->dtb || (machine->kernel_cmdline && *machine->kernel_cmdline)) {
        error_report("K230 firmware boot does not support -dtb or -append");
        exit(EXIT_FAILURE);
    }

    riscv_boot_info_init(&boot_info, &s->soc.c908_cpu);
    riscv_find_and_load_firmware(machine, &boot_info, firmware_name,
                                 &start_addr, NULL);

    riscv_setup_rom_reset_vec(machine, &s->soc.c908_cpu, start_addr,
                              memmap[K230_DEV_BOOTROM].base,
                              memmap[K230_DEV_BOOTROM].size, 0, 0);
}

static void k230_machine_done(Notifier *notifier, void *data)
{
    K230MachineState *s = container_of(notifier, K230MachineState,
                                       machine_done);
    MachineState *machine = MACHINE(s);

    if (machine->kernel_filename) {
        k230_direct_boot(s, machine);
    } else {
        k230_firmware_boot(s, machine);
    }
}

static void k230_attach_sd_drive(K230MachineState *s, int sd_index,
                                 int drive_unit)
{
    DriveInfo *dinfo = drive_get(IF_SD, 0, drive_unit);
    DeviceState *card;

    if (!dinfo) {
        return;
    }

    card = qdev_new(TYPE_SD_CARD);
    qdev_prop_set_drive_err(card, "drive", blk_by_legacy_dinfo(dinfo),
                            &error_fatal);
    qdev_realize_and_unref(card, s->soc.sdhci[sd_index].sd_bus, &error_fatal);
}

static void k230_attach_sd_drives(K230MachineState *s)
{
    /*
     * The SDK's CANMV DTB uses SD1 as the removable card slot; keep the first
     * legacy SD drive there so "-drive if=sd" and "-sd" boot the SDK image.
     */
    k230_attach_sd_drive(s, 1, 0);
    k230_attach_sd_drive(s, 0, 1);
}

static void k230_attach_spi_flash(K230MachineState *s)
{
    DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);

    if (!dinfo) {
        return;
    }

    qdev_prop_set_drive_err(DEVICE(&s->soc.spi[K230_SPI_SPI0]), "drive",
                            blk_by_legacy_dinfo(dinfo), &error_fatal);
}

static void k230_machine_init(MachineState *machine)
{
    MachineClass *mc = MACHINE_GET_CLASS(machine);
    K230MachineState *s = RISCV_K230_MACHINE(machine);
    MemoryRegion *sys_mem = get_system_memory();

    if (machine->ram_size < mc->default_ram_size) {
        char *sz = size_to_str(mc->default_ram_size);
        error_report("Invalid RAM size, should be %s", sz);
        g_free(sz);
        exit(EXIT_FAILURE);
    }

    /* Initialize SoC */
    object_initialize_child(OBJECT(machine), "soc", &s->soc,
                            TYPE_RISCV_K230_SOC);
    k230_attach_spi_flash(s);
    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    /* Data Memory */
    memory_region_add_subregion(sys_mem, memmap[K230_DEV_DDRC].base,
                                machine->ram);

    k230_attach_sd_drives(s);

    s->machine_done.notify = k230_machine_done;
    qemu_add_machine_init_done_notifier(&s->machine_done);
}

static void k230_machine_instance_init(Object *obj)
{
}

static void k230_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V Board compatible with Kendryte K230 SDK";
    mc->init = k230_machine_init;
    mc->default_cpus = 1;
    mc->default_ram_id = "riscv.K230.ram"; /* DDR */
    mc->default_ram_size = memmap[K230_DEV_DDRC].size;
    mc->default_nic = "usb-rtl8152";
    mc->auto_create_sdcard = true;
}

static const TypeInfo k230_machine_typeinfo = {
    .name       = TYPE_RISCV_K230_MACHINE,
    .parent     = TYPE_MACHINE,
    .class_init = k230_machine_class_init,
    .instance_init = k230_machine_instance_init,
    .instance_size = sizeof(K230MachineState),
    .interfaces = riscv64_machine_interfaces,
};

static void k230_machine_init_register_types(void)
{
    type_register_static(&k230_machine_typeinfo);
}

type_init(k230_machine_init_register_types)
