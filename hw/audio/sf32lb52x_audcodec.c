/*
 * SiFli SF32LB52X AUDIO codec
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register layout follows the SF32LB52x SVD AUDCODEC block (0x00..0xd8, ID
 * reset value 0xC0DEC000, all other registers reset to zero) and the vendor
 * HAL cmsis/sf32lb52x/audcodec.h definitions; behavior follows the Zephyr
 * drivers/audio/sf32lb_codec.c init/PLL/configure/start flows.
 *
 * Model scope (register/behaviour shim; no real PCM):
 *  - control/status register bank via RegisterInfo (0x00..0xd8);
 *  - ID is a read-only constant 0xC0DEC000;
 *  - IRQ/IRQ_MSK: raw status bits 0..5 (DAC) and 16..21 (ADC), IRQ is
 *    write-1-to-clear; IRQ_MSK is plain storage. The codec IRQ output is NOT
 *    wired by the SoC/DT (no NVIC line), so masking has no external effect;
 *  - APB data path at 0x40..0x57 is a manual MemoryRegionOps aperture:
 *    DAC_CHn_ENTRY writes push a 16-entry TX FIFO (overflow sets
 *    DAC_CHn_APB_OF), ADC_CHn_ENTRY reads pop a 16-entry RX FIFO (underflow
 *    sets ADC_CHn_APB_UF). As a modelling approximation the DAC data is also
 *    mirrored into the ADC RX FIFO (loopback) so the read path is testable;
 *    APB_STAT reports the four FIFO counts;
 *  - PLL: PLL_CAL_CFG.DONE always reads 1 and PLL_STAT.UNLOCK always reads 0.
 *    This terminates the two unbounded waits in the Zephyr driver
 *    (wait_pll_done, and the do/while on pll_update_freq) for a guest build
 *    that enables the audio codec; PLL_CAL_RESULT reports fixed calibration
 *    counters (PLL_CNT=1918, XTAL_CNT=2000) that keep adjust_pll_vco bounded;
 *  - analog/PLL/bandgap tuning registers are storage-only with LOG_UNIMP on
 *    write (no analog behaviour is modelled);
 *  - no real audio stream is produced or consumed.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/audio/sf32lb52x_audcodec.h"
#include "hw/core/register.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

REG32(AUDCODEC_ID, 0x00)
REG32(AUDCODEC_CFG, 0x04)
    FIELD(AUDCODEC_CFG, ADC_ENABLE, 0, 1)
    FIELD(AUDCODEC_CFG, DAC_ENABLE, 1, 1)
    FIELD(AUDCODEC_CFG, DAC_1K_MODE, 2, 1)
    FIELD(AUDCODEC_CFG, ADC_EN_DLY_SEL, 3, 2)
REG32(AUDCODEC_IRQ, 0x08)
REG32(AUDCODEC_IRQ_MSK, 0x0c)
REG32(AUDCODEC_DAC_CFG, 0x10)
REG32(AUDCODEC_ADC_CFG, 0x14)
REG32(AUDCODEC_APB_STAT, 0x18)
    FIELD(AUDCODEC_APB_STAT, DAC_CH0_FIFO_CNT, 0, 4)
    FIELD(AUDCODEC_APB_STAT, DAC_CH1_FIFO_CNT, 4, 4)
    FIELD(AUDCODEC_APB_STAT, ADC_CH0_FIFO_CNT, 16, 4)
    FIELD(AUDCODEC_APB_STAT, ADC_CH1_FIFO_CNT, 20, 4)
REG32(AUDCODEC_ADC_CH0_CFG, 0x20)
REG32(AUDCODEC_ADC_CH1_CFG, 0x24)
REG32(AUDCODEC_DAC_CH0_CFG, 0x30)
REG32(AUDCODEC_DAC_CH0_CFG_EXT, 0x34)
REG32(AUDCODEC_DAC_CH1_CFG, 0x38)
REG32(AUDCODEC_DAC_CH1_CFG_EXT, 0x3c)
REG32(AUDCODEC_DAC_CH0_DEBUG, 0x58)
REG32(AUDCODEC_DAC_CH1_DEBUG, 0x5c)
REG32(AUDCODEC_DAC_CH0_DC, 0x60)
REG32(AUDCODEC_DAC_CH1_DC, 0x64)
REG32(AUDCODEC_COMMON_CFG, 0x70)
REG32(AUDCODEC_BG_CFG0, 0x74)
REG32(AUDCODEC_BG_CFG1, 0x78)
REG32(AUDCODEC_BG_CFG2, 0x7c)
REG32(AUDCODEC_REFGEN_CFG, 0x80)
REG32(AUDCODEC_PLL_CFG0, 0x84)
REG32(AUDCODEC_PLL_CFG1, 0x88)
REG32(AUDCODEC_PLL_CFG2, 0x8c)
REG32(AUDCODEC_PLL_CFG3, 0x90)
REG32(AUDCODEC_PLL_CFG4, 0x94)
REG32(AUDCODEC_PLL_CFG5, 0x98)
REG32(AUDCODEC_PLL_CFG6, 0x9c)
REG32(AUDCODEC_PLL_STAT, 0xa0)
REG32(AUDCODEC_PLL_CAL_CFG, 0xa4)
    FIELD(AUDCODEC_PLL_CAL_CFG, EN, 0, 1)
    FIELD(AUDCODEC_PLL_CAL_CFG, DONE, 1, 1)
    FIELD(AUDCODEC_PLL_CAL_CFG, LEN, 16, 16)
REG32(AUDCODEC_PLL_CAL_RESULT, 0xa8)
REG32(AUDCODEC_ADC_ANA_CFG, 0xac)
REG32(AUDCODEC_ADC1_CFG1, 0xb0)
REG32(AUDCODEC_ADC1_CFG2, 0xb4)
REG32(AUDCODEC_ADC2_CFG1, 0xb8)
REG32(AUDCODEC_ADC2_CFG2, 0xbc)
REG32(AUDCODEC_DAC1_CFG, 0xc0)
REG32(AUDCODEC_DAC2_CFG, 0xc4)

/* APB data path aperture: 0x40..0x57, shadowing the register bank. */
#define A_AUDCODEC_ADC_CH0_ENTRY 0x40
#define A_AUDCODEC_ENTRY_SIZE    0x18

