/*
 * Rockchip SFC (SPI Flash Controller) model
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Models the RK3588 SFC register interface used by the Linux
 * "rockchip,sfc" driver (drivers/spi/spi-rockchip-sfc.c) and the U-Boot
 * rockchip_sfc driver.  The controller is a register-driven SPI-memory
 * master: the guest programs SFC_CMD / SFC_ADDR / SFC_LEN_EXT, then moves
 * data through the SFC_DATA FIFO (PIO) or through SFC_DMA_ADDR +
 * SFC_DMA_TRIGGER (DMA).  SFC_VER reports 4 (RK3588): per-transfer sizes
 * live in SFC_LEN_EXT and the controller reports an effectively unlimited
 * max I/O size.
 *
 * The modeled flash device sits on a child SSI bus; commands are executed
 * against it through byte transfers with an active-low chip-select, the
 * same wiring used by the NPCM7xx FIU.  A 16 MiB m25p80-class device is
 * attached by the board (e.g. "xt25f128").
 *
 * Register behavior follows the drivers' contract:
 *  - SFC_VER >= 4 selects the v4 register semantics.
 *  - SFC_RCVR writes self-clear so reset polling terminates immediately.
 *  - SFC_SR reports idle (0) after every command; transfers are executed
 *    synchronously in the model.
 *  - SFC_FSR reports FIFO levels (TXLV/RXLV) so the PIO loops make
 *    progress.
 *  - DMA completion sets SFC_RISR.DMA and raises the interrupt line when
 *    the DMA interrupt is unmasked in SFC_IMR.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "hw/core/irq.h"
#include "hw/ssi/rockchip_sfc.h"
#include "migration/vmstate.h"

static void rockchip_sfc_update_irq(RockchipSFCState *s)
{
    /*
     * SFC_IMR bits are masks: a set bit disables the corresponding raw
     * interrupt (rockchip_sfc_irq_unmask() clears them to enable).
     */
    qemu_set_irq(s->irq, (s->risr & ~s->imr) != 0);
}

static void rockchip_sfc_reset_op(RockchipSFCState *s)
{
    s->op.pending = false;
    s->op.cmd_sent = false;
    s->op.addr_sent = false;
    s->op.opcode = 0;
    s->op.addr = 0;
    s->op.addr_nbytes = 0;
    s->op.dummy_bytes = 0;
    s->op.dir = ROCKCHIP_SFC_CMD_DIR_RD;
    s->op.cs = 0;
    s->op.len = 0;
    g_free(s->rx);
    s->rx = NULL;
    s->rx_len = 0;
    s->rx_pos = 0;
    g_free(s->tx);
    s->tx = NULL;
    s->tx_len = 0;
}

static void rockchip_sfc_select(RockchipSFCState *s, unsigned int cs)
{
    if (cs < 4 && s->cs_lines[cs]) {
        qemu_irq_lower(s->cs_lines[cs]);
    }
}

static void rockchip_sfc_deselect(RockchipSFCState *s, unsigned int cs)
{
    if (cs < 4 && s->cs_lines[cs]) {
        qemu_irq_raise(s->cs_lines[cs]);
    }
}

/*
 * Send the command, address and dummy phases of the pending operation.
 * The address must already have been written by the guest (the drivers
 * program SFC_CMD before SFC_ADDR).
 */
static void rockchip_sfc_begin_op(RockchipSFCState *s)
{
    RockchipSFCOp *op = &s->op;
    int i;

    if (!op->pending || op->cmd_sent) {
        return;
    }

    rockchip_sfc_select(s, op->cs);
    ssi_transfer(s->spi, op->opcode & ROCKCHIP_SFC_CMD_IDX_MASK);

    for (i = op->addr_nbytes - 1; i >= 0; i--) {
        ssi_transfer(s->spi, (op->addr >> (8 * i)) & 0xff);
    }
    for (i = 0; i < op->dummy_bytes; i++) {
        ssi_transfer(s->spi, 0);
    }

    op->cmd_sent = true;
    op->addr_sent = true;
}

/* Complete an operation: deselect and drop the pending state. */
static void rockchip_sfc_finish_op(RockchipSFCState *s)
{
    rockchip_sfc_deselect(s, s->op.cs);
    rockchip_sfc_reset_op(s);
}

/*
 * Execute a read operation: pull `len` bytes from the flash into the
 * internal RX buffer where SFC_DATA / DMA reads will find them.  The
 * chip-select stays asserted (the wire-level data phase) until the guest
 * signals transfer completion through SFC_SR (PIO) or SFC_DMA_TRIGGER.
 */
