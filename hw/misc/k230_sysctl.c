/*
 * K230 system controller blocks
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "exec/cpu-common.h"
#include "system/physmem.h"
#include "migration/vmstate.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/k230_sysctl.h"

#define K230_SYSCTL_RTT_SAVE_SIZE (32 * 1024 * 1024)

#define K230_SYSCTL_PLL_COUNT 4
#define K230_SYSCTL_PLL_STRIDE 0x10
#define K230_SYSCTL_PLL_CFG0(n) ((n) * K230_SYSCTL_PLL_STRIDE)
#define K230_SYSCTL_PLL_CTL(n)  (K230_SYSCTL_PLL_CFG0(n) + 0x08)
#define K230_SYSCTL_PLL_STAT(n) (K230_SYSCTL_PLL_CFG0(n) + 0x0c)
#define K230_SYSCTL_PLL_GATE_EN BIT(2)
#define K230_SYSCTL_PLL_LOCK    BIT(0)

#define K230_SYSCTL_PWR_ON      BIT(1)
#define K230_SYSCTL_PWR_ON_WEN  BIT(17)
#define K230_SYSCTL_PWR_OFF     BIT(0)
#define K230_SYSCTL_PWR_OFF_WEN BIT(16)
#define K230_SYSCTL_REPAIR_DONE 0x7
#define K230_SYSCTL_AI_REPAIR   BIT(4)
#define K230_SYSCTL_REPAIR_WEN  BIT(20)

/*
 * SDK U-Boot programs cpu1_hart_rstvec in the BOOT block, then releases
 * CPU1 through CPU1_RST_CTL using per-bit write-enable bits.
 */
#define K230_SYSCTL_CPU1_RST_CTL      0x0c
#define K230_SYSCTL_CPU1_RSTVEC       0x104
#define K230_SYSCTL_CPU1_RST_REQ      BIT(0)
#define K230_SYSCTL_CPU1_RST_DONE     BIT(12)
#define K230_SYSCTL_CPU1_PRST_DONE    BIT(13)
#define K230_SYSCTL_CPU1_RST_REQ_WEN  BIT(16)
#define K230_SYSCTL_CPU1_RST_DONE_WEN BIT(28)
#define K230_SYSCTL_CPU1_PRST_DONE_WEN BIT(29)
#define K230_SYSCTL_CPU1_RST_CTL_RESET 0x00002001

typedef struct K230SysctlPowerDomain {
    hwaddr en;
    hwaddr stat;
} K230SysctlPowerDomain;

typedef struct K230SysctlCpu1Reset {
    uint64_t rstvec;
} K230SysctlCpu1Reset;

static const K230SysctlPowerDomain k230_power_domains[] = {
    { 0x018, 0x01c }, /* CPU1 */
    { 0x028, 0x02c }, /* AI */
    { 0x03c, 0x040 }, /* DISP */
    { 0x07c, 0x080 }, /* VPU */
    { 0x108, 0x10c }, /* DPU */
};

static uint64_t k230_sysctl_read_bytes(uint8_t *regs, hwaddr addr,
                                       unsigned int size)
{
    uint64_t val = 0;

    if (addr > K230_SYSCTL_SIZE || size > K230_SYSCTL_SIZE - addr) {
        return 0;
    }

    for (int i = 0; i < size; i++) {
        val |= (uint64_t)regs[addr + i] << (i * 8);
    }

    return val;
}

static void k230_sysctl_write_bytes(uint8_t *regs, hwaddr addr, uint64_t val,
                                    unsigned int size)
{
    if (addr > K230_SYSCTL_SIZE || size > K230_SYSCTL_SIZE - addr) {
        return;
    }

    for (int i = 0; i < size; i++) {
        regs[addr + i] = val >> (i * 8);
    }
}

static uint32_t k230_sysctl_reg_read32(uint8_t *regs, hwaddr addr)
{
    return ldl_le_p(regs + addr);
}

static void k230_sysctl_reg_write32(uint8_t *regs, hwaddr addr, uint32_t val)
{
    stl_le_p(regs + addr, val);
}

static uint64_t k230_sysctl_boot_read(void *opaque, hwaddr addr,
                                      unsigned int size)
{
    return k230_sysctl_read_bytes(K230_SYSCTL_BOOT(opaque)->regs, addr, size);
}

