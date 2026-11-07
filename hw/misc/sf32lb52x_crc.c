/*
 * SiFli SF32LB52X CRC
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The CRC1/CRC2 block is a programmable CRC engine:
 *  - CR selects the polynomial size (7/8/16/32 bit), the number of valid
 *    input bytes per DR write (1..4), the input bit reversal granularity
 *    (byte/half-word/word) and output bit reversal;
 *  - INIT is loaded into the datapath when CR.RESET is set;
 *  - writing DR feeds the selected number of low bytes into the engine,
 *    reading DR returns the current result;
 *  - SR.DONE is set after the data written to DR has been consumed.
 *
 * Register behavior follows:
 *  - SVD "SF32LB52x v1.0", peripheral CRC1 @ 0x50048000 (DR/SR/CR/RSVD1/
 *    INIT/POL; CR.RESET is self-clearing, SR.DONE is cleared by a DR write);
 *  - vendor HAL cmsis/Include/crc.h (field positions/masks) and
 *    hal/src/bf0_hal_crc.c (init and data-feed sequences);
 *  - Zephyr drivers/crc/crc_sf32lb.c (programming order, DATASIZE encoding,
 *    SR.DONE polling, DR read-back).
 *
 * DR offsets:
 *   DR    0x00 RW  data in / result out
 *   SR    0x04 RO  bit0 DONE, bit1 OVERFLOW
 *   CR    0x08 RW  bit0 RESET (self-clearing), [2:1] DATASIZE,
 *                  [4:3] POLYSIZE, [6:5] REV_IN, bit7 REV_OUT
 *   RSVD1 0x0c reserved
 *   INIT  0x10 RW
 *   POL   0x14 RW
 *
 * The CRC is bit-serial (1 byte per HCLK cycle) because the polynomial is
 * guest-programmable; no table-driven shortcut is possible.
 */

#include "qemu/osdep.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sf32lb52x_crc.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

REG32(DR, 0x00)
REG32(SR, 0x04)
    FIELD(SR, DONE, 0, 1)
    FIELD(SR, OVERFLOW, 1, 1)
REG32(CR, 0x08)
    FIELD(CR, RESET, 0, 1)
    FIELD(CR, DATASIZE, 1, 2)
    FIELD(CR, POLYSIZE, 3, 2)
    FIELD(CR, REV_IN, 5, 2)
    FIELD(CR, REV_OUT, 7, 1)
REG32(RSVD1, 0x0c)
REG32(INIT, 0x10)
REG32(POL, 0x14)

#define SF32LB52X_CRC_CR_WRITABLE_MASK 0x000000ff

static unsigned int sf32lb52x_crc_width(uint32_t cr)
{
    static const unsigned int width[] = { 32, 16, 8, 7 };

    return width[FIELD_EX32(cr, CR, POLYSIZE)];
}

static uint32_t sf32lb52x_crc_bits_mask(unsigned int nbits)
{
    return nbits >= 32 ? UINT32_MAX : (1U << nbits) - 1;
}

static uint32_t sf32lb52x_crc_reverse_bits(uint32_t value, unsigned int nbits)
{
    uint32_t ret = 0;
    unsigned int i;

    for (i = 0; i < nbits; i++) {
        ret = (ret << 1) | (value & 1U);
        value >>= 1;
    }

    return ret;
}

/*
 * Apply CR.REV_IN to the valid low bytes of a DR write. The reversal is done
 * per byte (REV_IN=1), per half-word (REV_IN=2) or per word (REV_IN=3); a
 * trailing partial unit is reversed over its own width.
 */
static uint32_t sf32lb52x_crc_reverse_input(uint32_t data,
                                            unsigned int nbytes,
                                            unsigned int unit)
{
    unsigned int i;

    for (i = 0; i < nbytes; i += unit) {
        unsigned int bytes = MIN(unit, nbytes - i);
        unsigned int nbits = bytes * 8;
        uint32_t mask = sf32lb52x_crc_bits_mask(nbits);
        uint32_t field = (data >> (8 * i)) & mask;

        field = sf32lb52x_crc_reverse_bits(field, nbits);
        data = (data & ~(mask << (8 * i))) | (field << (8 * i));
    }

    return data & sf32lb52x_crc_bits_mask(nbytes * 8);
}

static void sf32lb52x_crc_feed(SF32LB52XCrcState *s, uint32_t value)
{
    uint32_t cr = s->regs[R_CR];
    unsigned int width = sf32lb52x_crc_width(cr);
    uint32_t mask = sf32lb52x_crc_bits_mask(width);
    uint32_t poly = s->regs[R_POL] & mask;
    unsigned int nbytes = FIELD_EX32(cr, CR, DATASIZE) + 1;
    unsigned int rev_in = FIELD_EX32(cr, CR, REV_IN);
    uint32_t data = value & sf32lb52x_crc_bits_mask(nbytes * 8);
    uint32_t crc = s->crc & mask;
    unsigned int i, bit;

    if (rev_in) {
        data = sf32lb52x_crc_reverse_input(data, nbytes, 1U << (rev_in - 1));
    }

    for (i = 0; i < nbytes; i++) {
        uint8_t byte = (data >> (8 * i)) & 0xff;

        for (bit = 0; bit < 8; bit++) {
            unsigned int din = (byte >> (7 - bit)) & 1;
            unsigned int top = (crc >> (width - 1)) & 1;

            crc = (crc << 1) & mask;
            if (top ^ din) {
                crc ^= poly;
            }
        }
    }

    s->crc = crc & mask;
}

