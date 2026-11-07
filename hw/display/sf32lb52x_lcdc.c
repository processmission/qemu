/*
 * SiFli SF32LB52X LCDC display controller (LCD_IF block, DBI/SPI path)
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Register layout follows the SF32LB52x SVD LCDC1 block and the vendor HAL
 * LCD_IF_TypeDef definitions; behavior follows the Zephyr mipi_dbi_sf32lb
 * driver.
 *
 * Display output is not rendered. The model implements the register behavior
 * and the DBI/SPI command path (LCD_WR/LCD_SINGLE/LCD_RD/SPI_IF_CONF), keeps
 * every busy bit at zero as required by the driver's unbounded poll loops, and
 * captures command/data/read transactions in a model-side FIFO exposed through
 * QOM properties for qtests.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/display/sf32lb52x_lcdc.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* Register offsets (SVD LCDC1 dump:3-158). */
#define LCDC_COMMAND            0x00
#define LCDC_STATUS             0x04
#define LCDC_IRQ                0x08
#define LCDC_SETTING            0x0c
#define LCDC_CANVAS_TL_POS      0x10
#define LCDC_CANVAS_BR_POS      0x14
#define LCDC_CANVAS_BG          0x18
#define LCDC_LAYER0_CONFIG      0x1c
#define LCDC_LAYER0_TL_POS      0x20
#define LCDC_LAYER0_BR_POS      0x24
#define LCDC_LAYER0_FILTER      0x28
#define LCDC_LAYER0_SRC         0x2c
#define LCDC_LAYER0_FILL        0x30
#define LCDC_LAYER0_DECOMP      0x34
#define LCDC_LAYER0_DECOMP_CFG0 0x38
#define LCDC_LAYER0_DECOMP_CFG1 0x3c
#define LCDC_LAYER0_DECOMP_STAT 0x40
#define LCDC_LAYER1_CONFIG      0x60
#define LCDC_DITHER_CONF        0x78
#define LCDC_DITHER_LFSR        0x7c
#define LCDC_LCD_CONF           0x80
#define LCDC_LCD_IF_CONF        0x84
#define LCDC_LCD_MEM            0x88
#define LCDC_LCD_O_WIDTH        0x8c
#define LCDC_LCD_SINGLE         0x90
#define LCDC_LCD_WR             0x94
#define LCDC_LCD_RD             0x98
#define LCDC_SPI_IF_CONF        0x9c
#define LCDC_TE_CONF            0xa0
#define LCDC_TE_CONF2           0xa4
#define LCDC_STAT_FIRST         0x110
#define LCDC_STAT_LAST          0x124

/* COMMAND */
#define LCDC_COMMAND_START      (1U << 0)
#define LCDC_COMMAND_RESET      (1U << 1)

/* IRQ: masked status bits [0..6] and raw duplicates [16..22]. */
#define LCDC_IRQ_MASKED_Msk     0x7fU
#define LCDC_IRQ_RAW_SHIFT      16
#define LCDC_IRQ_EOF            (1U << 0)

/* SETTING */
#define LCDC_SETTING_WRITE_Msk  0x07ff017fU

/* LCD_SINGLE */
#define LCDC_SINGLE_TYPE        (1U << 0)
#define LCDC_SINGLE_WR_TRIG     (1U << 1)
#define LCDC_SINGLE_RD_TRIG     (1U << 2)
#define LCDC_SINGLE_BUSY        (1U << 3)

/* SPI_IF_CONF */
#define LCDC_SPI_RD_LEN_Pos     20
#define LCDC_SPI_WR_LEN_Pos     22
#define LCDC_SPI_IF_CONF_WRITE_Msk 0x7fffffffU

/* Field masks for the semantic configuration registers. */
#define LCDC_LCD_CONF_WRITE_Msk    0x001fffffU
#define LCDC_LCD_IF_CONF_WRITE_Msk 0x03ffffffU
#define LCDC_TE_CONF_WRITE_Msk     0x001fffffU

/* Reset value of the two documented layer decompression config registers. */
#define LCDC_LAYER0_DECOMP_CFG0_RESET 0x01055982U
#define LCDC_LAYER0_DECOMP_CFG1_RESET 0x80023307U

static unsigned lcdc_spi_len(uint32_t spi_if_conf, unsigned shift)
{
    return ((spi_if_conf >> shift) & 0x3U) + 1U;
}

static uint32_t lcdc_spi_if_conf(SF32LB52XLCDCState *s)
{
    return s->regs[LCDC_SPI_IF_CONF / 4];
}

static void lcdc_reset_state(SF32LB52XLCDCState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[LCDC_LAYER0_DECOMP_CFG0 / 4] = LCDC_LAYER0_DECOMP_CFG0_RESET;
    s->regs[LCDC_LAYER0_DECOMP_CFG1 / 4] = LCDC_LAYER0_DECOMP_CFG1_RESET;
    s->irq_raw = 0;
    s->setting = 0;
    s->last_wr = 0;
    s->lcd_rd = 0;
    s->single_type = 0;
    memset(s->cap_data, 0, sizeof(s->cap_data));
    memset(s->cap_type, 0, sizeof(s->cap_type));
    memset(s->cap_len, 0, sizeof(s->cap_len));
    memset(s->cap_is_read, 0, sizeof(s->cap_is_read));
    s->cap_head = 0;
    s->cap_num = 0;
    s->capture_count = 0;
    s->last_type = 0;
    s->last_data = 0;
    s->last_len = 0;
    s->last_is_read = 0;
    s->read_count = 0;
}

