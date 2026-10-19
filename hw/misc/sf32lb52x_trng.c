/*
 * SiFli SF32LB52X true random number generator
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sf32lb52x_trng.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void sf32lb52x_trng_update_irq(SF32LB52XTrngState *s)
{
    qemu_set_irq(s->irq, (s->raw_isr & ~s->isr_mask) != 0);
}

static uint64_t sf32lb52x_trng_next(SF32LB52XTrngState *s)
{
    /*
     * xorshift64*: a small deterministic PRNG so the model is reproducible
     * across runs and can be validated by qtests.
     */
    uint64_t x = s->prng_state;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s->prng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void sf32lb52x_trng_generate_seed(SF32LB52XTrngState *s, bool external)
{
    int i;

    for (i = 0; i < SF32LB52X_TRNG_RAND_NUM; i++) {
        if (!external) {
            s->seed[i] = (uint32_t)sf32lb52x_trng_next(s);
        }
        s->prng_state ^= (uint64_t)s->seed[i] << (i * 8);
    }
    s->seed_valid = true;
    s->raw_isr |= SF32LB52X_TRNG_ISR_SEED_GEN_DONE;
    sf32lb52x_trng_update_irq(s);
}

static void sf32lb52x_trng_generate_rnd(SF32LB52XTrngState *s)
{
    int i;

    if (!s->seed_valid) {
        sf32lb52x_trng_generate_seed(s, false);
    }
    for (i = 0; i < SF32LB52X_TRNG_RAND_NUM; i++) {
        s->rnd[i] = (uint32_t)sf32lb52x_trng_next(s);
    }
    s->rnd_valid = true;
    s->raw_isr |= SF32LB52X_TRNG_ISR_RAND_NUM_AVAIL;
    sf32lb52x_trng_update_irq(s);
}

static uint64_t sf32lb52x_trng_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    SF32LB52XTrngState *s = opaque;
    uint32_t value;

    if (addr >= SF32LB52X_TRNG_SEED0 && addr < SF32LB52X_TRNG_RAND0 &&
        ((addr - SF32LB52X_TRNG_SEED0) % 4) == 0) {
        return s->seed[(addr - SF32LB52X_TRNG_SEED0) / 4];
    }
    if (addr >= SF32LB52X_TRNG_RAND0 && addr < SF32LB52X_TRNG_CAL_CFG &&
        ((addr - SF32LB52X_TRNG_RAND0) % 4) == 0) {
        return s->rnd[(addr - SF32LB52X_TRNG_RAND0) / 4];
    }

    switch (addr) {
    case SF32LB52X_TRNG_CTRL:
        value = s->ctrl;
        break;
    case SF32LB52X_TRNG_STAT:
        value = (s->seed_valid ? SF32LB52X_TRNG_STAT_SEED_VALID : 0) |
                (s->rnd_valid ? SF32LB52X_TRNG_STAT_RAND_VALID : 0);
        break;
    case SF32LB52X_TRNG_CFG:
        value = s->cfg;
        break;
    case SF32LB52X_TRNG_ISR:
        value = (s->raw_isr & SF32LB52X_TRNG_ISR_RAW_MASK) |
                (s->isr_mask << SF32LB52X_TRNG_ISR_MASK_SHIFT);
        break;
    case SF32LB52X_TRNG_CAL_CFG:
        value = s->cal_cfg;
        break;
    case SF32LB52X_TRNG_CAL_RESULT:
        value = s->cal_result;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        value = 0;
        break;
    }

    return value;
}

