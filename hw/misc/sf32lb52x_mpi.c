/*
 * SiFli SF32LB52X MPI2 QSPI-NOR controller
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Command-engine model for the external QSPI NOR. The NOR storage is the
 * SoC XIP MemoryRegion linked through the "nor" property, so the CPU keeps
 * executing in place while the controller programs and erases the same
 * bytes. NOR accesses go through the system address space so that guest
 * code translations are invalidated and migration dirty tracking stays
 * coherent. Register framework: manual MMIO (the controller is a command
 * engine with a FIFO).
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/core/sysbus.h"
#include "hw/misc/sf32lb52x_mpi.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/memory.h"

static void sf32lb52x_mpi_fifo_clear(SF32LB52XMPIState *s)
{
    s->fifo_len = 0;
}

static bool sf32lb52x_mpi_fifo_push(SF32LB52XMPIState *s, uint8_t byte)
{
    if (s->fifo_len >= SF32LB52X_MPI_FIFO_SIZE) {
        s->regs[SF32LB52X_MPI_SR / 4] |= SF32LB52X_MPI_SR_FOF;
        return false;
    }
    s->fifo[s->fifo_len++] = byte;
    return true;
}

static bool sf32lb52x_mpi_fifo_pop(SF32LB52XMPIState *s, uint8_t *byte)
{
    if (s->fifo_len == 0) {
        s->regs[SF32LB52X_MPI_SR / 4] |= SF32LB52X_MPI_SR_FUF;
        *byte = 0;
        return false;
    }
    *byte = s->fifo[0];
    memmove(s->fifo, s->fifo + 1, --s->fifo_len);
    return true;
}

static uint8_t sf32lb52x_mpi_nor_readb(SF32LB52XMPIState *s, uint32_t addr)
{
    uint8_t data;
    hwaddr guest = s->xip_base + (addr % s->nor_size);

    address_space_read(&address_space_memory, guest, MEMTXATTRS_UNSPECIFIED,
                       &data, 1);
    return data;
}

static void sf32lb52x_mpi_nor_writeb(SF32LB52XMPIState *s, uint32_t addr,
                                     uint8_t byte)
{
    hwaddr guest = s->xip_base + (addr % s->nor_size);
    MemTxResult result;

    result = address_space_write(&address_space_memory, guest,
                                 MEMTXATTRS_UNSPECIFIED, &byte, 1);
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: failed to program NOR at 0x%" HWADDR_PRIx "\n",
                      __func__, guest);
    }
}

static void sf32lb52x_mpi_nor_erase(SF32LB52XMPIState *s, uint32_t addr,
                                    uint32_t len)
{
    addr %= s->nor_size;
    len = MIN(len, s->nor_size - addr);
    address_space_set(&address_space_memory, s->xip_base + addr, 0xff, len,
                      MEMTXATTRS_UNSPECIFIED);
}

static void sf32lb52x_mpi_execute(SF32LB52XMPIState *s)
{
    uint32_t cr = s->regs[SF32LB52X_MPI_CR / 4];
    uint32_t dl = (s->regs[SF32LB52X_MPI_DLR1 / 4] & 0x3ffU) + 1U;
    uint32_t addr = s->regs[SF32LB52X_MPI_AR1 / 4];
    uint8_t cmd = s->regs[SF32LB52X_MPI_CMDR1 / 4] & 0xffU;
    uint32_t i;

    if (!(cr & SF32LB52X_MPI_CR_EN)) {
        qemu_log_mask(LOG_UNIMP, "%s: command while controller disabled\n",
                      __func__);
        return;
    }

    switch (cmd) {
    case SF32LB52X_NOR_CMD_WREN:
        s->wel = true;
        break;
    case SF32LB52X_NOR_CMD_WRSR:
        /* The driver always writes two status bytes (SR1, SR2). */
        s->qe = s->fifo_len >= 2 && (s->fifo[1] & 0x02) != 0;
        s->wel = false;
        sf32lb52x_mpi_fifo_clear(s);
        break;
    case SF32LB52X_NOR_CMD_RDSR:
        sf32lb52x_mpi_fifo_clear(s);
        sf32lb52x_mpi_fifo_push(s, s->wel ? 0x02 : 0x00);
        break;
    case SF32LB52X_NOR_CMD_RDSR2:
        sf32lb52x_mpi_fifo_clear(s);
        sf32lb52x_mpi_fifo_push(s, s->qe ? 0x02 : 0x00);
        break;
    case SF32LB52X_NOR_CMD_RDID:
        sf32lb52x_mpi_fifo_clear(s);
        sf32lb52x_mpi_fifo_push(s, SF32LB52X_NOR_JEDEC_ID & 0xff);
        sf32lb52x_mpi_fifo_push(s, (SF32LB52X_NOR_JEDEC_ID >> 8) & 0xff);
        sf32lb52x_mpi_fifo_push(s, (SF32LB52X_NOR_JEDEC_ID >> 16) & 0xff);
        break;
    case SF32LB52X_NOR_CMD_READ:
        sf32lb52x_mpi_fifo_clear(s);
        for (i = 0; i < dl; i++) {
            sf32lb52x_mpi_fifo_push(s,
                sf32lb52x_mpi_nor_readb(s, addr + i));
        }
        break;
    case SF32LB52X_NOR_CMD_PP:
        for (i = 0; i < dl && i < s->fifo_len; i++) {
            sf32lb52x_mpi_nor_writeb(s, addr + i, s->fifo[i]);
        }
        s->wel = false;
        sf32lb52x_mpi_fifo_clear(s);
        break;
    case SF32LB52X_NOR_CMD_SE:
        sf32lb52x_mpi_nor_erase(s, addr, 0x1000);
        s->wel = false;
        break;
    case SF32LB52X_NOR_CMD_BE32:
        sf32lb52x_mpi_nor_erase(s, addr, 0x8000);
        s->wel = false;
        break;
    case SF32LB52X_NOR_CMD_BE64:
        sf32lb52x_mpi_nor_erase(s, addr, 0x10000);
        s->wel = false;
        break;
    case SF32LB52X_NOR_CMD_CE:
    case SF32LB52X_NOR_CMD_CE_ALT:
        sf32lb52x_mpi_nor_erase(s, 0, SF32LB52X_MPI_NOR_SIZE);
        s->wel = false;
        break;
    default:
        /*
         * SFDP and other optional reads return a zeroed FIFO; the driver
         * derives its parameters from the devicetree, not from SFDP.
         */
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented NOR command 0x%02x\n",
                      __func__, cmd);
        sf32lb52x_mpi_fifo_clear(s);
        for (i = 0; i < MIN(dl, SF32LB52X_MPI_FIFO_SIZE); i++) {
            sf32lb52x_mpi_fifo_push(s, 0);
        }
        break;
    }

    s->regs[SF32LB52X_MPI_SR / 4] |= SF32LB52X_MPI_SR_TCF;
    if ((cr & (SF32LB52X_MPI_CR_CMD2E | SF32LB52X_MPI_CR_SME2)) ==
        (SF32LB52X_MPI_CR_CMD2E | SF32LB52X_MPI_CR_SME2)) {
        /* The NOR is modeled as immediately ready for the status match. */
        s->regs[SF32LB52X_MPI_SR / 4] |= SF32LB52X_MPI_SR_SMF;
    }
}