static bool lcdc_addr_defined(hwaddr addr)
{
    if (addr & 3U) {
        return false;
    }
    return addr <= 0x40 ||
           (addr >= 0x60 && addr <= 0x7c) ||
           (addr >= 0x80 && addr <= 0xa4) ||
           (addr >= 0xa8 && addr <= 0xc0) ||
           (addr >= 0xc4 && addr <= 0x100) ||
           (addr >= LCDC_STAT_FIRST && addr <= LCDC_STAT_LAST);
}

static bool lcdc_addr_is_ro(hwaddr addr)
{
    return addr == LCDC_STATUS || addr == LCDC_LAYER0_DECOMP_STAT ||
           (addr >= LCDC_STAT_FIRST && addr <= LCDC_STAT_LAST);
}

static uint32_t lcdc_write_mask(hwaddr addr)
{
    switch (addr) {
    case LCDC_LCD_CONF:
        return LCDC_LCD_CONF_WRITE_Msk;
    case LCDC_LCD_IF_CONF:
        return LCDC_LCD_IF_CONF_WRITE_Msk;
    case LCDC_SPI_IF_CONF:
        return LCDC_SPI_IF_CONF_WRITE_Msk;
    case LCDC_TE_CONF:
        return LCDC_TE_CONF_WRITE_Msk;
    default:
        return 0xffffffffU;
    }
}

/*
 * Record one single-access transaction. The FIFO is model state only: qtest
 * reads it back through the capture-* QOM properties, never through the guest
 * address space (the hardware map has no such aperture).
 */
static void lcdc_capture(SF32LB52XLCDCState *s, bool is_read, unsigned len)
{
    unsigned idx;
    uint32_t data = is_read ? s->lcd_rd : s->last_wr;

    if (s->cap_num < SF32LB52X_LCDC_CAPTURE_FIFO) {
        idx = (s->cap_head + s->cap_num) % SF32LB52X_LCDC_CAPTURE_FIFO;
        s->cap_num++;
    } else {
        idx = s->cap_head;
        s->cap_head = (s->cap_head + 1U) % SF32LB52X_LCDC_CAPTURE_FIFO;
    }

    s->cap_data[idx] = data;
    s->cap_type[idx] = s->single_type;
    s->cap_len[idx] = len;
    s->cap_is_read[idx] = is_read;

    s->capture_count++;
    s->last_type = s->single_type;
    s->last_data = data;
    s->last_len = len;
    s->last_is_read = is_read;
    if (is_read) {
        s->read_count++;
    }
}

static uint64_t lcdc_read(void *opaque, hwaddr addr, unsigned int size)
{
    SF32LB52XLCDCState *s = opaque;
    uint32_t value;

    if (addr >= SF32LB52X_LCDC_SIZE || !lcdc_addr_defined(addr)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    if (addr == LCDC_LCD_RD) {
        if (size == 1) {
            return s->lcd_rd & 0xffU;
        }
        if (size == 2) {
            return s->lcd_rd & 0xffffU;
        }
        return s->lcd_rd;
    }

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid access size %u at 0x%"
                      HWADDR_PRIx "\n", __func__, size, addr);
        return 0;
    }

    switch (addr) {
    case LCDC_COMMAND:
        /* START/RESET are write-1 self-clearing triggers. */
        return 0;
    case LCDC_STATUS:
        /* No rendering engine: LCD_BUSY/DPI_RUN/JDI_PAR_RUN always idle. */
        return 0;
    case LCDC_IRQ:
        value = ((s->irq_raw & s->setting & LCDC_IRQ_MASKED_Msk) |
                 (s->irq_raw << LCDC_IRQ_RAW_SHIFT));
        return value;
    case LCDC_SETTING:
        return s->setting;
    case LCDC_LCD_SINGLE:
        /* TYPE is stored; WR_TRIG/RD_TRIG self-clear; LCD_BUSY always idle. */
        return s->single_type;
    case LCDC_LCD_WR:
        return s->last_wr;
    default:
        return s->regs[addr / 4];
    }
}