static void rockchip_sfc_exec_read(RockchipSFCState *s)
{
    uint32_t len = s->op.len;
    uint32_t i;

    if (len > ROCKCHIP_SFC_MAX_BUFFER) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: read length %" PRIu32
                      " exceeds model limit, clamping\n", __func__, len);
        len = ROCKCHIP_SFC_MAX_BUFFER;
        s->op.len = len;
    }

    s->rx = g_malloc(len);
    s->rx_len = len;
    s->rx_pos = 0;
    for (i = 0; i < len; i++) {
        s->rx[i] = ssi_transfer(s->spi, 0);
    }
}

/*
 * Start of an operation: SFC_CMD was just written.  The address arrives
 * next (SFC_ADDR), so for address-bearing commands defer the SSI phase
 * until then; commands without an address execute immediately.
 */
static void rockchip_sfc_start_op(RockchipSFCState *s, uint32_t cmd)
{
    unsigned int addr_mode = (cmd >> ROCKCHIP_SFC_CMD_ADDR_SHIFT) & 0x3;
    unsigned int dummy_cycles = (cmd >> ROCKCHIP_SFC_CMD_DUMMY_SHIFT) &
                                ROCKCHIP_SFC_CMD_DUMMY_MASK;

    /* Abort any stale operation so the flash sees a clean CS edge. */
    if (s->op.pending) {
        rockchip_sfc_finish_op(s);
    }
    rockchip_sfc_reset_op(s);

    s->op.pending = true;
    s->op.opcode = cmd & ROCKCHIP_SFC_CMD_IDX_MASK;
    s->op.dir = (cmd >> ROCKCHIP_SFC_CMD_DIR_SHIFT) & 0x1;
    s->op.cs = (cmd >> ROCKCHIP_SFC_CMD_CS_SHIFT) & 0x3;
    /* v4: length lives in SFC_LEN_EXT.  Cap it so the PIO write buffer
     * and the DMA staging buffer stay bounded. */
    s->op.len = MIN(s->len_ext, ROCKCHIP_SFC_MAX_BUFFER);

    switch (addr_mode) {
    case ROCKCHIP_SFC_CMD_ADDR_24BITS:
        s->op.addr_nbytes = 3;
        break;
    case ROCKCHIP_SFC_CMD_ADDR_32BITS:
        s->op.addr_nbytes = 4;
        break;
    case ROCKCHIP_SFC_CMD_ADDR_XBITS:
        /* SFC_ABIT holds nbytes * 8 - 1. */
        s->op.addr_nbytes = ((s->abit & 0x1f) + 1) / 8;
        break;
    default:
        s->op.addr_nbytes = 0;
        break;
    }

    if (dummy_cycles) {
        /* Dummy cycles convert to bytes at the data-phase bus width. */
        unsigned int data_bits = (s->ctrl >> 12) & 0x3;
        unsigned int data_width = 1 << data_bits;

        s->op.dummy_bytes = (dummy_cycles * data_width + 7) / 8;
    }

    s->cmd_reg = cmd;

    if (s->op.addr_nbytes == 0) {
        /* No address phase: execute immediately. */
        rockchip_sfc_begin_op(s);
        if (s->op.len) {
            if (s->op.dir == ROCKCHIP_SFC_CMD_DIR_RD) {
                rockchip_sfc_exec_read(s);
            }
            /* Write-without-address waits for FIFO data. */
        } else {
            rockchip_sfc_finish_op(s);
        }
    }
}

/* SFC_ADDR write: the address arrives after the command. */
static void rockchip_sfc_addr_write(RockchipSFCState *s, uint32_t addr)
{
    s->addr_reg = addr;
    if (!s->op.pending || s->op.addr_sent) {
        return;
    }
    s->op.addr = addr;
    rockchip_sfc_begin_op(s);

    if (s->op.len) {
        if (s->op.dir == ROCKCHIP_SFC_CMD_DIR_RD) {
            rockchip_sfc_exec_read(s);
        }
        /* Write ops wait for FIFO or DMA data. */
    } else {
        rockchip_sfc_finish_op(s);
    }
}

