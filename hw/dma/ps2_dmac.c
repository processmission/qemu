/*
 * Sony PlayStation 2 Emotion Engine DMA controller (DMAC)
 *
 * Ten channels move quadwords between main RAM and the EE peripherals.
 * Transfers complete synchronously when the guest sets CHCR.STR: for
 * memory-to-peripheral channels the DMAC walks normal or source-chain
 * mode and hands each block to the attached peripheral.  Peripheral-to-
 * memory channels are started and completed by the peripheral itself.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/dma/ps2_dmac.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/dma.h"

/* Offsets of the channel register blocks from 0x10008000. */
static const hwaddr ps2_dmac_chan_base[PS2_DMAC_NUM_CHANNELS] = {
    [PS2_DMAC_VIF0] = 0x0000,
    [PS2_DMAC_VIF1] = 0x1000,
    [PS2_DMAC_GIF] = 0x2000,
    [PS2_DMAC_FROM_IPU] = 0x3000,
    [PS2_DMAC_TO_IPU] = 0x3400,
    [PS2_DMAC_SIF0] = 0x4000,
    [PS2_DMAC_SIF1] = 0x4400,
    [PS2_DMAC_SIF2] = 0x4800,
    [PS2_DMAC_FROM_SPR] = 0x5000,
    [PS2_DMAC_TO_SPR] = 0x5400,
};

/* Source chain tag IDs. */
enum {
    PS2_DMA_TAG_REFE = 0,
    PS2_DMA_TAG_CNT = 1,
    PS2_DMA_TAG_NEXT = 2,
    PS2_DMA_TAG_REF = 3,
    PS2_DMA_TAG_REFS = 4,
    PS2_DMA_TAG_CALL = 5,
    PS2_DMA_TAG_RET = 6,
    PS2_DMA_TAG_END = 7,
};

/* Upper bound on chain tags per transfer, to stop runaway loops. */
#define PS2_DMAC_MAX_TAGS       65536

static void ps2_dmac_update_irq(PS2DMACState *s)
{
    uint32_t pending = s->stat & (s->stat >> 16) & 0x7fff;

    /* A bus error interrupts regardless of the mask. */
    qemu_set_irq(s->irq, pending || (s->stat & 0x8000));
}

void ps2_dmac_set_channel_ops(PS2DMACState *s, int ch,
                              const PS2DMACChannelOps *ops, void *opaque)
{
    s->ch[ch].ops = ops;
    s->ch[ch].opaque = opaque;
}

bool ps2_dmac_channel_active(PS2DMACState *s, int ch)
{
    return s->ch[ch].chcr & PS2_DMAC_CHCR_STR;
}

void ps2_dmac_channel_done(PS2DMACState *s, int ch)
{
    s->ch[ch].chcr &= ~PS2_DMAC_CHCR_STR;
    s->ch[ch].qwc = 0;
    s->stat |= 1u << ch;
    ps2_dmac_update_irq(s);
}

static void ps2_dmac_bus_error(PS2DMACState *s, int ch, uint32_t addr)
{
    qemu_log_mask(LOG_GUEST_ERROR, "%s: channel %d bus error at 0x%08x\n",
                  __func__, ch, addr);
    s->stat |= 1u << 15;
}

