/*
 * SiFli SF32LB52X eFuse (OTP) controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sf32lb52x_efuse.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/*
 * The eFuse array contents are per-chip (UID, factory calibration) and are not
 * documented in the local references. The model exposes erased/zero banks
 * until vendor evidence lands; the read handshake is fully modeled.
 */
static uint64_t sf32lb52x_efuse_read(void *opaque, hwaddr addr,
                                     unsigned int size)
{
    SF32LB52XEfuseState *s = opaque;
    uint32_t value;

    if (addr >= SF32LB52X_EFUSE_PGM_DATA(0) &&
        addr < SF32LB52X_EFUSE_PGM_DATA(SF32LB52X_EFUSE_BANK_WORDS)) {
        return s->pgm_data[(addr - SF32LB52X_EFUSE_PGM_DATA(0)) / 4];
    }
    if (addr >= SF32LB52X_EFUSE_BANK_DATA(0, 0) &&
        addr < SF32LB52X_EFUSE_ANACR) {
        uint32_t rel = addr - SF32LB52X_EFUSE_BANK_DATA(0, 0);
        uint32_t bank = rel / 0x20;
        uint32_t word = (rel % 0x20) / 4;

        return s->bank[bank][word];
    }

    switch (addr) {
    case SF32LB52X_EFUSE_CR:
        value = s->cr;
        break;
    case SF32LB52X_EFUSE_TIMR:
        value = s->timr;
        break;
    case SF32LB52X_EFUSE_SR:
        value = s->sr;
        break;
    case SF32LB52X_EFUSE_RSVDR:
        value = s->rsvdr;
        break;
    case SF32LB52X_EFUSE_ANACR:
        value = s->anacr;
        break;
    case SF32LB52X_EFUSE_DB_SEL:
        value = s->db_sel;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        value = 0;
        break;
    }

    return value;
}

static void sf32lb52x_efuse_write(void *opaque, hwaddr addr,
                                  uint64_t value, unsigned int size)
{
    SF32LB52XEfuseState *s = opaque;

    if (addr >= SF32LB52X_EFUSE_PGM_DATA(0) &&
        addr < SF32LB52X_EFUSE_PGM_DATA(SF32LB52X_EFUSE_BANK_WORDS)) {
        s->pgm_data[(addr - SF32LB52X_EFUSE_PGM_DATA(0)) / 4] = value;
        return;
    }
    if (addr >= SF32LB52X_EFUSE_BANK_DATA(0, 0) &&
        addr < SF32LB52X_EFUSE_ANACR) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bank data is read-only\n",
                      __func__);
        return;
    }

    switch (addr) {
    case SF32LB52X_EFUSE_CR:
        /*
         * EN is a self-clearing read command trigger. MODE/BANKSEL/IE stay
         * readable; programming is out of scope and only logged.
         */
        s->cr = (uint32_t)value & ~SF32LB52X_EFUSE_CR_EN;
        if (value & SF32LB52X_EFUSE_CR_EN) {
            if (value & SF32LB52X_EFUSE_CR_MODE) {
                qemu_log_mask(LOG_UNIMP,
                              "%s: eFuse programming not modeled\n", __func__);
            }
            s->sr |= SF32LB52X_EFUSE_SR_DONE;
        }
        break;
    case SF32LB52X_EFUSE_TIMR:
        s->timr = value;
        break;
    case SF32LB52X_EFUSE_SR:
        s->sr &= ~((uint32_t)value & SF32LB52X_EFUSE_SR_DONE);
        break;
    case SF32LB52X_EFUSE_RSVDR:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: reserved register\n", __func__);
        break;
    case SF32LB52X_EFUSE_ANACR:
        s->anacr = value;
        break;
    case SF32LB52X_EFUSE_DB_SEL:
        s->db_sel = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps sf32lb52x_efuse_ops = {
    .read = sf32lb52x_efuse_read,
    .write = sf32lb52x_efuse_write,
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

static void sf32lb52x_efuse_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XEfuseState *s = SF32LB52X_EFUSE(obj);

    s->cr = 0;
    s->timr = 0;
    s->sr = 0;
    s->rsvdr = 0;
    memset(s->pgm_data, 0, sizeof(s->pgm_data));
    memset(s->bank, 0, sizeof(s->bank));
    s->anacr = 0;
    s->db_sel = 0;
}

static void sf32lb52x_efuse_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XEfuseState *s = SF32LB52X_EFUSE(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sf32lb52x_efuse_ops, s,
                          TYPE_SF32LB52X_EFUSE, SF32LB52X_EFUSE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const VMStateDescription vmstate_sf32lb52x_efuse = {
    .name = TYPE_SF32LB52X_EFUSE,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(cr, SF32LB52XEfuseState),
        VMSTATE_UINT32(timr, SF32LB52XEfuseState),
        VMSTATE_UINT32(sr, SF32LB52XEfuseState),
        VMSTATE_UINT32(rsvdr, SF32LB52XEfuseState),
        VMSTATE_UINT32_ARRAY(pgm_data, SF32LB52XEfuseState,
                             SF32LB52X_EFUSE_BANK_WORDS),
        VMSTATE_UINT32_2DARRAY(bank, SF32LB52XEfuseState,
                               SF32LB52X_EFUSE_BANKS,
                               SF32LB52X_EFUSE_BANK_WORDS),
        VMSTATE_UINT32(anacr, SF32LB52XEfuseState),
        VMSTATE_UINT32(db_sel, SF32LB52XEfuseState),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb52x_efuse_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_efuse_reset_hold;
    dc->realize = sf32lb52x_efuse_realize;
    dc->desc = "SiFli SF32LB52X eFuse (OTP) controller";
    dc->vmsd = &vmstate_sf32lb52x_efuse;
    dc->user_creatable = false;
}

static const TypeInfo sf32lb52x_efuse_types[] = {
    {
        .name = TYPE_SF32LB52X_EFUSE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XEfuseState),
        .class_init = sf32lb52x_efuse_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_efuse_types);