/* SFC_DATA write: buffered TX bytes are pushed into the flash. */
static void rockchip_sfc_data_write(RockchipSFCState *s, uint32_t v)
{
    uint32_t i, n = 4;
    uint8_t bytes[4];

    if (!s->op.pending || s->op.dir != ROCKCHIP_SFC_CMD_DIR_WR) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DATA write with no pending write op\n", __func__);
        return;
    }

    for (i = 0; i < n; i++) {
        bytes[i] = (v >> (8 * i)) & 0xff;
    }

    /* Only the tail word may be partial; drop padding beyond len. */
    if (s->tx_len + n > s->op.len) {
        n = s->op.len - s->tx_len;
    }

    s->tx = g_realloc(s->tx, s->tx_len + n);
    for (i = 0; i < n; i++) {
        s->tx[s->tx_len + i] = bytes[i];
        ssi_transfer(s->spi, bytes[i]);
    }
    s->tx_len += n;

    if (s->tx_len >= s->op.len) {
        rockchip_sfc_finish_op(s);
    }
}

/* SFC_DMA_TRIGGER write: move the data phase through guest memory. */
static void rockchip_sfc_dma_trigger(RockchipSFCState *s)
{
    uint32_t len = s->op.pending ? s->op.len : 0;
    hwaddr dma_addr = s->dma_addr;
    uint8_t *buf;
    uint32_t i;

    if (!s->op.pending) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DMA trigger with no pending op\n", __func__);
        return;
    }
    if (s->op.dir == ROCKCHIP_SFC_CMD_DIR_RD && !s->rx) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: DMA read before the address phase\n", __func__);
        return;
    }

    if (len > ROCKCHIP_SFC_MAX_BUFFER) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: DMA length %" PRIu32
                      " exceeds model limit, clamping\n", __func__, len);
        len = ROCKCHIP_SFC_MAX_BUFFER;
    }

    buf = g_malloc(len);

    if (s->op.dir == ROCKCHIP_SFC_CMD_DIR_WR) {
        address_space_read(&address_space_memory, dma_addr,
                           MEMTXATTRS_UNSPECIFIED, buf, len);
        for (i = 0; i < len; i++) {
            ssi_transfer(s->spi, buf[i]);
        }
    } else {
        /* Read data was pulled at command time; copy it to memory. */
        memcpy(buf, s->rx, len);
        address_space_write(&address_space_memory, dma_addr,
                            MEMTXATTRS_UNSPECIFIED, buf, len);
    }
    g_free(buf);

    rockchip_sfc_finish_op(s);
    s->risr |= ROCKCHIP_SFC_RISR_DMA;
    rockchip_sfc_update_irq(s);
}

static uint64_t rockchip_sfc_read(void *opaque, hwaddr offset, unsigned size)
{
    RockchipSFCState *s = opaque;
    uint32_t value = 0;

    switch (offset) {
    case ROCKCHIP_SFC_CTRL:
        value = s->ctrl;
        break;
    case ROCKCHIP_SFC_IMR:
        value = s->imr;
        break;
    case ROCKCHIP_SFC_FTLR:
        value = s->ftlr;
        break;
    case ROCKCHIP_SFC_RCVR:
        /* Reset completes synchronously. */
        value = 0;
        break;
    case ROCKCHIP_SFC_AX:
        value = s->ax;
        break;
    case ROCKCHIP_SFC_ABIT:
        value = s->abit;
        break;
    case ROCKCHIP_SFC_FSR:
    {
        uint32_t rx_level;
        uint32_t rx_words = (s->rx_len - s->rx_pos + 3) / 4;

        /*
         * TX data drains synchronously: every SFC_DATA write hands its
         * words straight to the flash, so the FIFO is always empty when
         * the guest reads FSR and the PIO write loop can keep filling it.
         * tx_len is only the cumulative operation length, so it must not
         * feed the occupancy report.
         */
        /* The level fields are five bits wide: 31 is the full value. */
        rx_level = MIN(rx_words, ROCKCHIP_SFC_FIFO_DEPTH - 1);

        value = (ROCKCHIP_SFC_FIFO_DEPTH << ROCKCHIP_SFC_FSR_TXLV_SHIFT) |
                (rx_level << ROCKCHIP_SFC_FSR_RXLV_SHIFT);
        value |= ROCKCHIP_SFC_FSR_TX_IS_EMPTY;
        if (s->rx_pos >= s->rx_len) {
            value |= ROCKCHIP_SFC_FSR_RX_IS_EMPTY;
        }
        break;
    }
    case ROCKCHIP_SFC_SR:
        /*
         * FSM is always idle: commands complete synchronously.  A PIO
         * read keeps the chip-select asserted through its data phase;
         * the driver polls SFC_SR after draining the FIFO, which is the
         * point where the wire-level transfer ends.
         */
        if (s->op.pending && s->op.cmd_sent &&
            s->op.dir == ROCKCHIP_SFC_CMD_DIR_RD && s->rx) {
            rockchip_sfc_finish_op(s);
        }
        value = 0;
        break;
    case ROCKCHIP_SFC_RISR:
        value = s->risr;
        break;
    case ROCKCHIP_SFC_VER:
        value = ROCKCHIP_SFC_VER_RK3588;
        break;
    case ROCKCHIP_SFC_DMA_ADDR:
        value = s->dma_addr;
        break;
    case ROCKCHIP_SFC_LEN_CTRL:
        value = s->len_ctrl;
        break;
    case ROCKCHIP_SFC_LEN_EXT:
        value = s->len_ext;
        break;
    case ROCKCHIP_SFC_DLL_CTRL0:
        value = 0;
        break;
    case ROCKCHIP_SFC_CMD:
        value = s->cmd_reg;
        break;
    case ROCKCHIP_SFC_ADDR:
        value = s->addr_reg;
        break;
    case ROCKCHIP_SFC_DATA:
        /* Serve the next FIFO word (little-endian byte stream). */
        if (s->rx && s->rx_pos < s->rx_len) {
            uint32_t i, n = MIN(4, s->rx_len - s->rx_pos);

            for (i = 0; i < n; i++) {
                value |= (uint32_t)s->rx[s->rx_pos + i] << (8 * i);
            }
            s->rx_pos += n;
        }
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%"
                      HWADDR_PRIx "\n", __func__, offset);
        break;
    }

    return value;
}

