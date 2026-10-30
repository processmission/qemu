/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/guest-random.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/register.h"
#include "hw/core/sysbus.h"

#define TYPE_RK3566_SYSTEM "rk3566-system"
#define RK3566_SYSTEM_MMIO_SIZE 0x2000
#define RK3566_SYSTEM_NUM_WORDS (RK3566_SYSTEM_MMIO_SIZE / 4)
#define RK3566_SYSTEM_PVTM_CLOCK_HZ 24000000

OBJECT_DECLARE_SIMPLE_TYPE(RK3566SystemState, RK3566_SYSTEM)

REG32(PMU_MIRROR0, 0x50)
REG32(PMU_MIRROR0_LO, 0x60)
REG32(PMU_MIRROR0_HI, 0x68)
REG32(PMU_MIRROR1, 0xa0)
REG32(PMU_MIRROR1_LO, 0x98)
REG32(RNG_CTRL, 0x400)
REG32(RNG_DATA, 0x410)
REG32(PVTM_CTRL, 0x04)
REG32(PVTM_PERIOD, 0x08)
REG32(PVTM_STATUS, 0x80)
REG32(PVTM_RESULT, 0x84)

#define RK3566_SYSTEM_NUM_REGS (A_RNG_CTRL / 4 + 1)

struct RK3566SystemState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[RK3566_SYSTEM_NUM_REGS];
    uint32_t regs[RK3566_SYSTEM_NUM_WORDS];

    bool rng;
    bool pvtm;
    uint32_t oscillator_hz;
    int64_t deadline;
};

typedef struct RK3566SystemVariant {
    const RegisterAccessInfo *regs_info;
    int num_regs;
} RK3566SystemVariant;

/*
 * Most addresses of the window take the upper half-word as a bit mask.  The
 * RNG control block and the PVTM period are plain stores instead.
 */
static bool system_unmasked_write(RK3566SystemState *s, hwaddr addr)
{
    return (s->rng && addr != A_RNG_CTRL) ||
           (s->pvtm && addr == A_PVTM_PERIOD);
}

static uint64_t system_hiword_pre_write(RegisterInfo *reg, uint64_t val)
{
    RK3566SystemState *s = RK3566_SYSTEM(reg->opaque);
    uint32_t *data = reg->data;
    uint32_t mask;

    if (system_unmasked_write(s, reg->access->addr)) {
        return val;
    }
    mask = extract32(val, 16, 16);
    return (*data & ~mask) | (val & mask);
}

static void system_pmu_mirror0_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566SystemState *s = RK3566_SYSTEM(reg->opaque);

    s->regs[R_PMU_MIRROR0_LO] = val;
    s->regs[R_PMU_MIRROR0_HI] = val;
}

static void system_pmu_mirror1_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566SystemState *s = RK3566_SYSTEM(reg->opaque);

    s->regs[R_PMU_MIRROR1_LO] = val;
}

static void system_rng_ctrl_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566SystemState *s = RK3566_SYSTEM(reg->opaque);

    if ((val & 3) != 3) {
        return;
    }
    if (!qemu_guest_getrandom(&s->regs[R_RNG_DATA], 32, &error_fatal)) {
        s->regs[R_RNG_CTRL] &= ~1;
    }
}

static void system_pvtm_ctrl_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566SystemState *s = RK3566_SYSTEM(reg->opaque);

    if ((val & 3) != 3) {
        return;
    }
    s->regs[R_PVTM_STATUS] = 0;
    s->deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (uint64_t)s->regs[R_PVTM_PERIOD] *
                  NANOSECONDS_PER_SECOND / RK3566_SYSTEM_PVTM_CLOCK_HZ;
}

static bool system_pvtm_measure(RK3566SystemState *s)
{
    if (!s->pvtm || (s->regs[R_PVTM_CTRL] & 3) != 3 ||
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < s->deadline) {
        return false;
    }
    s->regs[R_PVTM_STATUS] = 1;
    s->regs[R_PVTM_RESULT] = (uint64_t)s->regs[R_PVTM_PERIOD] *
                             s->oscillator_hz / RK3566_SYSTEM_PVTM_CLOCK_HZ;
    return true;
}

static uint64_t system_pvtm_status_post_read(RegisterInfo *reg, uint64_t val)
{
    RK3566SystemState *s = RK3566_SYSTEM(reg->opaque);

    return system_pvtm_measure(s) ? 1 : val;
}

static uint64_t system_pvtm_result_post_read(RegisterInfo *reg, uint64_t val)
{
    RK3566SystemState *s = RK3566_SYSTEM(reg->opaque);

    return system_pvtm_measure(s) ? s->regs[R_PVTM_RESULT] : val;
}

