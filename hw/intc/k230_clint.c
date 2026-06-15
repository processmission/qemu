/*
 * K230 T-Head C908 S-mode CLINT extension
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/intc/k230_clint.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "target/riscv/cpu.h"
#include "target/riscv/time_helper.h"

#define K230_CLINT_CPU_INDEX_AUTO UINT32_MAX
#define K230_CLINT_SMODE_SIZE     0x2000
#define K230_CLINT_SSIP_BASE      0x0000
#define K230_CLINT_STIMECMP_BASE  0x1000

static CPUState *k230_clint_cpu(K230ClintSModeState *s, uint32_t hart_offset)
{
    if (s->cpu_index_base == K230_CLINT_CPU_INDEX_AUTO) {
        return cpu_by_arch_id(s->hartid_base + hart_offset);
    }

    return qemu_get_cpu(s->cpu_index_base + hart_offset);
}

static CPURISCVState *k230_clint_env(K230ClintSModeState *s,
                                     hwaddr hart_offset)
{
    CPUState *cpu;

    if (hart_offset >= s->num_harts) {
        return NULL;
    }

    cpu = k230_clint_cpu(s, hart_offset);
    return cpu ? cpu_env(cpu) : NULL;
}

static uint64_t k230_clint_read_ssip(K230ClintSModeState *s, hwaddr addr)
{
    CPURISCVState *env = k230_clint_env(s, addr >> 2);

    if (!env || (addr & 0x3)) {
        qemu_log_mask(LOG_UNIMP,
                      "k230-clint: invalid SSIP read: %08x",
                      (uint32_t)addr);
        return 0;
    }

    return (env->mip & MIP_SSIP) != 0;
}

static void k230_clint_write_ssip(K230ClintSModeState *s, hwaddr addr,
                                  uint64_t value)
{
    CPURISCVState *env = k230_clint_env(s, addr >> 2);

    if (!env || (addr & 0x3)) {
        qemu_log_mask(LOG_UNIMP,
                      "k230-clint: invalid SSIP write: %08x",
                      (uint32_t)addr);
        return;
    }

    riscv_cpu_update_mip(env, MIP_SSIP, BOOL_TO_MASK(value & 0x1));
}

static uint64_t k230_clint_read_stimecmp(K230ClintSModeState *s, hwaddr addr,
                                         unsigned size)
{
    hwaddr reg_addr = addr - K230_CLINT_STIMECMP_BASE;
    CPURISCVState *env = k230_clint_env(s, reg_addr >> 3);

    if (!env) {
        qemu_log_mask(LOG_UNIMP,
                      "k230-clint: invalid STIMECMP read: %08x",
                      (uint32_t)addr);
        return 0;
    }

    switch (reg_addr & 0x7) {
    case 0:
        return (size == 4) ? (env->stimecmp & 0xffffffff) : env->stimecmp;
    case 4:
        if (size == 4) {
            return env->stimecmp >> 32;
        }
        break;
    }

    qemu_log_mask(LOG_UNIMP,
                  "k230-clint: invalid STIMECMP read: %08x",
                  (uint32_t)addr);
    return 0;
}

static void k230_clint_write_stimecmp(K230ClintSModeState *s, hwaddr addr,
                                      uint64_t value, unsigned size)
{
    hwaddr reg_addr = addr - K230_CLINT_STIMECMP_BASE;
    CPURISCVState *env = k230_clint_env(s, reg_addr >> 3);

    if (!env) {
        qemu_log_mask(LOG_UNIMP,
                      "k230-clint: invalid STIMECMP write: %08x",
                      (uint32_t)addr);
        return;
    }

    switch (reg_addr & 0x7) {
    case 0:
        if (size == 4) {
            env->stimecmp = deposit64(env->stimecmp, 0, 32, value);
        } else {
            env->stimecmp = value;
        }
        break;
    case 4:
        if (size == 4) {
            env->stimecmp = deposit64(env->stimecmp, 32, 32, value);
            break;
        }
        qemu_log_mask(LOG_UNIMP,
                      "k230-clint: invalid STIMECMPH write: %08x",
                      (uint32_t)addr);
        return;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "k230-clint: invalid STIMECMP write: %08x",
                      (uint32_t)addr);
        return;
    }

    riscv_timer_write_timecmp_mmio(env, env->stimer, env->stimecmp, 0,
                                   MIP_STIP);
}

static uint64_t k230_clint_read(void *opaque, hwaddr addr, unsigned size)
{
    K230ClintSModeState *s = K230_CLINT_SMODE(opaque);

    if (addr >= K230_CLINT_SSIP_BASE &&
        addr < K230_CLINT_STIMECMP_BASE) {
        return k230_clint_read_ssip(s, addr);
    }

    if (addr >= K230_CLINT_STIMECMP_BASE &&
        addr < K230_CLINT_SMODE_SIZE) {
        return k230_clint_read_stimecmp(s, addr, size);
    }

    qemu_log_mask(LOG_UNIMP, "k230-clint: invalid read: %08x",
                  (uint32_t)addr);
    return 0;
}

static void k230_clint_write(void *opaque, hwaddr addr, uint64_t value,
                             unsigned size)
{
    K230ClintSModeState *s = K230_CLINT_SMODE(opaque);

    if (addr >= K230_CLINT_SSIP_BASE &&
        addr < K230_CLINT_STIMECMP_BASE) {
        k230_clint_write_ssip(s, addr, value);
        return;
    }

    if (addr >= K230_CLINT_STIMECMP_BASE &&
        addr < K230_CLINT_SMODE_SIZE) {
        k230_clint_write_stimecmp(s, addr, value, size);
        return;
    }

    qemu_log_mask(LOG_UNIMP, "k230-clint: invalid write: %08x",
                  (uint32_t)addr);
}

static const MemoryRegionOps k230_clint_ops = {
    .read = k230_clint_read,
    .write = k230_clint_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static const Property k230_clint_properties[] = {
    DEFINE_PROP_UINT32("hartid-base", K230ClintSModeState, hartid_base, 0),
    DEFINE_PROP_UINT32("cpu-index-base", K230ClintSModeState, cpu_index_base,
                       K230_CLINT_CPU_INDEX_AUTO),
    DEFINE_PROP_UINT32("num-harts", K230ClintSModeState, num_harts, 1),
};

static void k230_clint_realize(DeviceState *dev, Error **errp)
{
    K230ClintSModeState *s = K230_CLINT_SMODE(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &k230_clint_ops, s,
                          TYPE_K230_CLINT_SMODE, K230_CLINT_SMODE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static void k230_clint_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = k230_clint_realize;
    device_class_set_props(dc, k230_clint_properties);
    dc->desc = "K230 T-Head C908 S-mode CLINT extension";
}

static const TypeInfo k230_clint_type_info = {
    .name = TYPE_K230_CLINT_SMODE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(K230ClintSModeState),
    .class_init = k230_clint_class_init,
};

DeviceState *k230_clint_smode_create_in(MemoryRegion *mem, hwaddr addr,
                                        uint32_t hartid_base,
                                        uint32_t cpu_index_base,
                                        uint32_t num_harts)
{
    DeviceState *dev = qdev_new(TYPE_K230_CLINT_SMODE);

    qdev_prop_set_uint32(dev, "hartid-base", hartid_base);
    qdev_prop_set_uint32(dev, "cpu-index-base", cpu_index_base);
    qdev_prop_set_uint32(dev, "num-harts", num_harts);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    if (mem == get_system_memory()) {
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    } else {
        memory_region_add_subregion_overlap(mem, addr,
                                             sysbus_mmio_get_region(
                                                 SYS_BUS_DEVICE(dev), 0),
                                             1);
    }

    return dev;
}

static void k230_clint_register_types(void)
{
    type_register_static(&k230_clint_type_info);
}

type_init(k230_clint_register_types)