static void k230_sysctl_boot_refresh_locks(K230SysctlBootState *s)
{
    for (int i = 0; i < K230_SYSCTL_PLL_COUNT; i++) {
        hwaddr stat = K230_SYSCTL_PLL_STAT(i);

        k230_sysctl_reg_write32(s->regs, stat,
            k230_sysctl_reg_read32(s->regs, stat) | K230_SYSCTL_PLL_LOCK);
    }
}

static void k230_sysctl_boot_write(void *opaque, hwaddr addr, uint64_t val,
                                   unsigned int size)
{
    K230SysctlBootState *s = K230_SYSCTL_BOOT(opaque);

    k230_sysctl_write_bytes(s->regs, addr, val, size);
    k230_sysctl_boot_refresh_locks(s);
}

static const MemoryRegionOps k230_sysctl_boot_ops = {
    .read = k230_sysctl_boot_read,
    .write = k230_sysctl_boot_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void k230_sysctl_boot_set_pll(K230SysctlBootState *s, int pll,
                                     uint32_t fb_div, uint32_t ref_div,
                                     uint32_t out_div)
{
    uint32_t cfg0 = ((out_div - 1) << 24) |
                   ((ref_div - 1) << 16) |
                   (fb_div - 1);

    k230_sysctl_reg_write32(s->regs, K230_SYSCTL_PLL_CFG0(pll), cfg0);
    k230_sysctl_reg_write32(s->regs, K230_SYSCTL_PLL_CTL(pll),
                            K230_SYSCTL_PLL_GATE_EN);
    k230_sysctl_reg_write32(s->regs, K230_SYSCTL_PLL_STAT(pll),
                            K230_SYSCTL_PLL_LOCK);
}

static void k230_sysctl_boot_reset(DeviceState *dev)
{
    K230SysctlBootState *s = K230_SYSCTL_BOOT(dev);

    memset(s->regs, 0, sizeof(s->regs));

    k230_sysctl_boot_set_pll(s, 0, 200, 3, 1);
    k230_sysctl_boot_set_pll(s, 1, 99, 1, 1);
    k230_sysctl_boot_set_pll(s, 2, 111, 1, 1);
    k230_sysctl_boot_set_pll(s, 3, 200, 3, 1);
}

static const VMStateDescription vmstate_k230_sysctl_boot = {
    .name = TYPE_K230_SYSCTL_BOOT,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230SysctlBootState, K230_SYSCTL_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_sysctl_boot_realize(DeviceState *dev, Error **errp)
{
    K230SysctlBootState *s = K230_SYSCTL_BOOT(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_sysctl_boot_ops, s,
                          TYPE_K230_SYSCTL_BOOT, K230_SYSCTL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_sysctl_boot_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_sysctl_boot_realize;
    device_class_set_legacy_reset(dc, k230_sysctl_boot_reset);
    dc->vmsd = &vmstate_k230_sysctl_boot;
    dc->desc = "K230 sysctl boot registers";
}

static const TypeInfo k230_sysctl_boot_type_info = {
    .name = TYPE_K230_SYSCTL_BOOT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230SysctlBootState),
    .class_init = k230_sysctl_boot_class_init,
};

static uint64_t k230_sysctl_power_read(void *opaque, hwaddr addr,
                                       unsigned int size)
{
    return k230_sysctl_read_bytes(K230_SYSCTL_POWER(opaque)->regs, addr, size);
}

static void k230_sysctl_power_update_domain(K230SysctlPowerState *s,
                                            hwaddr addr, uint32_t val)
{
    for (int i = 0; i < ARRAY_SIZE(k230_power_domains); i++) {
        const K230SysctlPowerDomain *domain = &k230_power_domains[i];

        if (addr != domain->en) {
            continue;
        }

        if ((val & K230_SYSCTL_PWR_ON_WEN) && (val & K230_SYSCTL_PWR_ON)) {
            k230_sysctl_reg_write32(s->regs, domain->stat,
                                    K230_SYSCTL_PWR_ON);
        } else if ((val & K230_SYSCTL_PWR_OFF_WEN) &&
                   (val & K230_SYSCTL_PWR_OFF)) {
            k230_sysctl_reg_write32(s->regs, domain->stat,
                                    K230_SYSCTL_PWR_OFF);
        }

        if (addr == k230_power_domains[1].en &&
            (val & K230_SYSCTL_REPAIR_WEN) && (val & K230_SYSCTL_AI_REPAIR)) {
            k230_sysctl_reg_write32(s->regs, 0x160, K230_SYSCTL_REPAIR_DONE);
        }
    }
}

static void k230_sysctl_power_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned int size)
{
    K230SysctlPowerState *s = K230_SYSCTL_POWER(opaque);

    k230_sysctl_write_bytes(s->regs, addr, val, size);

    if (size == 4) {
        k230_sysctl_power_update_domain(s, addr, val);
    }
}

static const MemoryRegionOps k230_sysctl_power_ops = {
    .read = k230_sysctl_power_read,
    .write = k230_sysctl_power_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void k230_sysctl_power_reset(DeviceState *dev)
{
    K230SysctlPowerState *s = K230_SYSCTL_POWER(dev);

    memset(s->regs, 0, sizeof(s->regs));

    for (int i = 0; i < ARRAY_SIZE(k230_power_domains); i++) {
        k230_sysctl_reg_write32(s->regs, k230_power_domains[i].stat,
                                K230_SYSCTL_PWR_OFF);
    }
}

static const VMStateDescription vmstate_k230_sysctl_power = {
    .name = TYPE_K230_SYSCTL_POWER,
    .version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230SysctlPowerState, K230_SYSCTL_SIZE),
        VMSTATE_END_OF_LIST(),
    },
};