static void lcdc_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned int size)
{
    SF32LB52XLCDCState *s = opaque;
    uint32_t val = value;

    if (addr >= SF32LB52X_LCDC_SIZE || !lcdc_addr_defined(addr)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    if (size != 4) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid access size %u at 0x%"
                      HWADDR_PRIx "\n", __func__, size, addr);
        return;
    }

    switch (addr) {
    case LCDC_COMMAND:
        if (val & LCDC_COMMAND_RESET) {
            lcdc_reset_state(s);
            return;
        }
        if (val & LCDC_COMMAND_START) {
            /* No frame engine: the interface block completes immediately. */
            s->irq_raw |= LCDC_IRQ_EOF;
        }
        return;
    case LCDC_STATUS:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: STATUS is read-only\n",
                      __func__);
        return;
    case LCDC_IRQ:
        /* W1C on both the masked and raw status copies. */
        for (unsigned i = 0; i < 7; i++) {
            uint32_t bit = 1U << i;

            if ((val & bit) || (val & (bit << LCDC_IRQ_RAW_SHIFT))) {
                s->irq_raw &= ~bit;
            }
        }
        return;
    case LCDC_SETTING:
        s->setting = val & LCDC_SETTING_WRITE_Msk;
        return;
    case LCDC_LCD_SINGLE:
        s->single_type = val & LCDC_SINGLE_TYPE;
        if (val & LCDC_SINGLE_WR_TRIG) {
            unsigned len = lcdc_spi_len(lcdc_spi_if_conf(s),
                                        LCDC_SPI_WR_LEN_Pos);

            lcdc_capture(s, false, len);
        }
        if (val & LCDC_SINGLE_RD_TRIG) {
            unsigned len = lcdc_spi_len(lcdc_spi_if_conf(s),
                                        LCDC_SPI_RD_LEN_Pos);

            /* No panel attached: the read data register is deterministic 0. */
            s->lcd_rd = 0;
            lcdc_capture(s, true, len);
        }
        return;
    case LCDC_LCD_WR:
        s->last_wr = val;
        s->regs[addr / 4] = val;
        return;
    case LCDC_LCD_RD:
        s->lcd_rd = val;
        return;
    default:
        break;
    }

    if (lcdc_addr_is_ro(addr)) {
        qemu_log_mask(LOG_UNIMP, "%s: write to read-only register 0x%"
                      HWADDR_PRIx "\n", __func__, addr);
        return;
    }

    s->regs[addr / 4] = val & lcdc_write_mask(addr);
}

static const MemoryRegionOps lcdc_ops = {
    .read = lcdc_read,
    .write = lcdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void lcdc_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XLCDCState *s = SF32LB52X_LCDC(obj);

    lcdc_reset_state(s);
}

static void lcdc_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XLCDCState *s = SF32LB52X_LCDC(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &lcdc_ops, s,
                          TYPE_SF32LB52X_LCDC, SF32LB52X_LCDC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    object_property_add_uint32_ptr(OBJECT(dev), "capture-count",
                                   &s->capture_count, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(OBJECT(dev), "last-type",
                                   &s->last_type, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(OBJECT(dev), "last-data",
                                   &s->last_data, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(OBJECT(dev), "last-len",
                                   &s->last_len, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(OBJECT(dev), "last-is-read",
                                   &s->last_is_read, OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(OBJECT(dev), "read-count",
                                   &s->read_count, OBJ_PROP_FLAG_READ);
}

static const VMStateDescription vmstate_sf32lb52x_lcdc = {
    .name = TYPE_SF32LB52X_LCDC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, SF32LB52XLCDCState, 0x400),
        VMSTATE_UINT32(irq_raw, SF32LB52XLCDCState),
        VMSTATE_UINT32(setting, SF32LB52XLCDCState),
        VMSTATE_UINT32(last_wr, SF32LB52XLCDCState),
        VMSTATE_UINT32(lcd_rd, SF32LB52XLCDCState),
        VMSTATE_UINT32(single_type, SF32LB52XLCDCState),
        VMSTATE_UINT32_ARRAY(cap_data, SF32LB52XLCDCState,
                             SF32LB52X_LCDC_CAPTURE_FIFO),
        VMSTATE_UINT8_ARRAY(cap_type, SF32LB52XLCDCState,
                            SF32LB52X_LCDC_CAPTURE_FIFO),
        VMSTATE_UINT8_ARRAY(cap_len, SF32LB52XLCDCState,
                            SF32LB52X_LCDC_CAPTURE_FIFO),
        VMSTATE_UINT8_ARRAY(cap_is_read, SF32LB52XLCDCState,
                            SF32LB52X_LCDC_CAPTURE_FIFO),
        VMSTATE_UINT8(cap_head, SF32LB52XLCDCState),
        VMSTATE_UINT8(cap_num, SF32LB52XLCDCState),
        VMSTATE_UINT32(capture_count, SF32LB52XLCDCState),
        VMSTATE_UINT32(last_type, SF32LB52XLCDCState),
        VMSTATE_UINT32(last_data, SF32LB52XLCDCState),
        VMSTATE_UINT32(last_len, SF32LB52XLCDCState),
        VMSTATE_UINT32(last_is_read, SF32LB52XLCDCState),
        VMSTATE_UINT32(read_count, SF32LB52XLCDCState),
        VMSTATE_END_OF_LIST()
    },
};

static void lcdc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = lcdc_reset_hold;
    dc->realize = lcdc_realize;
    dc->desc = "SiFli SF32LB52X LCDC display controller";
    dc->vmsd = &vmstate_sf32lb52x_lcdc;
    dc->user_creatable = false;
}

static const TypeInfo sf32lb52x_lcdc_types[] = {
    {
        .name = TYPE_SF32LB52X_LCDC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XLCDCState),
        .class_init = lcdc_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_lcdc_types)