static const RegisterAccessInfo system_pmu_regs_info[] = {
    {
        .name = "PMU_MIRROR0",
        .addr = A_PMU_MIRROR0,
        .pre_write = system_hiword_pre_write,
        .post_write = system_pmu_mirror0_post_write,
    }, {
        .name = "PMU_MIRROR1",
        .addr = A_PMU_MIRROR1,
        .pre_write = system_hiword_pre_write,
        .post_write = system_pmu_mirror1_post_write,
    },
};

static const RegisterAccessInfo system_rng_regs_info[] = {
    {
        .name = "RNG_CTRL",
        .addr = A_RNG_CTRL,
        .pre_write = system_hiword_pre_write,
        .post_write = system_rng_ctrl_post_write,
    },
};

static const RegisterAccessInfo system_pvtm_regs_info[] = {
    {
        .name = "PVTM_CTRL",
        .addr = A_PVTM_CTRL,
        .pre_write = system_hiword_pre_write,
        .post_write = system_pvtm_ctrl_post_write,
    }, {
        .name = "PVTM_PERIOD",
        .addr = A_PVTM_PERIOD,
    }, {
        .name = "PVTM_STATUS",
        .addr = A_PVTM_STATUS,
        .ro = UINT32_MAX,
        .post_read = system_pvtm_status_post_read,
    }, {
        .name = "PVTM_RESULT",
        .addr = A_PVTM_RESULT,
        .ro = UINT32_MAX,
        .post_read = system_pvtm_result_post_read,
    },
};

static const RK3566SystemVariant system_pmu_variant = {
    .regs_info = system_pmu_regs_info,
    .num_regs = ARRAY_SIZE(system_pmu_regs_info),
};

static const RK3566SystemVariant system_rng_variant = {
    .regs_info = system_rng_regs_info,
    .num_regs = ARRAY_SIZE(system_rng_regs_info),
};

static const RK3566SystemVariant system_pvtm_variant = {
    .regs_info = system_pvtm_regs_info,
    .num_regs = ARRAY_SIZE(system_pvtm_regs_info),
};

static const RK3566SystemVariant *system_get_variant(RK3566SystemState *s)
{
    if (s->pvtm) {
        return &system_pvtm_variant;
    }
    if (s->rng) {
        return &system_rng_variant;
    }
    return &system_pmu_variant;
}

static bool system_is_modeled_reg(RegisterInfoArray *reg_array, hwaddr offset)
{
    for (int i = 0; i < reg_array->num_elements; i++) {
        if (reg_array->r[i]->access->addr == offset) {
            return true;
        }
    }
    return false;
}

static uint64_t system_read(void *opaque, hwaddr offset, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566SystemState *s =
        RK3566_SYSTEM(register_array_get_owner(reg_array));

    if (system_is_modeled_reg(reg_array, offset)) {
        return register_read_memory(reg_array, offset, size);
    }
    return s->regs[offset / sizeof(uint32_t)];
}

static void system_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566SystemState *s =
        RK3566_SYSTEM(register_array_get_owner(reg_array));
    uint32_t *reg = &s->regs[offset / sizeof(uint32_t)];
    uint32_t mask;

    if (system_is_modeled_reg(reg_array, offset)) {
        register_write_memory(reg_array, offset, value, size);
        return;
    }
    if (system_unmasked_write(s, offset)) {
        *reg = value;
        return;
    }
    mask = extract32(value, 16, 16);
    *reg = (*reg & ~mask) | (value & mask);
}

static const MemoryRegionOps system_ops = {
    .read = system_read,
    .write = system_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void system_realize(DeviceState *dev, Error **errp)
{
    RK3566SystemState *s = RK3566_SYSTEM(dev);
    const RK3566SystemVariant *variant = system_get_variant(s);

    s->reg_array = register_init_block32(dev, variant->regs_info,
                                         variant->num_regs, s->regs_info,
                                         s->regs, &system_ops, false,
                                         RK3566_SYSTEM_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->reg_array->mem);
}

static const Property system_properties[] = {
    DEFINE_PROP_BOOL("rng", RK3566SystemState, rng, false),
    DEFINE_PROP_BOOL("pvtm", RK3566SystemState, pvtm, false),
    DEFINE_PROP_UINT32("oscillator-hz", RK3566SystemState, oscillator_hz,
                       84000000),
};

static void system_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = system_realize;
    device_class_set_props(dc, system_properties);
}

static const TypeInfo system_info = {
    .name = TYPE_RK3566_SYSTEM, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566SystemState),
    .class_init = system_class_init,
};

static void system_register(void) { type_register_static(&system_info); }
type_init(system_register)