static void k230_sysctl_power_realize(DeviceState *dev, Error **errp)
{
    K230SysctlPowerState *s = K230_SYSCTL_POWER(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_sysctl_power_ops, s,
                          TYPE_K230_SYSCTL_POWER, K230_SYSCTL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_sysctl_power_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_sysctl_power_realize;
    device_class_set_legacy_reset(dc, k230_sysctl_power_reset);
    dc->vmsd = &vmstate_k230_sysctl_power;
    dc->desc = "K230 sysctl power registers";
}

static const TypeInfo k230_sysctl_power_type_info = {
    .name = TYPE_K230_SYSCTL_POWER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230SysctlPowerState),
    .class_init = k230_sysctl_power_class_init,
};

static uint64_t k230_sysctl_reset_read(void *opaque, hwaddr addr,
                                       unsigned int size)
{
    return k230_sysctl_read_bytes(K230_SYSCTL_RESET(opaque)->regs, addr, size);
}

static void k230_sysctl_reset_restore_rtt(K230SysctlResetState *s)
{
    if (s->rtt_saved_valid) {
        physical_memory_write(s->rtt_addr, s->rtt_saved, s->rtt_size);
        g_free(s->rtt_saved);
        s->rtt_saved = NULL;
        s->rtt_saved_valid = false;
        s->rtt_addr = 0;
        s->rtt_size = 0;
        s->deferred_rstvec = 0;
    }
}

static void k230_sysctl_reset_cancel_deferred_release(K230SysctlResetState *s)
{
    if (s->release_timer) {
        timer_del(s->release_timer);
    }
    k230_sysctl_reset_restore_rtt(s);
}

static bool k230_sysctl_reset_has_rtt_saved(void *opaque, int version_id)
{
    K230SysctlResetState *s = K230_SYSCTL_RESET(opaque);

    return s->rtt_saved_valid;
}




static void k230_sysctl_reset_cpu1_async_work(CPUState *cpu,
                                              run_on_cpu_data data)
{
    K230SysctlCpu1Reset *reset = data.host_ptr;

    cpu_reset(cpu);
    cpu_set_pc(cpu, reset->rstvec);
    cpu->halted = 0;
    qemu_cpu_kick(cpu);
    g_free(reset);
}

static void k230_sysctl_reset_cpu1_hold_work(CPUState *cpu,
                                             run_on_cpu_data data)
{
    cpu_reset(cpu);
    cpu->halted = 1;
    qemu_cpu_kick(cpu);
}

static void k230_sysctl_reset_assert_cpu1(K230SysctlResetState *s)
{
    if (!s->cpu1) {
        return;
    }
    run_on_cpu(s->cpu1, k230_sysctl_reset_cpu1_hold_work, RUN_ON_CPU_NULL);
}


static void k230_sysctl_reset_release_cpu1_rstvec(K230SysctlResetState *s,
                                                  uint32_t rstvec)
{
    K230SysctlCpu1Reset *reset;

    if (!s->cpu1 || !s->boot) {
        return;
    }

    reset = g_new(K230SysctlCpu1Reset, 1);
    reset->rstvec = rstvec;
    s->last_cpu1_rstvec = reset->rstvec;
    run_on_cpu(s->cpu1, k230_sysctl_reset_cpu1_async_work,
               RUN_ON_CPU_HOST_PTR(reset));
}