static void rockchip_sfc_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    RockchipSFCState *s = opaque;
    uint32_t v = value;

    switch (offset) {
    case ROCKCHIP_SFC_CTRL:
        s->ctrl = v;
        break;
    case ROCKCHIP_SFC_IMR:
        s->imr = v;
        rockchip_sfc_update_irq(s);
        break;
    case ROCKCHIP_SFC_ICLR:
        /* Write-1-to-clear raw interrupt status. */
        s->risr &= ~v;
        rockchip_sfc_update_irq(s);
        break;
    case ROCKCHIP_SFC_FTLR:
        s->ftlr = v;
        break;
    case ROCKCHIP_SFC_RCVR:
        /* Reset the controller: abort any in-flight operation. */
        rockchip_sfc_deselect(s, s->op.cs);
        rockchip_sfc_reset_op(s);
        s->ctrl = 0;
        s->risr = 0;
        rockchip_sfc_update_irq(s);
        break;
    case ROCKCHIP_SFC_AX:
        s->ax = v;
        break;
    case ROCKCHIP_SFC_ABIT:
        s->abit = v;
        break;
    case ROCKCHIP_SFC_ISR:
        /* Status mirror is read-only in the drivers; ignore writes. */
        break;
    case ROCKCHIP_SFC_LEN_CTRL:
        s->len_ctrl = v;
        break;
    case ROCKCHIP_SFC_LEN_EXT:
        s->len_ext = v;
        break;
    case ROCKCHIP_SFC_DLL_CTRL0:
        /* Delay-line calibration: accepted and ignored. */
        break;
    case ROCKCHIP_SFC_DMA_ADDR:
        s->dma_addr = v;
        break;
    case ROCKCHIP_SFC_DMA_TRIGGER:
        if (v & 1) {
            rockchip_sfc_dma_trigger(s);
        }
        break;
    case ROCKCHIP_SFC_CMD:
        rockchip_sfc_start_op(s, v);
        break;
    case ROCKCHIP_SFC_ADDR:
        rockchip_sfc_addr_write(s, v);
        break;
    case ROCKCHIP_SFC_DATA:
        rockchip_sfc_data_write(s, v);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%"
                      HWADDR_PRIx " = 0x%" PRIx64 "\n", __func__,
                      offset, value);
        break;
    }
}