enum {
    ENTRY_ADC_CH0 = 0x00,
    ENTRY_ADC_CH1 = 0x04,
    ENTRY_RSVD3_0 = 0x08,
    ENTRY_RSVD3_1 = 0x0c,
    ENTRY_DAC_CH0 = 0x10,
    ENTRY_DAC_CH1 = 0x14,
};

/* IRQ raw bits: DAC ch0/ch1 [5:0], ADC ch0/ch1 [21:16] (SVD). */
#define AUDCODEC_IRQ_DAC_MASK 0x0000003fU
#define AUDCODEC_IRQ_ADC_MASK 0x003f0000U
#define AUDCODEC_IRQ_W1C_MASK (AUDCODEC_IRQ_DAC_MASK | AUDCODEC_IRQ_ADC_MASK)

#define AUDCODEC_IRQ_DAC_APB_OF(ch) (1U << (3 * (ch) + 0))
#define AUDCODEC_IRQ_DAC_OUT_UF(ch) (1U << (3 * (ch) + 1))
#define AUDCODEC_IRQ_DAC_STB_OF(ch) (1U << (3 * (ch) + 2))
#define AUDCODEC_IRQ_ADC_APB_OF(ch) (1U << (16 + 3 * (ch) + 0))
#define AUDCODEC_IRQ_ADC_APB_UF(ch) (1U << (16 + 3 * (ch) + 1))
#define AUDCODEC_IRQ_ADC_SAT(ch)    (1U << (16 + 3 * (ch) + 2))

/* Fixed calibration result reported to the guest (see file header). */
#define AUDCODEC_PLL_CNT  1918U
#define AUDCODEC_XTAL_CNT 2000U

static void audcodec_raise_irq(SF32LB52XAudCodecState *s, uint32_t bits)
{
    s->regs[R_AUDCODEC_IRQ] |= bits;
}

static void audcodec_dac_push(SF32LB52XAudCodecState *s, int ch, uint32_t value)
{
    s->dac_last[ch] = value;

    if (s->dac_fifo_len[ch] < SF32LB52X_AUDCODEC_FIFO_DEPTH) {
        s->dac_fifo[ch][s->dac_fifo_len[ch]++] = value;
    } else {
        audcodec_raise_irq(s, AUDCODEC_IRQ_DAC_APB_OF(ch));
    }

    /* Loopback approximation: mirror DAC data into the ADC RX FIFO. */
    if (s->adc_fifo_len[ch] < SF32LB52X_AUDCODEC_FIFO_DEPTH) {
        s->adc_fifo[ch][s->adc_fifo_len[ch]++] = value;
    } else {
        audcodec_raise_irq(s, AUDCODEC_IRQ_ADC_APB_OF(ch));
    }
}