static void k230_sysctl_reset_release_cpu1_now(K230SysctlResetState *s)
{
    uint32_t rstvec;

    if (!s->cpu1 || !s->boot) {
        return;
    }

    rstvec = k230_sysctl_reg_read32(s->boot->regs, K230_SYSCTL_CPU1_RSTVEC);
    k230_sysctl_reset_release_cpu1_rstvec(s, rstvec);
}

static void k230_sysctl_reset_release_cpu1_timer(void *opaque)
{
    K230SysctlResetState *s = opaque;
    uint32_t rstvec = s->deferred_rstvec;

    k230_sysctl_reset_restore_rtt(s);
    k230_sysctl_reset_release_cpu1_rstvec(s, rstvec);
}

static void k230_sysctl_reset_defer_cpu1(K230SysctlResetState *s)
{
    void *zeros;

    k230_sysctl_reset_cancel_deferred_release(s);

    s->rtt_addr = k230_sysctl_reg_read32(s->boot->regs,
                                         K230_SYSCTL_CPU1_RSTVEC);
    s->deferred_rstvec = s->rtt_addr;
    s->rtt_size = K230_SYSCTL_RTT_SAVE_SIZE;
    s->rtt_saved = g_malloc(s->rtt_size);
    physical_memory_read(s->rtt_addr, s->rtt_saved, s->rtt_size);
    zeros = g_malloc0(s->rtt_size);
    physical_memory_write(s->rtt_addr, zeros, s->rtt_size);
    g_free(zeros);
    s->rtt_saved_valid = true;

    /*
     * Defer CPU1 release to give Linux time to boot before the big
     * core starts executing.
     */
    timer_mod(s->release_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 15000);
}

static void k230_sysctl_reset_release_cpu1(K230SysctlResetState *s)
{
    if (!s->cpu1 || !s->boot) {
        return;
    }

    if (s->defer_cpu1_release) {
        k230_sysctl_reset_defer_cpu1(s);
        return;
    }

    k230_sysctl_reset_release_cpu1_now(s);
}

static void k230_sysctl_reset_write_cpu1(K230SysctlResetState *s,
                                         uint32_t val)
{
    uint32_t old = k230_sysctl_reg_read32(s->regs,
                                          K230_SYSCTL_CPU1_RST_CTL);
    uint32_t new = old;

    if ((val & K230_SYSCTL_CPU1_RST_DONE_WEN) &&
        (val & K230_SYSCTL_CPU1_RST_DONE)) {
        new &= ~K230_SYSCTL_CPU1_RST_DONE;
    }
    if ((val & K230_SYSCTL_CPU1_PRST_DONE_WEN) &&
        (val & K230_SYSCTL_CPU1_PRST_DONE)) {
        new &= ~K230_SYSCTL_CPU1_PRST_DONE;
    }

    if (val & K230_SYSCTL_CPU1_RST_REQ_WEN) {
        if (val & K230_SYSCTL_CPU1_RST_REQ) {
            new |= K230_SYSCTL_CPU1_RST_REQ;
        } else {
            new &= ~K230_SYSCTL_CPU1_RST_REQ;
        }
    }

    if (!(old & K230_SYSCTL_CPU1_RST_REQ) &&
        (new & K230_SYSCTL_CPU1_RST_REQ)) {
        k230_sysctl_reset_cancel_deferred_release(s);
        k230_sysctl_reset_assert_cpu1(s);
    }

    if ((old & K230_SYSCTL_CPU1_RST_REQ) &&
        !(new & K230_SYSCTL_CPU1_RST_REQ)) {
        k230_sysctl_reset_release_cpu1(s);
        new |= K230_SYSCTL_CPU1_RST_DONE;
    }

    k230_sysctl_reg_write32(s->regs, K230_SYSCTL_CPU1_RST_CTL, new);
}

static void k230_sysctl_reset_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned int size)
{
    K230SysctlResetState *s = K230_SYSCTL_RESET(opaque);

    if (size == 4 && addr == K230_SYSCTL_CPU1_RST_CTL) {
        k230_sysctl_reset_write_cpu1(s, val);
    } else {
        k230_sysctl_write_bytes(s->regs, addr, val, size);
    }
}

