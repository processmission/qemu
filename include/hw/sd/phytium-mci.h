/*
 * Local-only Phytium MCI controller model
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SD_PHYTIUM_MCI_H
#define HW_SD_PHYTIUM_MCI_H

#include "hw/core/register.h"
#include "hw/core/sysbus.h"
#include "hw/sd/sd.h"
#include "qom/object.h"

#define TYPE_PHYTIUM_MCI "phytium-mci"
OBJECT_DECLARE_SIMPLE_TYPE(PhytiumMciState, PHYTIUM_MCI)

#define TYPE_PHYTIUM_MCI_BUS "phytium-mci-bus"

#define PHYTIUM_MCI_MMIO_SIZE       0x1000
#define PHYTIUM_MCI_REG_WORDS       (0xfd4 / 4)
#define PHYTIUM_MCI_FIFO_DEPTH      0x100
#define PHYTIUM_MCI_ADMA_MAX_DESCS  4096

struct PhytiumMciState {
    SysBusDevice parent_obj;

    RegisterInfoArray *reg_array;
    qemu_irq irq;
    SDBus sdbus;

    uint32_t regs[PHYTIUM_MCI_REG_WORDS];
    RegisterInfo regs_info[PHYTIUM_MCI_REG_WORDS];

    uint8_t fifo[PHYTIUM_MCI_FIFO_DEPTH * 4];
    uint32_t fifo_len;
    uint32_t fifo_pos;

    uint32_t transfer_bytes_remaining;
    bool transfer_is_write;
    bool transfer_send_stop;
    bool transfer_synthetic_scr;
    bool transfer_active;
    uint8_t last_cmd;
};

#endif /* HW_SD_PHYTIUM_MCI_H */
