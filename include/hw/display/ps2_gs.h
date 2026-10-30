/*
 * Sony PlayStation 2 Graphics Synthesizer (GS) and GIF
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_DISPLAY_PS2_GS_H
#define HW_DISPLAY_PS2_GS_H

#include "hw/core/sysbus.h"
#include "hw/dma/ps2_dmac.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_PS2_GS "ps2-gs"
OBJECT_DECLARE_SIMPLE_TYPE(PS2GSState, PS2_GS)

/* Privileged registers at 0x12000000; the kernel maps 32 MiB. */
#define PS2_GS_PRIV_SIZE        0x2000000
#define PS2_GS_PMODE            0x0000
#define PS2_GS_SMODE1           0x0010
#define PS2_GS_SMODE2           0x0020
#define PS2_GS_SRFSH            0x0030
#define PS2_GS_SYNCH1           0x0040
#define PS2_GS_SYNCH2           0x0050
#define PS2_GS_SYNCV            0x0060
#define PS2_GS_DISPFB1          0x0070
#define PS2_GS_DISPLAY1         0x0080
#define PS2_GS_DISPFB2          0x0090
#define PS2_GS_DISPLAY2         0x00a0
#define PS2_GS_EXTBUF           0x00b0
#define PS2_GS_EXTDATA          0x00c0
#define PS2_GS_EXTWRITE         0x00d0
#define PS2_GS_BGCOLOR          0x00e0
#define PS2_GS_CSR              0x1000
#define PS2_GS_IMR              0x1010
#define PS2_GS_BUSDIR           0x1040
#define PS2_GS_SIGLBLID         0x1080
#define PS2_GS_NUM_DISP_REGS    15

#define PS2_GS_CSR_SIGNAL       (1 << 0)
#define PS2_GS_CSR_FINISH       (1 << 1)
#define PS2_GS_CSR_HSINT        (1 << 2)
#define PS2_GS_CSR_VSINT        (1 << 3)
#define PS2_GS_CSR_EDWINT       (1 << 4)
#define PS2_GS_CSR_EVENTS       0x1f
#define PS2_GS_CSR_FLUSH        (1 << 8)
#define PS2_GS_CSR_RESET        (1 << 9)
#define PS2_GS_CSR_NFIELD       (1 << 12)
#define PS2_GS_CSR_FIELD        (1 << 13)
#define PS2_GS_CSR_FIFO_EMPTY   (1 << 14)
#define PS2_GS_CSR_REV          0x1b
#define PS2_GS_CSR_ID           0x55

/* GIF registers at 0x10003000. */
#define PS2_GIF_SIZE            0x100
#define PS2_GIF_CTRL            0x00
#define PS2_GIF_MODE            0x10
#define PS2_GIF_STAT            0x20
#define PS2_GIF_CTRL_RST        (1 << 0)

#define PS2_GS_VRAM_SIZE        (4 * MiB)
/* General registers, written through the GIF. */
#define PS2_GS_NUM_REGS         0x80

struct PS2GSState {
    SysBusDevice parent_obj;

    MemoryRegion priv_iomem;
    MemoryRegion gif_iomem;
    qemu_irq irq;
    qemu_irq vblank_start;
    qemu_irq vblank_end;
    QEMUTimer *vblank_timer;
    QEMUTimer *vblank_end_timer;
    QemuConsole *con;
    bool dirty;

    uint64_t disp[PS2_GS_NUM_DISP_REGS];
    uint64_t csr;
    uint64_t imr;
    uint64_t busdir;
    uint64_t siglblid;
    uint32_t gif_ctrl;
    uint32_t gif_mode;

    /* The GIF tag being processed and a partial quadword. */
    uint64_t gif_regs;
    uint32_t gif_nloop;
    uint32_t gif_flg;
    uint32_t gif_nreg;
    uint32_t gif_reg;
    uint8_t gif_qw[16];
    uint32_t gif_qw_len;

    uint64_t regs[PS2_GS_NUM_REGS];
    /* The Q of the last ST in PACKED mode, for the next RGBAQ. */
    uint32_t q;
    /* The vertex queue: XYZ, FOG, RGBAQ, ST and UV of each vertex. */
    uint64_t vtx_xyz[3];
    uint64_t vtx_fog[3];
    uint64_t vtx_rgbaq[3];
    uint64_t vtx_st[3];
    uint64_t vtx_uv[3];
    uint32_t vtx_count;

    /* A host to local transfer: the next pixel and a partial pixel. */
    bool trx_active;
    uint32_t trx_x;
    uint32_t trx_y;
    uint32_t trx_acc;
    uint32_t trx_acc_len;
    /* A local to host transfer, read through VIF1, uses trx_x and trx_y. */
    bool trx_read_active;

    uint8_t vram[PS2_GS_VRAM_SIZE];
};

/* DMAC channel 2 sink: one block of PATH3 GIF packet data. */
void ps2_gs_gif_write(PS2GSState *s, const uint8_t *buf, uint32_t len);

/*
 * Read up to len bytes of the local to host transfer started by TRXDIR,
 * padding with zeros once it ends.  Returns the bytes of image data.
 */
uint32_t ps2_gs_read_local(PS2GSState *s, uint8_t *buf, uint32_t len);

/* Channel ops to attach to DMAC channel 2, with the GS as opaque. */
const PS2DMACChannelOps *ps2_gs_get_dma_ops(void);

#endif