static const MemoryRegionOps k230_sysctl_reset_ops = {
    .read = k230_sysctl_reset_read,
    .write = k230_sysctl_reset_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
        .unaligned = true,
    },
};

static void k230_sysctl_reset_reset(DeviceState *dev)
{
    K230SysctlResetState *s = K230_SYSCTL_RESET(dev);

    k230_sysctl_reset_cancel_deferred_release(s);
    memset(s->regs, 0, sizeof(s->regs));
    k230_sysctl_reg_write32(s->regs, K230_SYSCTL_CPU1_RST_CTL,
                            K230_SYSCTL_CPU1_RST_CTL_RESET);
    s->last_cpu1_rstvec = 0;
    s->deferred_rstvec = 0;
}

static const VMStateDescription vmstate_k230_sysctl_reset = {
    .name = TYPE_K230_SYSCTL_RESET,
    .version_id = 4,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, K230SysctlResetState, K230_SYSCTL_SIZE),
        VMSTATE_UINT32_V(last_cpu1_rstvec, K230SysctlResetState, 2),
        VMSTATE_BOOL_V(rtt_saved_valid, K230SysctlResetState, 3),
        VMSTATE_UINT64_TEST(rtt_addr, K230SysctlResetState,
                            k230_sysctl_reset_has_rtt_saved),
        VMSTATE_UINT32_TEST(rtt_size, K230SysctlResetState,
                            k230_sysctl_reset_has_rtt_saved),
        VMSTATE_VBUFFER_ALLOC_UINT32(rtt_saved, K230SysctlResetState, 3,
                                     k230_sysctl_reset_has_rtt_saved,
                                     rtt_size),
        VMSTATE_TIMER_PTR_V(release_timer, K230SysctlResetState, 3),
        VMSTATE_UINT32_TEST(deferred_rstvec, K230SysctlResetState,
                            k230_sysctl_reset_has_rtt_saved),
        VMSTATE_END_OF_LIST(),
    },
};

static const Property k230_sysctl_reset_properties[] = {
    DEFINE_PROP_LINK("boot", K230SysctlResetState, boot,
                     TYPE_K230_SYSCTL_BOOT, K230SysctlBootState *),
    DEFINE_PROP_LINK("cpu1", K230SysctlResetState, cpu1, TYPE_CPU,
                     CPUState *),
    DEFINE_PROP_BOOL("defer-cpu1-release", K230SysctlResetState,
                     defer_cpu1_release, false),
};

static void k230_sysctl_reset_realize(DeviceState *dev, Error **errp)
{
    K230SysctlResetState *s = K230_SYSCTL_RESET(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_sysctl_reset_ops, s,
                          TYPE_K230_SYSCTL_RESET, K230_SYSCTL_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    s->release_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL,
                                    k230_sysctl_reset_release_cpu1_timer, s);
}

static void k230_sysctl_reset_init(Object *obj)
{
    K230SysctlResetState *s = K230_SYSCTL_RESET(obj);

    object_property_add_uint32_ptr(obj, "last-cpu1-rstvec",
                                   &s->last_cpu1_rstvec,
                                   OBJ_PROP_FLAG_READ);
}


static void k230_sysctl_reset_finalize(Object *obj)
{
    K230SysctlResetState *s = K230_SYSCTL_RESET(obj);

    if (s->release_timer) {
        timer_free(s->release_timer);
        s->release_timer = NULL;
    }
    g_free(s->rtt_saved);
    s->rtt_saved = NULL;
    s->rtt_saved_valid = false;
}

static void k230_sysctl_reset_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_sysctl_reset_realize;
    device_class_set_props(dc, k230_sysctl_reset_properties);
    device_class_set_legacy_reset(dc, k230_sysctl_reset_reset);
    dc->vmsd = &vmstate_k230_sysctl_reset;
    dc->desc = "K230 sysctl reset registers";
}

static const TypeInfo k230_sysctl_reset_type_info = {
    .name = TYPE_K230_SYSCTL_RESET,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230SysctlResetState),
    .instance_init = k230_sysctl_reset_init,
    .instance_finalize = k230_sysctl_reset_finalize,
    .class_init = k230_sysctl_reset_class_init,
};

static void k230_sysctl_register_types(void)
{
    type_register_static(&k230_sysctl_boot_type_info);
    type_register_static(&k230_sysctl_power_type_info);
    type_register_static(&k230_sysctl_reset_type_info);
}

type_init(k230_sysctl_register_types)