static const MemoryRegionOps rockchip_sfc_ops = {
    .read = rockchip_sfc_read,
    .write = rockchip_sfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void rockchip_sfc_realize(DeviceState *dev, Error **errp)
{
    RockchipSFCState *s = ROCKCHIP_SFC(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->spi = ssi_create_bus(dev, "spi");
    s->cs_lines = g_new0(qemu_irq, 4);
    qdev_init_gpio_out_named(dev, s->cs_lines, "cs", 4);
    sysbus_init_irq(sbd, &s->irq);

    memory_region_init_io(&s->iomem, OBJECT(dev), &rockchip_sfc_ops, s,
                          "rockchip-sfc", ROCKCHIP_SFC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static void rockchip_sfc_reset(DeviceState *dev)
{
    RockchipSFCState *s = ROCKCHIP_SFC(dev);

    rockchip_sfc_deselect(s, s->op.cs);
    rockchip_sfc_reset_op(s);
    s->ctrl = 0;
    s->imr = 0;
    s->risr = 0;
    s->ftlr = 0;
    s->ax = 0;
    s->abit = 0;
    s->len_ctrl = 0;
    s->len_ext = 0;
    s->dma_addr = 0;
    s->cmd_reg = 0;
    s->addr_reg = 0;
}

static int rockchip_sfc_post_load(void *opaque, int version_id)
{
    RockchipSFCState *s = opaque;

    /*
     * The chip-select line is not part of the VMState; reassert it when a
     * pending operation already sent its command/address phase, and rearm
     * the pending interrupt state.
     */
    if (s->op.pending && s->op.cmd_sent) {
        rockchip_sfc_select(s, s->op.cs);
    }
    rockchip_sfc_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_rockchip_sfc = {
    .name = "rockchip-sfc",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = rockchip_sfc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, RockchipSFCState),
        VMSTATE_UINT32(imr, RockchipSFCState),
        VMSTATE_UINT32(risr, RockchipSFCState),
        VMSTATE_UINT32(ftlr, RockchipSFCState),
        VMSTATE_UINT32(ax, RockchipSFCState),
        VMSTATE_UINT32(abit, RockchipSFCState),
        VMSTATE_UINT32(len_ctrl, RockchipSFCState),
        VMSTATE_UINT32(len_ext, RockchipSFCState),
        VMSTATE_UINT32(dma_addr, RockchipSFCState),
        VMSTATE_UINT32(cmd_reg, RockchipSFCState),
        VMSTATE_UINT32(addr_reg, RockchipSFCState),
        /*
         * Version 2: carry the pending operation across migration.
         * Transfers span multiple MMIO writes (CMD, ADDR, DATA words),
         * so the destination needs op, rx and tx to continue them.
         */
        VMSTATE_UINT32_V(op.opcode, RockchipSFCState, 2),
        VMSTATE_UINT32_V(op.addr, RockchipSFCState, 2),
        VMSTATE_UINT8_V(op.addr_nbytes, RockchipSFCState, 2),
        VMSTATE_UINT8_V(op.dummy_bytes, RockchipSFCState, 2),
        VMSTATE_UINT8_V(op.dir, RockchipSFCState, 2),
        VMSTATE_UINT8_V(op.cs, RockchipSFCState, 2),
        VMSTATE_UINT32_V(op.len, RockchipSFCState, 2),
        VMSTATE_BOOL_V(op.cmd_sent, RockchipSFCState, 2),
        VMSTATE_BOOL_V(op.addr_sent, RockchipSFCState, 2),
        VMSTATE_BOOL_V(op.pending, RockchipSFCState, 2),
        VMSTATE_UINT32_V(rx_len, RockchipSFCState, 2),
        VMSTATE_UINT32_V(rx_pos, RockchipSFCState, 2),
        VMSTATE_VARRAY_UINT32_ALLOC(rx, RockchipSFCState, rx_len, 2,
                                      vmstate_info_uint8, uint8_t),
        VMSTATE_UINT32_V(tx_len, RockchipSFCState, 2),
        VMSTATE_VARRAY_UINT32_ALLOC(tx, RockchipSFCState, tx_len, 2,
                                      vmstate_info_uint8, uint8_t),
        VMSTATE_END_OF_LIST()
    },
};

static void rockchip_sfc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = rockchip_sfc_realize;
    device_class_set_legacy_reset(dc, rockchip_sfc_reset);
    dc->vmsd = &vmstate_rockchip_sfc;
    /* Not user-creatable; instantiated by the board. */
    dc->user_creatable = false;
}

static const TypeInfo rockchip_sfc_info = {
    .name = TYPE_ROCKCHIP_SFC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RockchipSFCState),
    .class_init = rockchip_sfc_class_init,
};

static void rockchip_sfc_register_types(void)
{
    type_register_static(&rockchip_sfc_info);
}

type_init(rockchip_sfc_register_types)
