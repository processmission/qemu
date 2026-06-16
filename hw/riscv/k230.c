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
#include "qemu/bitops.h"
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
#include "hw/core/irq.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/k230_ov5647.h"
#include "hw/riscv/k230.h"
#include "hw/riscv/boot.h"
#include "hw/riscv/machines-qom.h"
#include "hw/intc/k230_clint.h"
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

#define K230_FIRMWARE_OPENSBI_ADDR 0x00200000
#define K230_CLINT_SMODE_OFFSET    0x0000c000
#define K230_TIMEBASE_FREQ         27000000
#define K230_STC_COUNTER_OFFSET    0x40
#define K230_STC_COUNTER_SIZE      0x30
#define K230_AI2D_CALC_ENABLE      0x80
#define K230_AI2D_JOB_OFFSET       0x8c
#define K230_AI2D_CLEAR_OFFSET     0xa0
#define K230_AI2D_LEGACY_START     0xc0

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
    [K230_DEV_RX_CSI] =       { 0x90009000,  0x00010000 },
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
    memory_region_init(&s->c908v_mem, obj, "k230.c908v-memory", UINT64_MAX);
    memory_region_init_alias(&s->c908v_sysmem, obj, "k230.c908v-system",
                             get_system_memory(), 0, UINT64_MAX);
    memory_region_add_subregion(&s->c908v_mem, 0, &s->c908v_sysmem);
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
    for (int i = 0; i < K230_UART_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("k230-uart-ext%d", i);

        object_initialize_child(obj, name, &s->uart_ext[i], TYPE_K230_REGS);
    }
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
    object_initialize_child(obj, "k230-sysctl-reset", &s->sysctl_reset,
                            TYPE_K230_SYSCTL_RESET);
    object_initialize_child(obj, "k230-pmu", &s->pmu, TYPE_K230_PMU);
    object_initialize_child(obj, "k230-rtc", &s->rtc, TYPE_K230_RTC);
    object_initialize_child(obj, "k230-security", &s->security,
                            TYPE_K230_SECURITY);
    object_initialize_child(obj, "k230-vo", &s->vo, TYPE_K230_VO);
    object_initialize_child(obj, "k230-dsi", &s->dsi, TYPE_K230_DSI);
    for (int i = 0; i < K230_SPI_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("k230-spi%d", i);

        object_initialize_child(obj, name, &s->spi[i], TYPE_K230_SPI);
    }
    object_initialize_child(obj, "k230-i2s", &s->i2s, TYPE_K230_I2S);
    for (int i = 0; i < K230_REGS_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("k230-regs%d", i);

        object_initialize_child(obj, name, &s->regs[i], TYPE_K230_REGS);
    }
    object_initialize_child(obj, "k230-kpu", &s->kpu, TYPE_K230_KPU);
    object_initialize_child(obj, "k230-nonai-2d", &s->nonai_2d,
                            TYPE_K230_NONAI_2D);
    object_initialize_child(obj, "k230-isp", &s->isp, TYPE_K230_ISP);
    object_initialize_child(obj, "k230-dewarp", &s->dewarp,
                            TYPE_K230_DEWARP);
    object_initialize_child(obj, "k230-rx-csi", &s->rx_csi,
                            TYPE_K230_RX_CSI);
    for (int i = 0; i < K230_PLIC_NUM_SOURCES; i++) {
        g_autofree char *name = g_strdup_printf("k230-plic-irq-splitter%d",
                                                i);

        object_initialize_child(obj, name, &s->plic_irq_splitter[i],
                                TYPE_SPLIT_IRQ);
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

static void k230_soc_init_c908v_cpu(K230SoCState *s, Object *obj)
{
    RISCVHartArrayState *cpu1 = &s->c908v_cpu;

    object_initialize_child(obj, "c908v-cpu", cpu1, TYPE_RISCV_HART_ARRAY);
    qdev_prop_set_uint32(DEVICE(cpu1), "hartid-base", C908V_CPU_HARTID);
    qdev_prop_set_string(DEVICE(cpu1), "cpu-type",
                         TYPE_RISCV_CPU_THEAD_C908V);
    qdev_prop_set_uint64(DEVICE(cpu1), "resetvec",
                         memmap[K230_DEV_BOOTROM].base);
    object_property_set_link(OBJECT(cpu1), "memory", OBJECT(&s->c908v_mem),
                             &error_abort);
    qdev_prop_set_bit(DEVICE(cpu1), "start-powered-off", true);
}

static RISCVHartArrayState *k230_boot_harts(K230MachineState *s)
{
    if (s->boot_both_cores) {
        return &s->soc.c908_cpu;
    }

    return s->soc.c908v_enabled ? &s->soc.c908v_cpu : &s->soc.c908_cpu;
}

static DeviceState *k230_create_plic_in(MemoryRegion *mem, int base_hartid,
                                        int cpu_index_base, int hartid_count)
{
    g_autofree char *plic_hart_config = NULL;

    /* Per-socket PLIC hart topology configuration string */
    plic_hart_config = riscv_plic_hart_config_string(hartid_count);

    /* Per-socket PLIC */
    return sifive_plic_create_in(mem, memmap[K230_DEV_PLIC].base,
                                 plic_hart_config, hartid_count, base_hartid,
                                 cpu_index_base, K230_PLIC_NUM_SOURCES,
                                 K230_PLIC_NUM_PRIORITIES,
                                 K230_PLIC_PRIORITY_BASE,
                                 K230_PLIC_PENDING_BASE,
                                 K230_PLIC_ENABLE_BASE,
                                 K230_PLIC_ENABLE_STRIDE,
                                 K230_PLIC_CONTEXT_BASE,
                                 K230_PLIC_CONTEXT_STRIDE,
                                 memmap[K230_DEV_PLIC].size);
}

static DeviceState *k230_create_plic(int base_hartid, int hartid_count)
{
    return k230_create_plic_in(get_system_memory(), base_hartid, base_hartid,
                               hartid_count);
}

static qemu_irq k230_plic_irq(K230SoCState *s, int irq)
{
    return qdev_get_gpio_in(DEVICE(&s->plic_irq_splitter[irq]), 0);
}

static void k230_create_plic_irq_splitters(K230SoCState *s)
{
    uint16_t num_lines = s->c908v_enabled ? 2 : 1;

    for (int i = 0; i < K230_PLIC_NUM_SOURCES; i++) {
        DeviceState *splitter = DEVICE(&s->plic_irq_splitter[i]);

        qdev_prop_set_uint16(splitter, "num-lines", num_lines);
        qdev_realize(splitter, NULL, &error_fatal);
        qdev_connect_gpio_out(splitter, 0,
                              qdev_get_gpio_in(DEVICE(s->c908_plic), i));
        if (s->c908v_enabled) {
            qdev_connect_gpio_out(splitter, 1,
                                  qdev_get_gpio_in(DEVICE(s->c908v_plic), i));
        }
    }
}

static void k230_create_uart(MemoryRegion *sys_mem, K230SoCState *s, int index)
{
    int uart_dev = K230_DEV_UART0 + index;
    SysBusDevice *uart_ext = SYS_BUS_DEVICE(&s->uart_ext[index]);

    /* Cover the non-16550 part of the SDK's 0x1000 UART window. */
    qdev_prop_set_uint64(DEVICE(&s->uart_ext[index]), "size",
                         memmap[uart_dev].size - 0x20);
    sysbus_realize(uart_ext, &error_fatal);
    sysbus_mmio_map(uart_ext, 0, memmap[uart_dev].base + 0x20);

    serial_mm_init(sys_mem, memmap[uart_dev].base, 2,
                   k230_plic_irq(s, K230_UART0_IRQ + index), 399193,
                   serial_hd(index), DEVICE_LITTLE_ENDIAN);
}

static void k230_create_sdhci(K230SoCState *s, int index, int irq)
{
    int sd_dev = K230_DEV_SD0 + index;
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->sdhci[index]);

    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, memmap[sd_dev].base);
    sysbus_connect_irq(sbd, 0, k230_plic_irq(s, irq));
}