static void sf32lb52x_trng_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned int size)
{
    SF32LB52XTrngState *s = opaque;
    uint32_t val = value;

    if (addr >= SF32LB52X_TRNG_SEED0 && addr < SF32LB52X_TRNG_RAND0 &&
        ((addr - SF32LB52X_TRNG_SEED0) % 4) == 0) {
        s->seed[(addr - SF32LB52X_TRNG_SEED0) / 4] = val;
        if (s->cfg & SF32LB52X_TRNG_CFG_USE_EXT_SEED) {
            s->seed_valid = false;
        }
        return;
    }

    switch (addr) {
    case SF32LB52X_TRNG_CTRL:
        if (val & (SF32LB52X_TRNG_CTRL_SEED_STOP |
                   SF32LB52X_TRNG_CTRL_RAND_STOP |
                   SF32LB52X_TRNG_CTRL_RAND_SUSPEND)) {
            qemu_log_mask(LOG_UNIMP,
                          "%s: seed/random stop/suspend not modeled\n",
                          __func__);
        }
        s->ctrl = val & ~(SF32LB52X_TRNG_CTRL_SEED_START |
                          SF32LB52X_TRNG_CTRL_RAND_START |
                          SF32LB52X_TRNG_CTRL_SEED_STOP |
                          SF32LB52X_TRNG_CTRL_RAND_STOP |
                          SF32LB52X_TRNG_CTRL_RAND_SUSPEND);
        if (val & SF32LB52X_TRNG_CTRL_SEED_START) {
            sf32lb52x_trng_generate_seed(s,
                (s->cfg & SF32LB52X_TRNG_CFG_USE_EXT_SEED) != 0);
        }
        if (val & SF32LB52X_TRNG_CTRL_RAND_START) {
            sf32lb52x_trng_generate_rnd(s);
        }
        break;
    case SF32LB52X_TRNG_CFG:
        s->cfg = val;
        break;
    case SF32LB52X_TRNG_ISR:
        s->raw_isr &= ~(val & SF32LB52X_TRNG_ISR_RAW_MASK);
        s->isr_mask = (val >> SF32LB52X_TRNG_ISR_MASK_SHIFT) &
                      SF32LB52X_TRNG_ISR_RAW_MASK;
        sf32lb52x_trng_update_irq(s);
        break;
    case SF32LB52X_TRNG_CAL_CFG:
        s->cal_cfg = val;
        if (val & SF32LB52X_TRNG_CAL_ENABLE) {
            /*
             * The oscillator calibration flow is not modeled: the
             * hardware counters have no local evidence. Keep the
             * request visible as unimplemented instead of inventing a
             * calibration result.
             */
            qemu_log_mask(LOG_UNIMP,
                          "%s: oscillator calibration not modeled\n",
                          __func__);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps sf32lb52x_trng_ops = {
    .read = sf32lb52x_trng_read,
    .write = sf32lb52x_trng_write,
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

static void sf32lb52x_trng_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XTrngState *s = SF32LB52X_TRNG(obj);

    s->ctrl = 0;
    s->cfg = 0;
    s->raw_isr = 0;
    s->isr_mask = SF32LB52X_TRNG_ISR_RAW_MASK;
    memset(s->seed, 0, sizeof(s->seed));
    memset(s->rnd, 0, sizeof(s->rnd));
    s->cal_cfg = 0;
    s->cal_result = 0;
    s->seed_valid = false;
    s->rnd_valid = false;
    s->prng_state = 0x9e3779b97f4a7c15ULL;
    sf32lb52x_trng_update_irq(s);
}

static void sf32lb52x_trng_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XTrngState *s = SF32LB52X_TRNG(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sf32lb52x_trng_ops, s,
                          TYPE_SF32LB52X_TRNG, SF32LB52X_TRNG_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_sf32lb52x_trng = {
    .name = TYPE_SF32LB52X_TRNG,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(ctrl, SF32LB52XTrngState),
        VMSTATE_UINT32(cfg, SF32LB52XTrngState),
        VMSTATE_UINT32(raw_isr, SF32LB52XTrngState),
        VMSTATE_UINT32(isr_mask, SF32LB52XTrngState),
        VMSTATE_UINT32_ARRAY(seed, SF32LB52XTrngState, SF32LB52X_TRNG_RAND_NUM),
        VMSTATE_UINT32_ARRAY(rnd, SF32LB52XTrngState, SF32LB52X_TRNG_RAND_NUM),
        VMSTATE_UINT32(cal_cfg, SF32LB52XTrngState),
        VMSTATE_UINT32(cal_result, SF32LB52XTrngState),
        VMSTATE_BOOL(seed_valid, SF32LB52XTrngState),
        VMSTATE_BOOL(rnd_valid, SF32LB52XTrngState),
        VMSTATE_UINT64(prng_state, SF32LB52XTrngState),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb52x_trng_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_trng_reset_hold;
    dc->realize = sf32lb52x_trng_realize;
    dc->desc = "SiFli SF32LB52X TRNG";
    dc->vmsd = &vmstate_sf32lb52x_trng;
    dc->user_creatable = false;
}

static const TypeInfo sf32lb52x_trng_types[] = {
    {
        .name = TYPE_SF32LB52X_TRNG,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XTrngState),
        .class_init = sf32lb52x_trng_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_trng_types);
