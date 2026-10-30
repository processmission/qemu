/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/irq.h"
#include "hw/core/register.h"

#define TYPE_RK3566_TSADC "rk3566-tsadc"
OBJECT_DECLARE_SIMPLE_TYPE(RK3566TSADCState, RK3566_TSADC)

REG32(CON, 0x00)
REG32(ENABLE, 0x04)
REG32(INT_EN, 0x08)
REG32(INT_STATUS, 0x0c)
REG32(DATA0, 0x20)
REG32(DATA1, 0x24)
REG32(THRESH0, 0x30)
REG32(THRESH1, 0x34)

#define RK3566_TSADC_NUM_WORDS (0x100 / 4)
#define RK3566_TSADC_NUM_REGS (A_THRESH1 / 4 + 1)

struct RK3566TSADCState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    RegisterInfo regs_info[RK3566_TSADC_NUM_REGS];
    uint32_t regs[RK3566_TSADC_NUM_WORDS];

    qemu_irq irq;
    uint32_t temperature_mc;
};

static uint32_t tsadc_code(RK3566TSADCState *s)
{
    /* RK3568 sensor calibration table, 0..125 C in steps of 5 C. */
    const unsigned code[] = {
        1856, 1892, 1924, 1956, 1992, 2024, 2060, 2092, 2128,
        2160, 2196, 2228, 2264, 2300, 2332, 2368, 2400, 2436,
        2468, 2500, 2536, 2572, 2604, 2636, 2672, 2704,
    };
    unsigned temp = MIN(s->temperature_mc, 125000);
    unsigned index = MIN(temp / 5000, ARRAY_SIZE(code) - 2);

    return code[index] +
           (code[index + 1] - code[index]) * (temp - index * 5000) / 5000;
}

static void tsadc_update(RK3566TSADCState *s)
{
    if (s->regs[R_ENABLE] & 1) {
        for (unsigned int channel = 0; channel < 2; channel++) {
            s->regs[R_DATA0 + channel] = tsadc_code(s);
            if ((s->regs[R_INT_EN] & (1 << channel)) &&
                s->regs[R_DATA0 + channel] >=
                s->regs[R_THRESH0 + channel]) {
                s->regs[R_INT_STATUS] |= 1 << channel;
            }
        }
    }
    qemu_set_irq(s->irq, !!(s->regs[R_INT_STATUS] & s->regs[R_INT_EN] & 3));
}

static uint64_t tsadc_data_post_read(RegisterInfo *reg, uint64_t val)
{
    RK3566TSADCState *s = RK3566_TSADC(reg->opaque);

    return (s->regs[R_ENABLE] & 1) ? tsadc_code(s) : val;
}

static void tsadc_post_write(RegisterInfo *reg, uint64_t val)
{
    RK3566TSADCState *s = RK3566_TSADC(reg->opaque);

    tsadc_update(s);
}

static const RegisterAccessInfo tsadc_regs_info[] = {
    {
        .name = "CON",
        .addr = A_CON,
        .post_write = tsadc_post_write,
    }, {
        .name = "ENABLE",
        .addr = A_ENABLE,
        .post_write = tsadc_post_write,
    }, {
        .name = "INT_EN",
        .addr = A_INT_EN,
        .post_write = tsadc_post_write,
    }, {
        .name = "INT_STATUS",
        .addr = A_INT_STATUS,
        .w1c = UINT32_MAX,
        .post_write = tsadc_post_write,
    }, {
        .name = "DATA0",
        .addr = A_DATA0,
        .ro = UINT32_MAX,
        .post_read = tsadc_data_post_read,
    }, {
        .name = "DATA1",
        .addr = A_DATA1,
        .ro = UINT32_MAX,
        .post_read = tsadc_data_post_read,
    }, {
        .name = "THRESH0",
        .addr = A_THRESH0,
        .post_write = tsadc_post_write,
    }, {
        .name = "THRESH1",
        .addr = A_THRESH1,
        .post_write = tsadc_post_write,
    },
};

static bool tsadc_is_modeled_reg(hwaddr addr)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(tsadc_regs_info); i++) {
        if (tsadc_regs_info[i].addr == addr) {
            return true;
        }
    }

    return false;
}

static uint64_t tsadc_read(void *opaque, hwaddr addr, unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566TSADCState *s =
        RK3566_TSADC(register_array_get_owner(reg_array));

    tsadc_update(s);
    if (tsadc_is_modeled_reg(addr)) {
        return register_read_memory(reg_array, addr, size);
    }

    return s->regs[addr / sizeof(uint32_t)];
}

static void tsadc_write(void *opaque, hwaddr addr, uint64_t value,
                        unsigned size)
{
    RegisterInfoArray *reg_array = opaque;
    RK3566TSADCState *s =
        RK3566_TSADC(register_array_get_owner(reg_array));

    if (tsadc_is_modeled_reg(addr)) {
        register_write_memory(reg_array, addr, value, size);
        return;
    }

    s->regs[addr / sizeof(uint32_t)] = value;
    tsadc_update(s);
}

static const MemoryRegionOps tsadc_ops = {
    .read = tsadc_read,
    .write = tsadc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void tsadc_init(Object *obj)
{
    RK3566TSADCState *s = RK3566_TSADC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->reg_array = register_init_block32(DEVICE(obj), tsadc_regs_info,
                                         ARRAY_SIZE(tsadc_regs_info),
                                         s->regs_info, s->regs,
                                         &tsadc_ops, false, 0x100);
    sysbus_init_mmio(sbd, &s->reg_array->mem);
    sysbus_init_irq(sbd, &s->irq);
}
static const Property tsadc_properties[] = {
    DEFINE_PROP_UINT32("temperature-mc", RK3566TSADCState, temperature_mc,
                       25000),
};
static void tsadc_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_props(DEVICE_CLASS(oc), tsadc_properties);
}
static const TypeInfo tsadc_info = {
    .name = TYPE_RK3566_TSADC, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RK3566TSADCState),
    .instance_init = tsadc_init, .class_init = tsadc_class_init,
};
static void tsadc_register(void) { type_register_static(&tsadc_info); }
type_init(tsadc_register)