static void sf32lb52x_crc_dr_post_write(RegisterInfo *reg, uint64_t val)
{
    SF32LB52XCrcState *s = SF32LB52X_CRC(reg->opaque);
    uint32_t sr;

    if (s->resetting) {
        return;
    }

    sr = s->regs[R_SR] & ~(uint32_t)R_SR_DONE_MASK;
    /*
     * The engine consumes one byte per HCLK cycle; in a synchronous model the
     * data is always consumed by the time the write returns. OVERFLOW (new
     * data while the previous word is still pending) is therefore not
     * reachable and stays clear.
     */
    s->regs[R_SR] = sr;
    sf32lb52x_crc_feed(s, (uint32_t)val);
    s->regs[R_SR] |= R_SR_DONE_MASK;
}

static uint64_t sf32lb52x_crc_dr_post_read(RegisterInfo *reg, uint64_t val)
{
    SF32LB52XCrcState *s = SF32LB52X_CRC(reg->opaque);
    uint32_t cr = s->regs[R_CR];
    unsigned int width = sf32lb52x_crc_width(cr);
    uint32_t out = s->crc & sf32lb52x_crc_bits_mask(width);

    if (FIELD_EX32(cr, CR, REV_OUT)) {
        out = sf32lb52x_crc_reverse_bits(out, width);
    }

    return out;
}

static void sf32lb52x_crc_cr_post_write(RegisterInfo *reg, uint64_t val)
{
    SF32LB52XCrcState *s = SF32LB52X_CRC(reg->opaque);
    uint32_t cr = (uint32_t)val;
    unsigned int width;

    if (s->resetting) {
        return;
    }

    if (!FIELD_EX32(cr, CR, RESET)) {
        return;
    }

    /* CR.RESET loads the datapath from INIT and clears itself. */
    width = sf32lb52x_crc_width(cr);
    s->crc = s->regs[R_INIT] & sf32lb52x_crc_bits_mask(width);
    s->regs[R_CR] &= ~(uint32_t)R_CR_RESET_MASK;
}

static const RegisterAccessInfo sf32lb52x_crc_regs_info[] = {
    {
        .name = "DR",
        .addr = A_DR,
        .post_write = sf32lb52x_crc_dr_post_write,
        .post_read = sf32lb52x_crc_dr_post_read,
    }, {
        .name = "SR",
        .addr = A_SR,
        /*
         * DONE/OVERFLOW are hardware-owned. The SVD marks SR read-write but
         * documents no software clear semantics, and neither the vendor HAL
         * nor the Zephyr driver ever writes it; writes are logged as
         * unimplemented and have no effect.
         */
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
    }, {
        .name = "CR",
        .addr = A_CR,
        .rsvd = ~(uint64_t)SF32LB52X_CRC_CR_WRITABLE_MASK,
        .post_write = sf32lb52x_crc_cr_post_write,
    }, {
        .name = "RSVD1",
        .addr = A_RSVD1,
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
    }, {
        .name = "INIT",
        .addr = A_INIT,
    }, {
        .name = "POL",
        .addr = A_POL,
    },
};

static const MemoryRegionOps sf32lb52x_crc_regs_ops = {
    .read = register_read_memory,
    .write = register_write_memory,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void sf32lb52x_crc_reset_enter(Object *obj, ResetType type)
{
    SF32LB52XCrcState *s = SF32LB52X_CRC(obj);

    s->resetting = true;
}

static void sf32lb52x_crc_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XCrcState *s = SF32LB52X_CRC(obj);
    int i;

    for (i = 0; i < s->reg_array->num_elements; i++) {
        register_reset(s->reg_array->r[i]);
    }
    s->crc = 0;
}

static void sf32lb52x_crc_reset_exit(Object *obj, ResetType type)
{
    SF32LB52XCrcState *s = SF32LB52X_CRC(obj);

    s->resetting = false;
}

static void sf32lb52x_crc_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XCrcState *s = SF32LB52X_CRC(dev);

    s->reg_array = register_init_block32(dev, sf32lb52x_crc_regs_info,
                                         ARRAY_SIZE(sf32lb52x_crc_regs_info),
                                         s->regs_info, s->regs,
                                         &sf32lb52x_crc_regs_ops, false,
                                         SF32LB52X_CRC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->reg_array->mem);
}

static const VMStateDescription vmstate_sf32lb52x_crc = {
    .name = TYPE_SF32LB52X_CRC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, SF32LB52XCrcState, SF32LB52X_CRC_R_MAX),
        VMSTATE_UINT32(crc, SF32LB52XCrcState),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb52x_crc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.enter = sf32lb52x_crc_reset_enter;
    rc->phases.hold = sf32lb52x_crc_reset_hold;
    rc->phases.exit = sf32lb52x_crc_reset_exit;
    dc->realize = sf32lb52x_crc_realize;
    dc->desc = "SiFli SF32LB52X CRC";
    dc->vmsd = &vmstate_sf32lb52x_crc;
    dc->user_creatable = false;
}

static const TypeInfo sf32lb52x_crc_types[] = {
    {
        .name = TYPE_SF32LB52X_CRC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XCrcState),
        .class_init = sf32lb52x_crc_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_crc_types)