static uint32_t audcodec_adc_pop(SF32LB52XAudCodecState *s, int ch)
{
    uint32_t value;

    if (s->adc_fifo_len[ch] == 0) {
        audcodec_raise_irq(s, AUDCODEC_IRQ_ADC_APB_UF(ch));
        return 0;
    }

    value = s->adc_fifo[ch][0];
    s->adc_fifo_len[ch]--;
    memmove(&s->adc_fifo[ch][0], &s->adc_fifo[ch][1],
            s->adc_fifo_len[ch] * sizeof(uint32_t));

    return value;
}

static uint64_t audcodec_entry_read(void *opaque, hwaddr addr,
                                    unsigned int size)
{
    SF32LB52XAudCodecState *s = opaque;

    switch (addr) {
    case ENTRY_ADC_CH0:
        return audcodec_adc_pop(s, 0);
    case ENTRY_ADC_CH1:
        return audcodec_adc_pop(s, 1);
    case ENTRY_DAC_CH0:
        return s->dac_last[0];
    case ENTRY_DAC_CH1:
        return s->dac_last[1];
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }
}

static void audcodec_entry_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned int size)
{
    SF32LB52XAudCodecState *s = opaque;

    switch (addr) {
    case ENTRY_DAC_CH0:
        audcodec_dac_push(s, 0, (uint32_t)value);
        break;
    case ENTRY_DAC_CH1:
        audcodec_dac_push(s, 1, (uint32_t)value);
        break;
    case ENTRY_ADC_CH0:
    case ENTRY_ADC_CH1:
        qemu_log_mask(LOG_UNIMP,
                      "%s: ADC CH%d entry is an ADC data source; guest "
                      "writes have no effect\n", __func__,
                      addr == ENTRY_ADC_CH0 ? 0 : 1);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad offset 0x%" HWADDR_PRIx "\n", __func__, addr);
        break;
    }
}