static uint64_t sf32lb52x_mpi_read(void *opaque, hwaddr addr, unsigned int size)
{
    SF32LB52XMPIState *s = opaque;
    uint64_t value = 0;
    uint32_t i;

    if (addr >= SF32LB52X_MPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }

    if (addr == SF32LB52X_MPI_DR) {
        for (i = 0; i < 4; i++) {
            uint8_t byte;

            sf32lb52x_mpi_fifo_pop(s, &byte);
            value |= (uint64_t)byte << (8 * i);
        }
        return value;
    }

    return s->regs[addr / 4];
}

static void sf32lb52x_mpi_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned int size)
{
    SF32LB52XMPIState *s = opaque;
    uint32_t i;

    if (addr >= SF32LB52X_MPI_SIZE) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return;
    }

    switch (addr) {
    case SF32LB52X_MPI_DR:
        for (i = 0; i < size; i++) {
            sf32lb52x_mpi_fifo_push(s, (value >> (8 * i)) & 0xff);
        }
        break;
    case SF32LB52X_MPI_CMDR1:
        s->regs[addr / 4] = value;
        sf32lb52x_mpi_execute(s);
        break;
    case SF32LB52X_MPI_SCR:
        s->regs[SF32LB52X_MPI_SR / 4] &= ~(value &
            (SF32LB52X_MPI_SCR_TCFC | SF32LB52X_MPI_SCR_FUFC |
             SF32LB52X_MPI_SCR_FOFC | SF32LB52X_MPI_SCR_SMFC));
        break;
    case SF32LB52X_MPI_FIFOCR:
        if (value & SF32LB52X_MPI_FIFOCR_RXCLR) {
            sf32lb52x_mpi_fifo_clear(s);
        }
        s->regs[addr / 4] = value & ~(SF32LB52X_MPI_FIFOCR_RXCLR |
                                      SF32LB52X_MPI_FIFOCR_TXCLR);
        break;
    default:
        s->regs[addr / 4] = value;
        break;
    }
}