static bool ps2_dmac_send(PS2DMACState *s, int ch, uint32_t addr,
                          uint32_t qwc)
{
    PS2DMACChannel *c = &s->ch[ch];
    uint32_t len = qwc * 16;
    g_autofree uint8_t *buf = NULL;

    if (!qwc) {
        return true;
    }
    if (addr & 0x80000000) {
        qemu_log_mask(LOG_UNIMP, "%s: scratchpad transfers are not "
                      "supported\n", __func__);
        return false;
    }

    buf = g_malloc(len);
    if (dma_memory_read(&address_space_memory, addr & ~0xf, buf, len,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        ps2_dmac_bus_error(s, ch, addr);
        return false;
    }
    if (c->ops && c->ops->sink) {
        c->ops->sink(c->opaque, buf, len);
    }
    return true;
}

static void ps2_dmac_run_chain(PS2DMACState *s, int ch)
{
    PS2DMACChannel *c = &s->ch[ch];

    /* A residual normal-mode block goes first. */
    if (c->qwc) {
        if (!ps2_dmac_send(s, ch, c->madr, c->qwc)) {
            return;
        }
        c->madr += c->qwc * 16;
        c->qwc = 0;
    }

    for (int n = 0; n < PS2_DMAC_MAX_TAGS; n++) {
        uint64_t tag[2];
        uint32_t qwc, id, addr, asp, data;
        bool end = false;

        if (dma_memory_read(&address_space_memory, c->tadr & 0x7ffffff0,
                            tag, sizeof(tag),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            ps2_dmac_bus_error(s, ch, c->tadr);
            return;
        }
        tag[0] = le64_to_cpu(tag[0]);
        qwc = extract64(tag[0], 0, 16);
        id = extract64(tag[0], 28, 3);
        addr = extract64(tag[0], 32, 32) & ~0xf;
        c->chcr = deposit32(c->chcr, 16, 16, extract64(tag[0], 16, 16));
        asp = extract32(c->chcr, PS2_DMAC_CHCR_ASP_SHIFT, 2);

        if ((c->chcr & PS2_DMAC_CHCR_TTE) && c->ops && c->ops->sink) {
            c->ops->sink(c->opaque, (uint8_t *)&tag[1], 8);
        }

        data = c->tadr + 16;
        switch (id) {
        case PS2_DMA_TAG_REFE:
            data = addr;
            c->tadr += 16;
            end = true;
            break;
        case PS2_DMA_TAG_CNT:
            c->tadr = data + qwc * 16;
            break;
        case PS2_DMA_TAG_NEXT:
            c->tadr = addr;
            break;
        case PS2_DMA_TAG_REF:
        case PS2_DMA_TAG_REFS:
            data = addr;
            c->tadr += 16;
            break;
        case PS2_DMA_TAG_CALL:
            if (asp >= 2) {
                ps2_dmac_bus_error(s, ch, c->tadr);
                return;
            }
            c->asr[asp++] = data + qwc * 16;
            c->tadr = addr;
            break;
        case PS2_DMA_TAG_RET:
            if (asp) {
                c->tadr = c->asr[--asp];
            } else {
                end = true;
            }
            break;
        case PS2_DMA_TAG_END:
            end = true;
            break;
        }
        c->chcr = deposit32(c->chcr, PS2_DMAC_CHCR_ASP_SHIFT, 2, asp);

        if (!ps2_dmac_send(s, ch, data, qwc)) {
            return;
        }
        c->madr = data + qwc * 16;

        if ((tag[0] >> 31 & 1) && (c->chcr & PS2_DMAC_CHCR_TIE)) {
            end = true;
        }
        if (end) {
            ps2_dmac_channel_done(s, ch);
            return;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR, "%s: channel %d chain does not end\n",
                  __func__, ch);
    ps2_dmac_bus_error(s, ch, c->tadr);
}

static void ps2_dmac_start(PS2DMACState *s, int ch)
{
    PS2DMACChannel *c = &s->ch[ch];

    if (!(c->chcr & PS2_DMAC_CHCR_DIR)) {
        /* Peripheral to memory: wait for the peripheral to deliver. */
        if (c->ops && c->ops->start) {
            c->ops->start(c->opaque);
        }
        return;
    }

    if ((c->chcr & PS2_DMAC_CHCR_MOD) == PS2_DMAC_CHCR_MOD_CHAIN) {
        ps2_dmac_run_chain(s, ch);
    } else {
        if (ps2_dmac_send(s, ch, c->madr, c->qwc)) {
            c->madr += c->qwc * 16;
            ps2_dmac_channel_done(s, ch);
        }
    }
    ps2_dmac_update_irq(s);
}

static int ps2_dmac_decode(hwaddr addr, hwaddr *reg)
{
    for (int i = 0; i < PS2_DMAC_NUM_CHANNELS; i++) {
        if (addr >= ps2_dmac_chan_base[i] &&
            addr < ps2_dmac_chan_base[i] + 0x100) {
            *reg = addr - ps2_dmac_chan_base[i];
            return i;
        }
    }
    return -1;
}

static uint64_t ps2_dmac_global_read(PS2DMACState *s, hwaddr reg)
{
    switch (reg) {
    case PS2_DMAC_D_CTRL:
        return s->ctrl;
    case PS2_DMAC_D_STAT:
        return s->stat;
    case PS2_DMAC_D_PCR:
        return s->pcr;
    case PS2_DMAC_D_SQWC:
        return s->sqwc;
    case PS2_DMAC_D_RBSR:
        return s->rbsr;
    case PS2_DMAC_D_RBOR:
        return s->rbor;
    case PS2_DMAC_D_STADR:
        return s->stadr;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, reg);
        return 0;
    }
}

static void ps2_dmac_global_write(PS2DMACState *s, hwaddr reg, uint32_t value)
{
    switch (reg) {
    case PS2_DMAC_D_CTRL:
        s->ctrl = value;
        break;
    case PS2_DMAC_D_STAT:
        /* Status bits are write-1-to-clear, mask bits write-1-to-toggle. */
        s->stat &= ~(value & 0xffff);
        s->stat ^= value & 0xffff0000;
        ps2_dmac_update_irq(s);
        break;
    case PS2_DMAC_D_PCR:
        s->pcr = value;
        break;
    case PS2_DMAC_D_SQWC:
        s->sqwc = value;
        break;
    case PS2_DMAC_D_RBSR:
        s->rbsr = value;
        break;
    case PS2_DMAC_D_RBOR:
        s->rbor = value;
        break;
    case PS2_DMAC_D_STADR:
        s->stadr = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, reg);
        break;
    }
}

static uint64_t ps2_dmac_read(void *opaque, hwaddr addr, unsigned int size)
{
    PS2DMACState *s = opaque;
    PS2DMACChannel *c;
    hwaddr reg;
    int ch;

    if (addr >= PS2_DMAC_GLOBAL_BASE) {
        return ps2_dmac_global_read(s, addr - PS2_DMAC_GLOBAL_BASE);
    }

    ch = ps2_dmac_decode(addr, &reg);
    if (ch < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
    c = &s->ch[ch];

    switch (reg) {
    case PS2_DMAC_CHCR:
        return c->chcr;
    case PS2_DMAC_MADR:
        return c->madr;
    case PS2_DMAC_QWC:
        return c->qwc;
    case PS2_DMAC_TADR:
        return c->tadr;
    case PS2_DMAC_ASR0:
        return c->asr[0];
    case PS2_DMAC_ASR1:
        return c->asr[1];
    case PS2_DMAC_SADR:
        return c->sadr;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void ps2_dmac_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned int size)
{
    PS2DMACState *s = opaque;
    PS2DMACChannel *c;
    hwaddr reg;
    int ch;

    if (addr >= PS2_DMAC_GLOBAL_BASE) {
        ps2_dmac_global_write(s, addr - PS2_DMAC_GLOBAL_BASE, value);
        return;
    }

    ch = ps2_dmac_decode(addr, &reg);
    if (ch < 0) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }
    c = &s->ch[ch];

    switch (reg) {
    case PS2_DMAC_CHCR: {
        bool was_active = c->chcr & PS2_DMAC_CHCR_STR;

        c->chcr = value;
        if (!was_active && (c->chcr & PS2_DMAC_CHCR_STR)) {
            ps2_dmac_start(s, ch);
        }
        break;
    }
    case PS2_DMAC_MADR:
        c->madr = value & ~0xf;
        break;
    case PS2_DMAC_QWC:
        c->qwc = value & 0xffff;
        break;
    case PS2_DMAC_TADR:
        c->tadr = value & ~0xf;
        break;
    case PS2_DMAC_ASR0:
        c->asr[0] = value & ~0xf;
        break;
    case PS2_DMAC_ASR1:
        c->asr[1] = value & ~0xf;
        break;
    case PS2_DMAC_SADR:
        c->sadr = value & 0x3ff0;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps ps2_dmac_ops = {
    .read = ps2_dmac_read,
    .write = ps2_dmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static uint64_t ps2_dmac_enable_read(void *opaque, hwaddr addr,
                                     unsigned int size)
{
    PS2DMACState *s = opaque;

    switch (addr) {
    case PS2_DMAC_D_ENABLER:
    case PS2_DMAC_D_ENABLEW:
        return s->enablew;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }
}

static void ps2_dmac_enable_write(void *opaque, hwaddr addr, uint64_t value,
                                  unsigned int size)
{
    PS2DMACState *s = opaque;

    switch (addr) {
    case PS2_DMAC_D_ENABLEW:
        s->enablew = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        break;
    }
}

static const MemoryRegionOps ps2_dmac_enable_ops = {
    .read = ps2_dmac_enable_read,
    .write = ps2_dmac_enable_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void ps2_dmac_reset_hold(Object *obj, ResetType type)
{
    PS2DMACState *s = PS2_DMAC(obj);

    for (int i = 0; i < PS2_DMAC_NUM_CHANNELS; i++) {
        PS2DMACChannel *c = &s->ch[i];

        c->chcr = 0;
        c->madr = 0;
        c->qwc = 0;
        c->tadr = 0;
        c->asr[0] = 0;
        c->asr[1] = 0;
        c->sadr = 0;
    }
    s->ctrl = 0;
    s->stat = 0;
    s->pcr = 0;
    s->sqwc = 0;
    s->rbsr = 0;
    s->rbor = 0;
    s->stadr = 0;
    s->enablew = 0x1201;
    ps2_dmac_update_irq(s);
}

static void ps2_dmac_init(Object *obj)
{
    PS2DMACState *s = PS2_DMAC(obj);

    memory_region_init_io(&s->iomem, obj, &ps2_dmac_ops, s, TYPE_PS2_DMAC,
                          PS2_DMAC_CHAN_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    memory_region_init_io(&s->enable_iomem, obj, &ps2_dmac_enable_ops, s,
                          TYPE_PS2_DMAC "-enable", PS2_DMAC_ENABLE_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->enable_iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_ps2_dmac_channel = {
    .name = TYPE_PS2_DMAC "/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(chcr, PS2DMACChannel),
        VMSTATE_UINT32(madr, PS2DMACChannel),
        VMSTATE_UINT32(qwc, PS2DMACChannel),
        VMSTATE_UINT32(tadr, PS2DMACChannel),
        VMSTATE_UINT32_ARRAY(asr, PS2DMACChannel, 2),
        VMSTATE_UINT32(sadr, PS2DMACChannel),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_ps2_dmac = {
    .name = TYPE_PS2_DMAC,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(ch, PS2DMACState, PS2_DMAC_NUM_CHANNELS, 1,
                             vmstate_ps2_dmac_channel, PS2DMACChannel),
        VMSTATE_UINT32(ctrl, PS2DMACState),
        VMSTATE_UINT32(stat, PS2DMACState),
        VMSTATE_UINT32(pcr, PS2DMACState),
        VMSTATE_UINT32(sqwc, PS2DMACState),
        VMSTATE_UINT32(rbsr, PS2DMACState),
        VMSTATE_UINT32(rbor, PS2DMACState),
        VMSTATE_UINT32(stadr, PS2DMACState),
        VMSTATE_UINT32(enablew, PS2DMACState),
        VMSTATE_END_OF_LIST()
    },
};

static void ps2_dmac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ps2_dmac_reset_hold;
    dc->vmsd = &vmstate_ps2_dmac;
}

static const TypeInfo ps2_dmac_types[] = {
    {
        .name = TYPE_PS2_DMAC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PS2DMACState),
        .instance_init = ps2_dmac_init,
        .class_init = ps2_dmac_class_init,
    },
};

DEFINE_TYPES(ps2_dmac_types);