static const MemoryRegionOps audcodec_entry_ops = {
    .read = audcodec_entry_read,
    .write = audcodec_entry_write,
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

static uint64_t audcodec_apb_stat_post_read(RegisterInfo *reg, uint64_t val)
{
    SF32LB52XAudCodecState *s = SF32LB52X_AUDCODEC(reg->opaque);
    uint32_t stat = 0;

    /*
     * The APB_STAT count fields are 4 bits wide, so a full 16-entry FIFO
     * saturates at 0xf (the overflow flag reports the extra entry).
     */
    stat = FIELD_DP32(stat, AUDCODEC_APB_STAT, DAC_CH0_FIFO_CNT,
                      MIN(s->dac_fifo_len[0], (uint32_t)0xf));
    stat = FIELD_DP32(stat, AUDCODEC_APB_STAT, DAC_CH1_FIFO_CNT,
                      MIN(s->dac_fifo_len[1], (uint32_t)0xf));
    stat = FIELD_DP32(stat, AUDCODEC_APB_STAT, ADC_CH0_FIFO_CNT,
                      MIN(s->adc_fifo_len[0], (uint32_t)0xf));
    stat = FIELD_DP32(stat, AUDCODEC_APB_STAT, ADC_CH1_FIFO_CNT,
                      MIN(s->adc_fifo_len[1], (uint32_t)0xf));

    return stat;
}

static uint64_t audcodec_pll_cal_cfg_post_read(RegisterInfo *reg, uint64_t val)
{
    /* Calibration completes instantaneously in the model. */
    return val | R_AUDCODEC_PLL_CAL_CFG_DONE_MASK;
}

static const RegisterAccessInfo audcodec_regs_info[] = {
    {
        .name = "ID",
        .addr = A_AUDCODEC_ID,
        .reset = 0xc0dec000,
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
    }, {
        .name = "CFG",
        .addr = A_AUDCODEC_CFG,
        .rsvd = ~(uint64_t)0x1f,
    }, {
        .name = "IRQ",
        .addr = A_AUDCODEC_IRQ,
        .w1c = AUDCODEC_IRQ_W1C_MASK,
        .rsvd = ~(uint64_t)AUDCODEC_IRQ_W1C_MASK,
    }, {
        .name = "IRQ_MSK",
        .addr = A_AUDCODEC_IRQ_MSK,
        .rsvd = ~(uint64_t)AUDCODEC_IRQ_W1C_MASK,
    }, {
        .name = "DAC_CFG",
        .addr = A_AUDCODEC_DAC_CFG,
    }, {
        .name = "ADC_CFG",
        .addr = A_AUDCODEC_ADC_CFG,
    }, {
        .name = "APB_STAT",
        .addr = A_AUDCODEC_APB_STAT,
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
        .post_read = audcodec_apb_stat_post_read,
    }, {
        .name = "ADC_CH0_CFG",
        .addr = A_AUDCODEC_ADC_CH0_CFG,
    }, {
        .name = "ADC_CH1_CFG",
        .addr = A_AUDCODEC_ADC_CH1_CFG,
    }, {
        .name = "DAC_CH0_CFG",
        .addr = A_AUDCODEC_DAC_CH0_CFG,
    }, {
        .name = "DAC_CH0_CFG_EXT",
        .addr = A_AUDCODEC_DAC_CH0_CFG_EXT,
    }, {
        .name = "DAC_CH1_CFG",
        .addr = A_AUDCODEC_DAC_CH1_CFG,
    }, {
        .name = "DAC_CH1_CFG_EXT",
        .addr = A_AUDCODEC_DAC_CH1_CFG_EXT,
    }, {
        .name = "DAC_CH0_DEBUG",
        .addr = A_AUDCODEC_DAC_CH0_DEBUG,
        .unimp = UINT32_MAX,
    }, {
        .name = "DAC_CH1_DEBUG",
        .addr = A_AUDCODEC_DAC_CH1_DEBUG,
        .unimp = UINT32_MAX,
    }, {
        .name = "DAC_CH0_DC",
        .addr = A_AUDCODEC_DAC_CH0_DC,
        .unimp = UINT32_MAX,
    }, {
        .name = "DAC_CH1_DC",
        .addr = A_AUDCODEC_DAC_CH1_DC,
        .unimp = UINT32_MAX,
    }, {
        .name = "COMMON_CFG",
        .addr = A_AUDCODEC_COMMON_CFG,
        .unimp = UINT32_MAX,
    }, {
        .name = "BG_CFG0",
        .addr = A_AUDCODEC_BG_CFG0,
        .unimp = UINT32_MAX,
    }, {
        .name = "BG_CFG1",
        .addr = A_AUDCODEC_BG_CFG1,
        .unimp = UINT32_MAX,
    }, {
        .name = "BG_CFG2",
        .addr = A_AUDCODEC_BG_CFG2,
        .unimp = UINT32_MAX,
    }, {
        .name = "REFGEN_CFG",
        .addr = A_AUDCODEC_REFGEN_CFG,
        .unimp = UINT32_MAX,
    }, {
        .name = "PLL_CFG0",
        .addr = A_AUDCODEC_PLL_CFG0,
        .unimp = UINT32_MAX,
    }, {
        .name = "PLL_CFG1",
        .addr = A_AUDCODEC_PLL_CFG1,
        .unimp = UINT32_MAX,
    }, {
        .name = "PLL_CFG2",
        .addr = A_AUDCODEC_PLL_CFG2,
        .unimp = UINT32_MAX,
    }, {
        .name = "PLL_CFG3",
        .addr = A_AUDCODEC_PLL_CFG3,
        .unimp = UINT32_MAX,
    }, {
        .name = "PLL_CFG4",
        .addr = A_AUDCODEC_PLL_CFG4,
        .unimp = UINT32_MAX,
    }, {
        .name = "PLL_CFG5",
        .addr = A_AUDCODEC_PLL_CFG5,
        .unimp = UINT32_MAX,
    }, {
        .name = "PLL_CFG6",
        .addr = A_AUDCODEC_PLL_CFG6,
        .unimp = UINT32_MAX,
    }, {
        .name = "PLL_STAT",
        .addr = A_AUDCODEC_PLL_STAT,
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
    }, {
        .name = "PLL_CAL_CFG",
        .addr = A_AUDCODEC_PLL_CAL_CFG,
        .reset = 0x2, /* DONE */
        .rsvd = ~(uint64_t)0xffff0003,
        .post_read = audcodec_pll_cal_cfg_post_read,
    }, {
        .name = "PLL_CAL_RESULT",
        .addr = A_AUDCODEC_PLL_CAL_RESULT,
        .reset = (AUDCODEC_PLL_CNT << 16) | AUDCODEC_XTAL_CNT,
        .ro = UINT32_MAX,
        .unimp = UINT32_MAX,
    }, {
        .name = "ADC_ANA_CFG",
        .addr = A_AUDCODEC_ADC_ANA_CFG,
        .unimp = UINT32_MAX,
    }, {
        .name = "ADC1_CFG1",
        .addr = A_AUDCODEC_ADC1_CFG1,
        .unimp = UINT32_MAX,
    }, {
        .name = "ADC1_CFG2",
        .addr = A_AUDCODEC_ADC1_CFG2,
        .unimp = UINT32_MAX,
    }, {
        .name = "ADC2_CFG1",
        .addr = A_AUDCODEC_ADC2_CFG1,
        .unimp = UINT32_MAX,
    }, {
        .name = "ADC2_CFG2",
        .addr = A_AUDCODEC_ADC2_CFG2,
        .unimp = UINT32_MAX,
    }, {
        .name = "DAC1_CFG",
        .addr = A_AUDCODEC_DAC1_CFG,
        .unimp = UINT32_MAX,
    }, {
        .name = "DAC2_CFG",
        .addr = A_AUDCODEC_DAC2_CFG,
        .unimp = UINT32_MAX,
    },
};

static const MemoryRegionOps audcodec_regs_ops = {
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

static void sf32lb52x_audcodec_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XAudCodecState *s = SF32LB52X_AUDCODEC(obj);
    int i;

    for (i = 0; i < s->reg_array->num_elements; i++) {
        register_reset(s->reg_array->r[i]);
    }

    memset(s->dac_fifo, 0, sizeof(s->dac_fifo));
    memset(s->adc_fifo, 0, sizeof(s->adc_fifo));
    memset(s->dac_fifo_len, 0, sizeof(s->dac_fifo_len));
    memset(s->adc_fifo_len, 0, sizeof(s->adc_fifo_len));
    memset(s->dac_last, 0, sizeof(s->dac_last));
}

static void sf32lb52x_audcodec_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XAudCodecState *s = SF32LB52X_AUDCODEC(dev);

    memory_region_init(&s->mmio, OBJECT(dev),
                       TYPE_SF32LB52X_AUDCODEC ".container",
                       SF32LB52X_AUDCODEC_SIZE);

    s->reg_array = register_init_block32(dev, audcodec_regs_info,
                                         ARRAY_SIZE(audcodec_regs_info),
                                         s->regs_info, s->regs,
                                         &audcodec_regs_ops, false,
                                         SF32LB52X_AUDCODEC_SIZE);
    memory_region_add_subregion(&s->mmio, 0, &s->reg_array->mem);

    memory_region_init_io(&s->entry_mem, OBJECT(dev), &audcodec_entry_ops, s,
                          TYPE_SF32LB52X_AUDCODEC ".entry",
                          A_AUDCODEC_ENTRY_SIZE);
    memory_region_add_subregion_overlap(&s->mmio,
                                        A_AUDCODEC_ADC_CH0_ENTRY,
                                        &s->entry_mem, 1);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const VMStateDescription vmstate_sf32lb52x_audcodec = {
    .name = TYPE_SF32LB52X_AUDCODEC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, SF32LB52XAudCodecState,
                             SF32LB52X_AUDCODEC_R_MAX),
        VMSTATE_UINT32_2DARRAY(dac_fifo, SF32LB52XAudCodecState, 2,
                               SF32LB52X_AUDCODEC_FIFO_DEPTH),
        VMSTATE_UINT32_2DARRAY(adc_fifo, SF32LB52XAudCodecState, 2,
                               SF32LB52X_AUDCODEC_FIFO_DEPTH),
        VMSTATE_UINT32_ARRAY(dac_fifo_len, SF32LB52XAudCodecState, 2),
        VMSTATE_UINT32_ARRAY(adc_fifo_len, SF32LB52XAudCodecState, 2),
        VMSTATE_UINT32_ARRAY(dac_last, SF32LB52XAudCodecState, 2),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb52x_audcodec_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_audcodec_reset_hold;
    dc->realize = sf32lb52x_audcodec_realize;
    dc->desc = "SiFli SF32LB52X AUDIO codec";
    dc->vmsd = &vmstate_sf32lb52x_audcodec;
    dc->user_creatable = false;
}

static const TypeInfo sf32lb52x_audcodec_types[] = {
    {
        .name = TYPE_SF32LB52X_AUDCODEC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XAudCodecState),
        .class_init = sf32lb52x_audcodec_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_audcodec_types)