static const MemoryRegionOps sf32lb52x_mpi_ops = {
    .read = sf32lb52x_mpi_read,
    .write = sf32lb52x_mpi_write,
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

static void sf32lb52x_mpi_reset_hold(Object *obj, ResetType type)
{
    SF32LB52XMPIState *s = SF32LB52X_MPI(obj);

    memset(s->regs, 0, sizeof(s->regs));
    sf32lb52x_mpi_fifo_clear(s);
    s->wel = false;
    s->qe = false;
}

static void sf32lb52x_mpi_realize(DeviceState *dev, Error **errp)
{
    SF32LB52XMPIState *s = SF32LB52X_MPI(dev);

    if (!s->nor) {
        error_setg(errp, "sf32lb52x-mpi: nor link is required");
        return;
    }
    if (!s->xip_base) {
        error_setg(errp, "sf32lb52x-mpi: xip-base must be set");
        return;
    }

    s->nor_size = int128_get64(s->nor->size);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sf32lb52x_mpi_ops, s,
                          TYPE_SF32LB52X_MPI, SF32LB52X_MPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const Property sf32lb52x_mpi_properties[] = {
    DEFINE_PROP_LINK("nor", SF32LB52XMPIState, nor,
                     TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_UINT64("xip-base", SF32LB52XMPIState, xip_base,
                       0x12000000),
};

static const VMStateDescription vmstate_sf32lb52x_mpi = {
    .name = TYPE_SF32LB52X_MPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, SF32LB52XMPIState, SF32LB52X_MPI_NUM_REGS),
        VMSTATE_UINT8_ARRAY(fifo, SF32LB52XMPIState, SF32LB52X_MPI_FIFO_SIZE),
        VMSTATE_UINT32(fifo_len, SF32LB52XMPIState),
        VMSTATE_BOOL(wel, SF32LB52XMPIState),
        VMSTATE_BOOL(qe, SF32LB52XMPIState),
        VMSTATE_END_OF_LIST()
    },
};

static void sf32lb52x_mpi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = sf32lb52x_mpi_reset_hold;
    dc->realize = sf32lb52x_mpi_realize;
    dc->desc = "SiFli SF32LB52X MPI2 QSPI-NOR";
    dc->vmsd = &vmstate_sf32lb52x_mpi;
    dc->user_creatable = false;
    device_class_set_props(dc, sf32lb52x_mpi_properties);
}

static const TypeInfo sf32lb52x_mpi_types[] = {
    {
        .name = TYPE_SF32LB52X_MPI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SF32LB52XMPIState),
        .class_init = sf32lb52x_mpi_class_init,
    },
};

DEFINE_TYPES(sf32lb52x_mpi_types);