static void k230_create_usb(K230SoCState *s, int index, int irq)
{
    int usb_dev = K230_DEV_USB0 + index;
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->usb[index]);

    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, memmap[usb_dev].base);
    sysbus_connect_irq(sbd, 0, k230_plic_irq(s, irq));
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
    sysbus_connect_irq(sbd, 0, k230_plic_irq(s, K230_I2C0_IRQ + index));
}

static void k230_create_camera_i2c_devices(K230SoCState *s)
{
    static const int ov5647_bus_ids[] = { 0, 1, 3, 4 };

    for (int i = 0; i < ARRAY_SIZE(ov5647_bus_ids); i++) {
        int index = ov5647_bus_ids[i];

        if (index < K230_I2C_COUNT) {
            i2c_slave_create_simple(s->i2c[index].bus, TYPE_K230_OV5647, 0x36);
        }
    }
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
        sysbus_connect_irq(sbd, i, k230_plic_irq(s, irq_base + i));
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

static bool k230_create_irq_regs(K230SoCState *s, int index,
                                 hwaddr base, hwaddr size, int irq,
                                 hwaddr start_offset, hwaddr start2_offset,
                                 hwaddr start3_offset, hwaddr clear_offset,
                                 hwaddr status_offset,
                                 uint64_t status_value,
                                 uint64_t delay_ns,
                                 hwaddr command_start_offset,
                                 hwaddr command_end_offset,
                                 hwaddr command_hi_offset,
                                 bool irq_clear_clears_status,
                                 bool irq_on_any_write, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->regs[index]);

    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "size", size);
    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "irq-start-offset",
                         start_offset);
    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "irq-start2-offset",
                         start2_offset);
    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "irq-start3-offset",
                         start3_offset);
    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "irq-clear-offset",
                         clear_offset);
    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "irq-status-offset",
                         status_offset);
    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "irq-status-size",
                         sizeof(status_value));
    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "irq-status-value",
                         status_value);
    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "irq-delay-ns", delay_ns);
    qdev_prop_set_uint64(DEVICE(&s->regs[index]),
                         "irq-command-start-offset", command_start_offset);
    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "irq-command-end-offset",
                         command_end_offset);
    qdev_prop_set_uint64(DEVICE(&s->regs[index]), "irq-command-hi-offset",
                         command_hi_offset);
    qdev_prop_set_bit(DEVICE(&s->regs[index]), "irq-clear-clears-status",
                      irq_clear_clears_status);
    qdev_prop_set_bit(DEVICE(&s->regs[index]), "irq-on-any-write",
                      irq_on_any_write);
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }

    sysbus_mmio_map(sbd, 0, base);
    sysbus_connect_irq(sbd, 0, k230_plic_irq(s, irq));
    return true;
}

static bool k230_create_stc(K230SoCState *s, hwaddr base, hwaddr size,
                            Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->regs[K230_REGS_STC]);

    qdev_prop_set_uint64(DEVICE(&s->regs[K230_REGS_STC]), "size", size);
    qdev_prop_set_uint64(DEVICE(&s->regs[K230_REGS_STC]), "counter-offset",
                         K230_STC_COUNTER_OFFSET);
    qdev_prop_set_uint64(DEVICE(&s->regs[K230_REGS_STC]), "counter-size",
                         K230_STC_COUNTER_SIZE);
    qdev_prop_set_uint64(DEVICE(&s->regs[K230_REGS_STC]),
                         "counter-frequency", K230_TIMEBASE_FREQ);
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

    /*
     * The RT-Smart fastboot image uses the XIP window as a mutable flash
     * staging area during early startup.  Back it with RAM so direct stores
     * complete while keeping erased flash contents at 0xff by default.
     */
    memory_region_init_ram(&s->flash_xip, OBJECT(dev), "k230.flash-xip",
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
    MachineState *machine = MACHINE(qdev_get_machine());
    K230MachineState *k230_machine = RISCV_K230_MACHINE(machine);
    MemoryRegion *sys_mem = get_system_memory();
    static const int sd_irqs[] = { K230_SD0_IRQ, K230_SD1_IRQ };
    static const int usb_irqs[] = { K230_USB0_IRQ, K230_USB1_IRQ };
    int c908_cpus;
    int c908v_cpus = 0;

    s->c908v_enabled = machine->smp.cpus > 1;
    qdev_prop_set_bit(DEVICE(&s->c908_cpu), "start-powered-off",
                      s->c908v_enabled && !k230_machine->boot_both_cores);

    sysbus_realize(SYS_BUS_DEVICE(&s->c908_cpu), &error_fatal);

    c908_cpus = s->c908_cpu.num_harts;
    if (s->c908v_enabled) {
        k230_soc_init_c908v_cpu(s, OBJECT(dev));
        qdev_prop_set_bit(DEVICE(&s->c908v_cpu), "start-powered-off",
                          false);
        sysbus_realize(SYS_BUS_DEVICE(&s->c908v_cpu), &error_fatal);
        c908v_cpus = s->c908v_cpu.num_harts;
    }

    /* SRAM */
    memory_region_init_ram(&s->sram, OBJECT(dev), "sram",
                           memmap[K230_DEV_SRAM].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[K230_DEV_SRAM].base,
                                &s->sram);

    /* KPU L2 cache is directly addressable by the SDK AI runtime. */
    memory_region_init_ram(&s->kpu_l2_cache, OBJECT(dev), "k230.kpu-l2-cache",
                           memmap[K230_DEV_KPU_L2_CACHE].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[K230_DEV_KPU_L2_CACHE].base,
                                &s->kpu_l2_cache);

    /* BootROM */
    memory_region_init_rom(&s->bootrom, OBJECT(dev), "bootrom",
                           memmap[K230_DEV_BOOTROM].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[K230_DEV_BOOTROM].base,
                                &s->bootrom);

    /* PLIC */
    s->c908_plic = k230_create_plic(C908_CPU_HARTID, c908_cpus);
    if (s->c908v_enabled) {
        s->c908v_plic = k230_create_plic_in(&s->c908v_mem,
                                            C908V_CPU_HARTID,
                                            C908V_CPU_INDEX, c908v_cpus);
    }
    k230_create_plic_irq_splitters(s);

    /* CLINT */
    riscv_aclint_swi_create(memmap[K230_DEV_CLINT].base,
                            C908_CPU_HARTID, c908_cpus, false);
    riscv_aclint_mtimer_create(memmap[K230_DEV_CLINT].base + 0x4000,
                               RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
                               C908_CPU_HARTID, c908_cpus,
                               RISCV_ACLINT_DEFAULT_MTIMECMP,
                               RISCV_ACLINT_DEFAULT_MTIME,
                               K230_TIMEBASE_FREQ, true);
    k230_clint_smode_create_in(sys_mem,
                               memmap[K230_DEV_CLINT].base +
                               K230_CLINT_SMODE_OFFSET,
                               C908_CPU_HARTID, C908_CPU_HARTID, c908_cpus);
    if (s->c908v_enabled) {
        riscv_aclint_swi_create_in(&s->c908v_mem,
                                   memmap[K230_DEV_CLINT].base,
                                   C908V_CPU_HARTID, C908V_CPU_INDEX,
                                   c908v_cpus, false);
        riscv_aclint_mtimer_create_in(&s->c908v_mem,
                                      memmap[K230_DEV_CLINT].base + 0x4000,
                                      RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
                                      C908V_CPU_HARTID, C908V_CPU_INDEX,
                                      c908v_cpus,
                                      RISCV_ACLINT_DEFAULT_MTIMECMP,
                                      RISCV_ACLINT_DEFAULT_MTIME,
                                      K230_TIMEBASE_FREQ, true);
        k230_clint_smode_create_in(&s->c908v_mem,
                                   memmap[K230_DEV_CLINT].base +
                                   K230_CLINT_SMODE_OFFSET,
                                   C908V_CPU_HARTID, C908V_CPU_INDEX,
                                   c908v_cpus);
    }

    /* UART */
    for (int i = 0; i < K230_UART_COUNT; i++) {
        k230_create_uart(sys_mem, s, i);
    }

    /* Watchdog */
    for (int i = 0; i < 2; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->wdt[i]), errp)) {
            return;
        }
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->wdt[0]), 0, memmap[K230_DEV_WDT0].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->wdt[0]), 0,
                       k230_plic_irq(s, K230_WDT0_IRQ));

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->wdt[1]), 0, memmap[K230_DEV_WDT1].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->wdt[1]), 0,
                       k230_plic_irq(s, K230_WDT1_IRQ));

    /* GSDMA/PDMA/UGZIP blocks used by SDK U-Boot and Linux DT. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gsdma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gsdma), 0,
                    memmap[K230_DEV_GSDMA].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gsdma), 0,
                       k230_plic_irq(s, K230_DMA_IRQ));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pdma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pdma), 0, memmap[K230_DEV_DMA].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->pdma), 0,
                       k230_plic_irq(s, K230_PDMA_IRQ));

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
    for (int i = 0; i < K230_IPCM_IRQ_COUNT; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->hardlock), i,
                           k230_plic_irq(s, K230_IPCM_IRQ_BASE + i));
    }

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
                k230_plic_irq(s, K230_GPIO0_IRQ +
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
    k230_create_camera_i2c_devices(s);

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

    object_property_set_link(OBJECT(&s->sysctl_reset), "boot",
                             OBJECT(&s->sysctl_boot), &error_abort);
    qdev_prop_set_bit(DEVICE(&s->sysctl_reset), "defer-cpu1-release",
                      k230_machine->boot_both_cores);
    if (s->c908v_enabled) {
        object_property_set_link(OBJECT(&s->sysctl_reset), "cpu1",
                                 OBJECT(&s->c908v_cpu.harts[0]),
                                 &error_abort);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sysctl_reset), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sysctl_reset), 0,
                    memmap[K230_DEV_RMU].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pmu), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pmu), 0, memmap[K230_DEV_PMU].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rtc), 0, memmap[K230_DEV_RTC].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 0,
                       k230_plic_irq(s, K230_PMU_IRQ));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->security), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->security), 0,
                    memmap[K230_DEV_SECURITY].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->vo), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->vo), 0, memmap[K230_DEV_VO].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->vo), 0,
                       k230_plic_irq(s, K230_VO_IRQ));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dsi), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dsi), 0, memmap[K230_DEV_VO_CFG].base);

    if (!k230_create_regs(s, K230_REGS_CMU, memmap[K230_DEV_CMU].base,
                          memmap[K230_DEV_CMU].size, errp)) {
        return;
    }
    /*
     * Keep the legacy regs[] slot realized so K230_REGS_KPU_CFG retains its
     * index, while the dedicated KPU device owns the MMIO window.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->regs[K230_REGS_KPU_CFG]), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->kpu), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->kpu), 0,
                    memmap[K230_DEV_KPU_CFG].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->kpu), 0,
                       k230_plic_irq(s, K230_GNNE_IRQ));
    if (!k230_create_regs(s, K230_REGS_HDI, memmap[K230_DEV_HDI].base,
                          memmap[K230_DEV_HDI].size, errp)) {
        return;
    }
    if (!k230_create_stc(s, memmap[K230_DEV_STC].base,
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
    qdev_prop_set_uint64(DEVICE(&s->regs[K230_REGS_I2S]), "size",
                         memmap[K230_DEV_I2S].size);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->regs[K230_REGS_I2S]), errp)) {
        return;
    }
    s->i2s.compat_regs = &s->regs[K230_REGS_I2S];
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2s), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2s), 0,
                    memmap[K230_DEV_I2S].base);
    if (!k230_create_regs(s, K230_REGS_DDRC_CFG,
                          memmap[K230_DEV_DDRC_CFG].base,
                          memmap[K230_DEV_DDRC_CFG].size, errp)) {
        return;
    }
    if (!k230_create_irq_regs(s, K230_REGS_FFT, memmap[K230_DEV_FFT].base,
                              memmap[K230_DEV_FFT].size, K230_FFT_IRQ,
                              0x10, K230_REGS_NO_IRQ_OFFSET,
                              K230_REGS_NO_IRQ_OFFSET, 0x20,
                              K230_REGS_NO_IRQ_OFFSET, 0,
                              0, K230_REGS_NO_IRQ_OFFSET,
                              K230_REGS_NO_IRQ_OFFSET,
                              K230_REGS_NO_IRQ_OFFSET,
                              true, false, errp)) {
        return;
    }
    if (!k230_create_irq_regs(s, K230_REGS_AI_2D_ENGINE,
                              memmap[K230_DEV_AI_2D_ENGINE].base,
                              memmap[K230_DEV_AI_2D_ENGINE].size,
                              K230_AI2D_IRQ, K230_AI2D_CALC_ENABLE,
                              K230_AI2D_JOB_OFFSET, K230_AI2D_LEGACY_START,
                              K230_AI2D_CLEAR_OFFSET,
                              K230_REGS_NO_IRQ_OFFSET, 0,
                              0, K230_REGS_NO_IRQ_OFFSET,
                              K230_REGS_NO_IRQ_OFFSET,
                              K230_REGS_NO_IRQ_OFFSET,
                              true, false, errp)) {
        return;
    }
    /*
     * Keep the legacy regs[] slot realized so K230_REGS_DPU retains its old
     * index, while the dedicated non-AI 2D device owns the MMIO window.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->regs[K230_REGS_NON_AI_2D]),
                        errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->nonai_2d), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->nonai_2d), 0,
                    memmap[K230_DEV_NON_AI_2D].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->nonai_2d), 0,
                       k230_plic_irq(s, K230_NON_AI_2D_IRQ));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->isp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->isp), 0, memmap[K230_DEV_ISP].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->isp), 0,
                       k230_plic_irq(s, K230_ISP_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->isp), 1,
                       k230_plic_irq(s, K230_ISP_MI_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->isp), 2,
                       k230_plic_irq(s, K230_ISP_FE_IRQ));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rx_csi), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rx_csi), 0,
                    memmap[K230_DEV_RX_CSI].base);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dewarp), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dewarp), 0,
                    memmap[K230_DEV_DEWARP].base);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dewarp), 0,
                       k230_plic_irq(s, K230_DWE_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dewarp), 1,
                       k230_plic_irq(s, K230_FE_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dewarp), 2,
                       k230_plic_irq(s, K230_VSE_IRQ));

    qdev_prop_set_uint64(DEVICE(&s->regs[K230_REGS_DPU]),
                         "irq-start-mask", BIT(0));
    if (!k230_create_irq_regs(s, K230_REGS_DPU,
                              memmap[K230_DEV_3D_ENGINE].base,
                              memmap[K230_DEV_3D_ENGINE].size,
                              K230_DPU_IRQ, 0x200, 0x380,
                              K230_REGS_NO_IRQ_OFFSET, 0x1fc, 0x1f4, 0x3,
                              0, K230_REGS_NO_IRQ_OFFSET,
                              K230_REGS_NO_IRQ_OFFSET,
                              K230_REGS_NO_IRQ_OFFSET,
                              true, false, errp)) {
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
    create_unimplemented_device("vpu", memmap[K230_DEV_H264].base,
                                memmap[K230_DEV_H264].size);

    create_unimplemented_device("gpu", memmap[K230_DEV_2P5D].base,
                                memmap[K230_DEV_2P5D].size);

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
    RISCVHartArrayState *boot_harts = k230_boot_harts(s);
    const char *firmware_name = riscv_default_firmware_name(boot_harts);
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

    riscv_boot_info_init(&boot_info, boot_harts);
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

    riscv_setup_rom_reset_vec(machine, boot_harts, start_addr,
                              memmap[K230_DEV_BOOTROM].base,
                              memmap[K230_DEV_BOOTROM].size, kernel_entry,
                              K230_DIRECT_DTB_ADDR);
}

static void k230_firmware_boot(K230MachineState *s, MachineState *machine)
{
    RISCVHartArrayState *boot_harts = k230_boot_harts(s);
    const char *firmware_name = riscv_default_firmware_name(boot_harts);
    hwaddr start_addr = K230_FIRMWARE_OPENSBI_ADDR;
    RISCVBootInfo boot_info = {0};

    if (machine->dtb || (machine->kernel_cmdline && *machine->kernel_cmdline)) {
        error_report("K230 firmware boot does not support -dtb or -append");
        exit(EXIT_FAILURE);
    }

    riscv_boot_info_init(&boot_info, boot_harts);
    riscv_find_and_load_firmware(machine, &boot_info, firmware_name,
                                 &start_addr, NULL);

    riscv_setup_rom_reset_vec(machine, boot_harts, start_addr,
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

static bool k230_machine_get_boot_both_cores(Object *obj, Error **errp)
{
    K230MachineState *s = RISCV_K230_MACHINE(obj);

    return s->boot_both_cores;
}

static void k230_machine_set_boot_both_cores(Object *obj, bool value,
                                             Error **errp)
{
    K230MachineState *s = RISCV_K230_MACHINE(obj);

    s->boot_both_cores = value;
}

static void k230_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Canaan CanMV-K230 board";
    mc->init = k230_machine_init;
    mc->max_cpus = 2;
    mc->default_cpus = 1;
    mc->default_ram_id = "riscv.K230.ram"; /* DDR */
    mc->default_ram_size = memmap[K230_DEV_DDRC].size;
    mc->default_nic = "usb-rtl8152";
    mc->auto_create_sdcard = true;

    object_class_property_add_bool(oc, "boot-both-cores",
                                   k230_machine_get_boot_both_cores,
                                   k230_machine_set_boot_both_cores);
    object_class_property_set_description(
        oc, "boot-both-cores",
        "Start C908 and C908V at reset for U-Boot dual-core bring-up");
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
