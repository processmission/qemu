/*
 * Sony PlayStation 2 Graphics Synthesizer (GS) and GIF
 *
 * The GS privileged registers configure the CRT controller and carry
 * the CSR/IMR interrupt pair that the kernel cascades from INTC line 0.
 * A vblank timer raises the INTC VBLANK start and end lines, sets
 * CSR.VSINT and toggles CSR.FIELD.  GIF packets arrive over DMAC
 * channel 2 through ps2_gs_gif_write().
 *
 * The GIF unpacks PACKED, REGLIST and IMAGE packets into writes of the
 * general registers.  Drawing covers points, lines, triangles and
 * sprites, flat or Gouraud shaded, textured with a colour lookup table,
 * fogged, alpha tested, depth tested against a Z buffer and blended, but
 * without antialiasing, mipmapping or bilinear filtering.  Host to local
 * and local to local transfers move pixels within the 4 MiB of local
 * memory; local to host transfers are read through VIF1.
 *
 * Local memory is made of 256-byte blocks, arranged into 8 KiB pages for
 * each pixel storage mode as on the GS, so that buffers placed by block
 * address do not overlap.  The pixels inside a block are kept in row
 * order, which only matters to software that reads one buffer with
 * different storage modes.
 *
 * The CRT controller output is composed from the two read circuits as
 * set by PMODE, DISPFB and DISPLAY, without blending them.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "hw/display/ps2_gs.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "ui/console.h"

#define PS2_GS_SMODE1_CMOD_PAL  3

/* The vertical blanking interval lasts about 22 scanlines. */
#define PS2_GS_VBLANK_NS        1400000

/* General register addresses. */
#define GS_PRIM         0x00
#define GS_RGBAQ        0x01
#define GS_ST           0x02
#define GS_UV           0x03
#define GS_XYZF2        0x04
#define GS_XYZ2         0x05
#define GS_TEX0_1       0x06
#define GS_CLAMP_1      0x08
#define GS_FOG          0x0a
#define GS_XYZF3        0x0c
#define GS_XYZ3         0x0d
#define GS_TEX2_1       0x16
#define GS_TEX2_2       0x17
#define GS_XYOFFSET_1   0x18
#define GS_PRMODECONT   0x1a
#define GS_PRMODE       0x1b
#define GS_TEXA         0x3b
#define GS_FOGCOL       0x3d
#define GS_SCISSOR_1    0x40
#define GS_ALPHA_1      0x42
#define GS_COLCLAMP     0x46
#define GS_TEST_1       0x47
#define GS_FRAME_1      0x4c
#define GS_ZBUF_1       0x4e
#define GS_BITBLTBUF    0x50
#define GS_TRXPOS       0x51
#define GS_TRXREG       0x52
#define GS_TRXDIR       0x53
#define GS_HWREG        0x54
#define GS_SIGNAL       0x60
#define GS_FINISH       0x61
#define GS_LABEL        0x62

/* GIF tag register descriptors. */
#define GIF_REG_PRIM    0x0
#define GIF_REG_RGBAQ   0x1
#define GIF_REG_ST      0x2
#define GIF_REG_UV      0x3
#define GIF_REG_XYZF2   0x4
#define GIF_REG_XYZ2    0x5
#define GIF_REG_FOG     0xa
#define GIF_REG_XYZF3   0xc
#define GIF_REG_XYZ3    0xd
#define GIF_REG_AD      0xe
#define GIF_REG_NOP     0xf

#define GIF_FLG_PACKED  0
#define GIF_FLG_REGLIST 1

/* PRIM types and attributes. */
#define GS_PRIM_POINT           0
#define GS_PRIM_LINE            1
#define GS_PRIM_LINE_STRIP      2
#define GS_PRIM_TRIANGLE        3
#define GS_PRIM_TRIANGLE_STRIP  4
#define GS_PRIM_TRIANGLE_FAN    5
#define GS_PRIM_SPRITE          6
#define GS_PRIM_IIP             (1 << 3)
#define GS_PRIM_TME             (1 << 4)
#define GS_PRIM_FGE             (1 << 5)
#define GS_PRIM_ABE             (1 << 6)
#define GS_PRIM_FST             (1 << 8)
#define GS_PRIM_CTXT            (1 << 9)
#define GS_PRIM_ATTRS           0x7f8

/* Pixel storage modes. */
#define GS_PSMCT32      0x00
#define GS_PSMCT24      0x01
#define GS_PSMCT16      0x02
#define GS_PSMCT16S     0x0a
#define GS_PSMT8        0x13
#define GS_PSMT4        0x14
#define GS_PSMT8H       0x1b
#define GS_PSMT4HL      0x24
#define GS_PSMT4HH      0x2c
#define GS_PSMZ32       0x30
#define GS_PSMZ24       0x31
#define GS_PSMZ16       0x32
#define GS_PSMZ16S      0x3a

#define GS_TFX_MODULATE     0
#define GS_TFX_DECAL        1
#define GS_TFX_HIGHLIGHT    2

#define GS_XDIR_HOST_LOCAL  0
#define GS_XDIR_LOCAL_HOST  1
#define GS_XDIR_LOCAL_LOCAL 2

#define GS_BLOCK_SIZE       256
#define GS_BLOCKS_PER_PAGE  32
#define GS_NUM_BLOCKS       (PS2_GS_VRAM_SIZE / GS_BLOCK_SIZE)
/* Transfer and display coordinates are 11 bits. */
#define GS_COORD_MASK       0x7ff

/*
 * The arrangement of the blocks in a page for a pixel storage mode, and
 * the number of bits a pixel takes in memory.
 */
typedef struct PS2GSLayout {
    uint8_t bpp;
    uint8_t page_w;
    uint8_t page_h;
    uint8_t block_w;
    uint8_t block_h;
    /* Pages are 64 pixels wide for each unit of buffer width. */
    uint8_t bw_per_page;
    uint8_t table[GS_BLOCKS_PER_PAGE];
} PS2GSLayout;

static const PS2GSLayout ps2_gs_layout32 = {
    32, 64, 32, 8, 8, 1, {
        0, 1, 4, 5, 16, 17, 20, 21,
        2, 3, 6, 7, 18, 19, 22, 23,
        8, 9, 12, 13, 24, 25, 28, 29,
        10, 11, 14, 15, 26, 27, 30, 31,
    },
};

static const PS2GSLayout ps2_gs_layout16 = {
    16, 64, 64, 16, 8, 1, {
        0, 2, 8, 10,
        1, 3, 9, 11,
        4, 6, 12, 14,
        5, 7, 13, 15,
        16, 18, 24, 26,
        17, 19, 25, 27,
        20, 22, 28, 30,
        21, 23, 29, 31,
    },
};

static const PS2GSLayout ps2_gs_layout16s = {
    16, 64, 64, 16, 8, 1, {
        0, 2, 16, 18,
        1, 3, 17, 19,
        8, 10, 24, 26,
        9, 11, 25, 27,
        4, 6, 20, 22,
        5, 7, 21, 23,
        12, 14, 28, 30,
        13, 15, 29, 31,
    },
};

static const PS2GSLayout ps2_gs_layout8 = {
    8, 128, 64, 16, 16, 2, {
        0, 1, 4, 5, 16, 17, 20, 21,
        2, 3, 6, 7, 18, 19, 22, 23,
        8, 9, 12, 13, 24, 25, 28, 29,
        10, 11, 14, 15, 26, 27, 30, 31,
    },
};

static const PS2GSLayout ps2_gs_layout4 = {
    4, 128, 128, 32, 16, 2, {
        0, 2, 8, 10,
        1, 3, 9, 11,
        4, 6, 12, 14,
        5, 7, 13, 15,
        16, 18, 24, 26,
        17, 19, 25, 27,
        20, 22, 28, 30,
        21, 23, 29, 31,
    },
};

static const PS2GSLayout *ps2_gs_layout(unsigned int psm)
{
    switch (psm) {
    case GS_PSMCT16:
    case GS_PSMZ16:
        return &ps2_gs_layout16;
    case GS_PSMCT16S:
    case GS_PSMZ16S:
        return &ps2_gs_layout16s;
    case GS_PSMT8:
        return &ps2_gs_layout8;
    case GS_PSMT4:
        return &ps2_gs_layout4;
    default:
        /* PSMT8H and PSMT4HL/HH live in the upper bits of 32-bit pixels. */
        return &ps2_gs_layout32;
    }
}

/* The number of bits a pixel takes in a transfer. */
static unsigned int ps2_gs_transfer_bpp(unsigned int psm)
{
    switch (psm) {
    case GS_PSMCT24:
    case GS_PSMZ24:
        return 24;
    case GS_PSMCT16:
    case GS_PSMCT16S:
    case GS_PSMZ16:
    case GS_PSMZ16S:
        return 16;
    case GS_PSMT8:
    case GS_PSMT8H:
        return 8;
    case GS_PSMT4:
    case GS_PSMT4HL:
    case GS_PSMT4HH:
        return 4;
    default:
        return 32;
    }
}

static bool ps2_gs_psm_valid(unsigned int psm)
{
    switch (psm) {
    case GS_PSMCT32:
    case GS_PSMCT24:
    case GS_PSMCT16:
    case GS_PSMCT16S:
    case GS_PSMT8:
    case GS_PSMT4:
    case GS_PSMT8H:
    case GS_PSMT4HL:
    case GS_PSMT4HH:
    case GS_PSMZ32:
    case GS_PSMZ24:
    case GS_PSMZ16:
    case GS_PSMZ16S:
        return true;
    default:
        return false;
    }
}

/*
 * The index of a pixel in local memory, in units of the pixel size of its
 * layout.  @bp is a block address and @bw a buffer width in 64 pixels.
 */
static uint32_t ps2_gs_pixel_index(const PS2GSLayout *l, uint32_t bp,
                                   uint32_t bw, uint32_t x, uint32_t y)
{
    uint32_t pages_per_row = bw / l->bw_per_page;
    uint32_t page = (y / l->page_h) * pages_per_row + x / l->page_w;
    uint32_t px = x % l->page_w, py = y % l->page_h;
    uint32_t cols = l->page_w / l->block_w;
    uint32_t block = bp + page * GS_BLOCKS_PER_PAGE +
                     l->table[(py / l->block_h) * cols + px / l->block_w];
    uint32_t pixels_per_block = GS_BLOCK_SIZE * 8 / l->bpp;

    return (block % GS_NUM_BLOCKS) * pixels_per_block +
           (py % l->block_h) * l->block_w + px % l->block_w;
}

static uint32_t ps2_gs_read_pixel(PS2GSState *s, unsigned int psm,
                                  uint32_t bp, uint32_t bw,
                                  uint32_t x, uint32_t y)
{
    const PS2GSLayout *l = ps2_gs_layout(psm);
    uint32_t i = ps2_gs_pixel_index(l, bp, bw, x, y);
    uint32_t v;

    switch (l->bpp) {
    case 4:
        return (s->vram[i / 2] >> ((i & 1) * 4)) & 0xf;
    case 8:
        return s->vram[i];
    case 16:
        return lduw_le_p(&s->vram[i * 2]);
    }

    v = ldl_le_p(&s->vram[i * 4]);
    switch (psm) {
    case GS_PSMCT24:
    case GS_PSMZ24:
        return v & 0xffffff;
    case GS_PSMT8H:
        return v >> 24;
    case GS_PSMT4HL:
        return (v >> 24) & 0xf;
    case GS_PSMT4HH:
        return v >> 28;
    default:
        return v;
    }
}

static void ps2_gs_write_pixel(PS2GSState *s, unsigned int psm,
                               uint32_t bp, uint32_t bw,
                               uint32_t x, uint32_t y, uint32_t value)
{
    const PS2GSLayout *l = ps2_gs_layout(psm);
    uint32_t i = ps2_gs_pixel_index(l, bp, bw, x, y);
    uint8_t *p;
    uint32_t v;

    s->dirty = true;

    switch (l->bpp) {
    case 4:
        p = &s->vram[i / 2];
        *p = (i & 1) ? (*p & 0x0f) | (value << 4) : (*p & 0xf0) | (value & 0xf);
        return;
    case 8:
        s->vram[i] = value;
        return;
    case 16:
        stw_le_p(&s->vram[i * 2], value);
        return;
    }

    p = &s->vram[i * 4];
    v = ldl_le_p(p);
    switch (psm) {
    case GS_PSMCT24:
    case GS_PSMZ24:
        v = deposit32(v, 0, 24, value);
        break;
    case GS_PSMT8H:
        v = deposit32(v, 24, 8, value);
        break;
    case GS_PSMT4HL:
        v = deposit32(v, 24, 4, value);
        break;
    case GS_PSMT4HH:
        v = deposit32(v, 28, 4, value);
        break;
    default:
        v = value;
        break;
    }
    stl_le_p(p, v);
}

static bool ps2_gs_psm_is_16(unsigned int psm)
{
    return ps2_gs_transfer_bpp(psm) == 16;
}

/* Expand a pixel of a colour storage mode to 32-bit RGBA, with TEXA. */
static uint32_t ps2_gs_to_rgba(unsigned int psm, uint32_t v, uint64_t texa)
{
    uint32_t ta0 = extract64(texa, 0, 8);
    bool aem = extract64(texa, 15, 1);
    uint32_t ta1 = extract64(texa, 32, 8);
    uint32_t rgb, a;

    if (ps2_gs_psm_is_16(psm)) {
        rgb = (extract32(v, 0, 5) << 3) | (extract32(v, 5, 5) << 11) |
              (extract32(v, 10, 5) << 19);
        a = (v & 0x8000) ? ta1 : (aem && !rgb) ? 0 : ta0;
        return rgb | (a << 24);
    }
    if (psm == GS_PSMCT24 || psm == GS_PSMZ24) {
        rgb = v & 0xffffff;
        return rgb | (((aem && !rgb) ? 0 : ta0) << 24);
    }
    return v;
}

static uint32_t ps2_gs_from_rgba(unsigned int psm, uint32_t rgba)
{
    if (ps2_gs_psm_is_16(psm)) {
        return extract32(rgba, 3, 5) | (extract32(rgba, 11, 5) << 5) |
               (extract32(rgba, 19, 5) << 10) | (extract32(rgba, 31, 1) << 15);
    }
    return rgba;
}

static uint64_t ps2_gs_frame_ns(PS2GSState *s)
{
    uint64_t smode1 = s->disp[PS2_GS_SMODE1 >> 4];

    if (extract64(smode1, 13, 2) == PS2_GS_SMODE1_CMOD_PAL) {
        return NANOSECONDS_PER_SECOND / 50;
    }
    return muldiv64(NANOSECONDS_PER_SECOND, 1001, 60000);
}

static void ps2_gs_update_irq(PS2GSState *s)
{
    uint64_t pending = s->csr & ~(s->imr >> 8) & PS2_GS_CSR_EVENTS;

    qemu_set_irq(s->irq, pending != 0);
}

static void ps2_gs_vblank(void *opaque)
{
    PS2GSState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->csr ^= PS2_GS_CSR_FIELD;
    s->csr |= PS2_GS_CSR_VSINT;
    ps2_gs_update_irq(s);
    qemu_irq_pulse(s->vblank_start);

    timer_mod(s->vblank_end_timer, now + PS2_GS_VBLANK_NS);
    timer_mod(s->vblank_timer, now + ps2_gs_frame_ns(s));
}

static void ps2_gs_vblank_end(void *opaque)
{
    PS2GSState *s = opaque;

    qemu_irq_pulse(s->vblank_end);
}

/* The drawing environment of a primitive. */
typedef struct PS2GSContext {
    uint32_t prim;
    uint32_t fbp;
    uint32_t fbw;
    uint32_t fpsm;
    uint32_t fbmsk;
    uint32_t zbp;
    uint32_t zpsm;
    bool zmsk;
    int32_t ofx;
    int32_t ofy;
    int32_t scax0;
    int32_t scax1;
    int32_t scay0;
    int32_t scay1;
    uint64_t tex0;
    uint64_t clamp;
    uint64_t alpha;
    uint64_t test;
    uint32_t tw;
    uint32_t th;
} PS2GSContext;

static void ps2_gs_context(PS2GSState *s, PS2GSContext *c)
{
    uint64_t prim = s->regs[GS_PRIM];
    uint32_t attrs = (s->regs[GS_PRMODECONT] & 1) ? prim : s->regs[GS_PRMODE];
    unsigned int n;
    uint64_t r;

    c->prim = (prim & 7) | (attrs & GS_PRIM_ATTRS);
    n = (c->prim & GS_PRIM_CTXT) ? 1 : 0;

    r = s->regs[GS_FRAME_1 + n];
    c->fbp = extract64(r, 0, 9) * GS_BLOCKS_PER_PAGE;
    c->fbw = extract64(r, 16, 6);
    c->fpsm = extract64(r, 24, 6);
    c->fbmsk = extract64(r, 32, 32);

    r = s->regs[GS_ZBUF_1 + n];
    c->zbp = extract64(r, 0, 9) * GS_BLOCKS_PER_PAGE;
    c->zpsm = extract64(r, 24, 4) | GS_PSMZ32;
    c->zmsk = extract64(r, 32, 1);

    r = s->regs[GS_XYOFFSET_1 + n];
    c->ofx = extract64(r, 0, 16);
    c->ofy = extract64(r, 32, 16);

    r = s->regs[GS_SCISSOR_1 + n];
    c->scax0 = extract64(r, 0, 11);
    c->scax1 = extract64(r, 16, 11);
    c->scay0 = extract64(r, 32, 11);
    c->scay1 = extract64(r, 48, 11);

    c->tex0 = s->regs[GS_TEX0_1 + n];
    c->clamp = s->regs[GS_CLAMP_1 + n];
    c->alpha = s->regs[GS_ALPHA_1 + n];
    c->test = s->regs[GS_TEST_1 + n];
    c->tw = 1 << MIN(extract64(c->tex0, 26, 4), 10);
    c->th = 1 << MIN(extract64(c->tex0, 30, 4), 10);
}

static int32_t ps2_gs_wrap(int32_t t, unsigned int mode, int32_t min,
                           int32_t max, int32_t size)
{
    switch (mode) {
    case 0:
        return t & (size - 1);
    case 1:
        return MIN(MAX(t, 0), size - 1);
    case 2:
        return MIN(MAX(t, min), max);
    default:
        return (t & min) | max;
    }
}

/* The colour lookup table entry for an index, in CSM1 arrangement. */
static uint32_t ps2_gs_clut(PS2GSState *s, const PS2GSContext *c,
                            uint32_t index)
{
    uint32_t cbp = extract64(c->tex0, 37, 14);
    uint32_t cpsm = extract64(c->tex0, 51, 4);
    uint32_t x = (index & 7) | ((index & 0x10) >> 1);
    uint32_t y = ((index & 8) >> 3) | ((index >> 5) << 1);

    if (extract64(c->tex0, 55, 1)) {
        qemu_log_mask(LOG_UNIMP, "%s: CSM2 is not supported\n", __func__);
    }
    return ps2_gs_to_rgba(cpsm, ps2_gs_read_pixel(s, cpsm, cbp, 1, x, y),
                          s->regs[GS_TEXA]);
}

static uint32_t ps2_gs_texel(PS2GSState *s, const PS2GSContext *c,
                             int32_t u, int32_t v)
{
    uint32_t tbp = extract64(c->tex0, 0, 14);
    uint32_t tbw = extract64(c->tex0, 14, 6);
    uint32_t psm = extract64(c->tex0, 20, 6);
    uint32_t t;

    u = ps2_gs_wrap(u, extract64(c->clamp, 0, 2), extract64(c->clamp, 4, 10),
                    extract64(c->clamp, 14, 10), c->tw);
    v = ps2_gs_wrap(v, extract64(c->clamp, 2, 2), extract64(c->clamp, 24, 10),
                    extract64(c->clamp, 34, 10), c->th);
    t = ps2_gs_read_pixel(s, psm, tbp, tbw, u & GS_COORD_MASK,
                          v & GS_COORD_MASK);

    switch (psm) {
    case GS_PSMT8:
    case GS_PSMT8H:
        return ps2_gs_clut(s, c, t);
    case GS_PSMT4:
    case GS_PSMT4HL:
    case GS_PSMT4HH:
        return ps2_gs_clut(s, c, (t + extract64(c->tex0, 56, 5) * 16) & 0xff);
    default:
        return ps2_gs_to_rgba(psm, t, s->regs[GS_TEXA]);
    }
}

static uint32_t ps2_gs_channel(uint32_t c, unsigned int shift)
{
    return (c >> shift) & 0xff;
}

/* Combine the texture and fragment colours as given by TEX0.TFX/TCC. */
static uint32_t ps2_gs_texture_function(const PS2GSContext *c,
                                        uint32_t tex, uint32_t frag)
{
    unsigned int tfx = extract64(c->tex0, 35, 2);
    bool tcc = extract64(c->tex0, 34, 1);
    uint32_t af = frag >> 24, at = tex >> 24;
    uint32_t out = 0, a;

    if (tfx == GS_TFX_DECAL) {
        return (tex & 0xffffff) | ((tcc ? at : af) << 24);
    }
    for (unsigned int i = 0; i < 24; i += 8) {
        uint32_t v = (ps2_gs_channel(tex, i) * ps2_gs_channel(frag, i)) >> 7;

        if (tfx >= GS_TFX_HIGHLIGHT) {
            v += af;
        }
        out |= MIN(v, 0xff) << i;
    }
    if (!tcc) {
        a = af;
    } else if (tfx == GS_TFX_MODULATE) {
        a = MIN((at * af) >> 7, 0xff);
    } else if (tfx == GS_TFX_HIGHLIGHT) {
        a = MIN(at + af, 0xff);
    } else {
        a = at;
    }
    return out | (a << 24);
}

static bool ps2_gs_alpha_test(const PS2GSContext *c, uint32_t a)
{
    uint32_t aref = extract64(c->test, 4, 8);

    switch (extract64(c->test, 1, 3)) {
    case 0:
        return false;
    case 1:
        return true;
    case 2:
        return a < aref;
    case 3:
        return a <= aref;
    case 4:
        return a == aref;
    case 5:
        return a >= aref;
    case 6:
        return a > aref;
    default:
        return a != aref;
    }
}

static uint32_t ps2_gs_blend(PS2GSState *s, const PS2GSContext *c,
                             uint32_t cs, uint32_t cd)
{
    uint32_t sel[3] = { cs, cd, 0 };
    uint32_t a = sel[MIN(extract64(c->alpha, 0, 2), 2)];
    uint32_t b = sel[MIN(extract64(c->alpha, 2, 2), 2)];
    uint32_t d = sel[MIN(extract64(c->alpha, 6, 2), 2)];
    uint32_t factors[3] = { cs >> 24, cd >> 24, extract64(c->alpha, 32, 8) };
    int32_t f = factors[MIN(extract64(c->alpha, 4, 2), 2)];
    bool clamp = s->regs[GS_COLCLAMP] & 1;
    uint32_t out = cs & 0xff000000;

    for (unsigned int i = 0; i < 24; i += 8) {
        int32_t v = (((int32_t)ps2_gs_channel(a, i) -
                      (int32_t)ps2_gs_channel(b, i)) * f >> 7) +
                    ps2_gs_channel(d, i);

        out |= (clamp ? MIN(MAX(v, 0), 0xff) : v & 0xff) << i;
    }
    return out;
}

/* Mix the fog colour into a fragment with fog coefficient @f. */
static uint32_t ps2_gs_fog(PS2GSState *s, uint32_t rgba, uint32_t f)
{
    uint32_t fogcol = s->regs[GS_FOGCOL];
    uint32_t out = rgba & 0xff000000;

    for (unsigned int i = 0; i < 24; i += 8) {
        out |= ((ps2_gs_channel(rgba, i) * f +
                 ps2_gs_channel(fogcol, i) * (0xff - f)) >> 8) << i;
    }
    return out;
}

/* The largest depth that a Z buffer storage mode holds. */
static uint32_t ps2_gs_z_max(unsigned int zpsm)
{
    switch (zpsm) {
    case GS_PSMZ32:
        return UINT32_MAX;
    case GS_PSMZ24:
        return 0xffffff;
    default:
        return 0xffff;
    }
}

/* Run a fragment through the pixel pipeline into the frame buffer. */
static void ps2_gs_fragment(PS2GSState *s, const PS2GSContext *c,
                            int32_t x, int32_t y, uint32_t rgba,
                            int32_t u, int32_t v, uint32_t z, uint32_t f)
{
    uint32_t mask = c->fbmsk, dst, out, zdst = 0;
    bool write_fb = true, write_alpha = true, write_z = !c->zmsk;
    bool zte = extract64(c->test, 16, 1);

    if (c->prim & GS_PRIM_TME) {
        rgba = ps2_gs_texture_function(c, ps2_gs_texel(s, c, u, v), rgba);
    }
    if (c->prim & GS_PRIM_FGE) {
        rgba = ps2_gs_fog(s, rgba, f);
    }

    if ((c->test & 1) && !ps2_gs_alpha_test(c, rgba >> 24)) {
        switch (extract64(c->test, 12, 2)) {
        case 0:
            /* KEEP */
            return;
        case 1:
            /* FB_ONLY */
            write_z = false;
            break;
        case 2:
            /* ZB_ONLY */
            write_fb = false;
            break;
        default:
            /* RGB_ONLY */
            write_alpha = false;
            write_z = false;
            break;
        }
    }

    if (zte) {
        z = MIN(z, ps2_gs_z_max(c->zpsm));
        zdst = ps2_gs_read_pixel(s, c->zpsm, c->zbp, c->fbw, x, y);
        switch (extract64(c->test, 17, 2)) {
        case 0:
            /* NEVER */
            return;
        case 2:
            /* GEQUAL */
            if (z < zdst) {
                return;
            }
            break;
        case 3:
            /* GREATER */
            if (z <= zdst) {
                return;
            }
            break;
        }
    }

    dst = ps2_gs_to_rgba(c->fpsm,
                         ps2_gs_read_pixel(s, c->fpsm, c->fbp, c->fbw, x, y),
                         0x8000000080ULL);
    if (extract64(c->test, 14, 1) &&
        extract32(dst, 31, 1) != extract64(c->test, 15, 1)) {
        return;
    }

    if (zte && write_z) {
        ps2_gs_write_pixel(s, c->zpsm, c->zbp, c->fbw, x, y, z);
    }
    if (!write_fb) {
        return;
    }
    out = (c->prim & GS_PRIM_ABE) ? ps2_gs_blend(s, c, rgba, dst) : rgba;
    if (!write_alpha) {
        mask |= 0xff000000;
    }
    out = (out & ~mask) | (dst & mask);
    ps2_gs_write_pixel(s, c->fpsm, c->fbp, c->fbw, x, y,
                       ps2_gs_from_rgba(c->fpsm, out));
}

static float ps2_gs_float(uint32_t bits)
{
    union {
        uint32_t i;
        float f;
    } u = { .i = bits };

    return u.f;
}

/* A vertex in window coordinates with 4 fractional bits. */
typedef struct PS2GSVertex {
    int32_t x;
    int32_t y;
    uint32_t rgba;
    uint32_t z;
    uint32_t f;
    /* Texel coordinates with 4 fractional bits, or S, T and Q. */
    float u;
    float v;
    float q;
} PS2GSVertex;

static void ps2_gs_vertex(PS2GSState *s, const PS2GSContext *c,
                          unsigned int i, PS2GSVertex *v)
{
    uint64_t xyz = s->vtx_xyz[i], st = s->vtx_st[i], uv = s->vtx_uv[i];

    v->x = (int32_t)extract64(xyz, 0, 16) - c->ofx;
    v->y = (int32_t)extract64(xyz, 16, 16) - c->ofy;
    v->z = extract64(xyz, 32, 32);
    v->f = extract64(s->vtx_fog[i], 56, 8);
    v->rgba = s->vtx_rgbaq[i];
    if (c->prim & GS_PRIM_FST) {
        v->u = extract64(uv, 0, 14);
        v->v = extract64(uv, 16, 14);
        v->q = 1;
    } else {
        v->u = ps2_gs_float(st) * c->tw * 16;
        v->v = ps2_gs_float(st >> 32) * c->th * 16;
        v->q = ps2_gs_float(s->vtx_rgbaq[i] >> 32);
    }
}

static bool ps2_gs_clip(const PS2GSContext *c, int32_t *x0, int32_t *y0,
                        int32_t *x1, int32_t *y1)
{
    *x0 = MAX(*x0, c->scax0);
    *y0 = MAX(*y0, c->scay0);
    *x1 = MIN(*x1, c->scax1 + 1);
    *y1 = MIN(*y1, c->scay1 + 1);
    return *x0 < *x1 && *y0 < *y1;
}

static int32_t ps2_gs_texel_coord(float t, float q)
{
    return (int32_t)floorf((q != 0 ? t / q : t) / 16);
}

static void ps2_gs_draw_point(PS2GSState *s, const PS2GSContext *c,
                              const PS2GSVertex *v)
{
    int32_t x0 = (v->x + 8) >> 4, y0 = (v->y + 8) >> 4;
    int32_t x1 = x0 + 1, y1 = y0 + 1;

    if (ps2_gs_clip(c, &x0, &y0, &x1, &y1)) {
        ps2_gs_fragment(s, c, x0, y0, v->rgba,
                        ps2_gs_texel_coord(v->u, v->q),
                        ps2_gs_texel_coord(v->v, v->q), v->z, v->f);
    }
}

/* Interpolate between two vertices, @t of the way from @a to @b. */
static uint32_t ps2_gs_lerp_rgba(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0;

    for (unsigned int i = 0; i < 32; i += 8) {
        float ca = ps2_gs_channel(a, i), cb = ps2_gs_channel(b, i);

        out |= MIN((uint32_t)(ca + (cb - ca) * t + 0.5f), 0xff) << i;
    }
    return out;
}

/*
 * A line lights one pixel for each step along its major axis, from its
 * first vertex up to, but not including, its second.
 */
static void ps2_gs_draw_line(PS2GSState *s, const PS2GSContext *c,
                             const PS2GSVertex *v0, const PS2GSVertex *v1)
{
    int32_t dx = v1->x - v0->x, dy = v1->y - v0->y;
    bool x_major = abs(dx) >= abs(dy);
    int32_t a0 = x_major ? v0->x : v0->y, a1 = x_major ? v1->x : v1->y;
    int32_t p0 = (a0 + 8) >> 4, p1 = (a1 + 8) >> 4;
    int32_t step = p1 >= p0 ? 1 : -1;
    int32_t d = a1 - a0;

    if (!d) {
        return;
    }
    for (int32_t p = p0; p != p1; p += step) {
        float t = MIN(MAX((float)(p * 16 - a0) / d, 0.0f), 1.0f);
        int32_t x, y, x1, y1;
        uint32_t rgba = v1->rgba;
        float u = v0->u + (v1->u - v0->u) * t;
        float v = v0->v + (v1->v - v0->v) * t;
        float q = v0->q + (v1->q - v0->q) * t;
        double z = v0->z + ((double)v1->z - v0->z) * t;
        float f = v0->f + ((float)v1->f - v0->f) * t;

        if (x_major) {
            x = p;
            y = (int32_t)floorf((v0->y + dy * t + 8) / 16);
        } else {
            y = p;
            x = (int32_t)floorf((v0->x + dx * t + 8) / 16);
        }
        x1 = x + 1;
        y1 = y + 1;
        if (!ps2_gs_clip(c, &x, &y, &x1, &y1)) {
            continue;
        }
        if (c->prim & GS_PRIM_IIP) {
            rgba = ps2_gs_lerp_rgba(v0->rgba, v1->rgba, t);
        }
        ps2_gs_fragment(s, c, x, y, rgba, ps2_gs_texel_coord(u, q),
                        ps2_gs_texel_coord(v, q), z, f);
    }
}

/*
 * A sprite covers the pixels from its first to its second corner, with
 * texel coordinates interpolated linearly and the colour, depth and fog
 * of the second.
 */
static void ps2_gs_draw_sprite(PS2GSState *s, const PS2GSContext *c,
                               const PS2GSVertex *v0, const PS2GSVertex *v1)
{
    int32_t x0 = (MIN(v0->x, v1->x) + 15) >> 4;
    int32_t x1 = (MAX(v0->x, v1->x) + 15) >> 4;
    int32_t y0 = (MIN(v0->y, v1->y) + 15) >> 4;
    int32_t y1 = (MAX(v0->y, v1->y) + 15) >> 4;
    float u0 = v0->u / v0->q, du, vv0 = v0->v / v0->q, dv;

    if (!ps2_gs_clip(c, &x0, &y0, &x1, &y1)) {
        return;
    }
    du = (v1->u / v1->q - u0) / (v1->x - v0->x);
    dv = (v1->v / v1->q - vv0) / (v1->y - v0->y);

    for (int32_t y = y0; y < y1; y++) {
        int32_t tv = ps2_gs_texel_coord(vv0 + (y * 16 - v0->y) * dv, 1);

        for (int32_t x = x0; x < x1; x++) {
            int32_t tu = ps2_gs_texel_coord(u0 + (x * 16 - v0->x) * du, 1);

            ps2_gs_fragment(s, c, x, y, v1->rgba, tu, tv, v1->z, v1->f);
        }
    }
}

static int64_t ps2_gs_edge(const PS2GSVertex *a, const PS2GSVertex *b,
                           int32_t x, int32_t y)
{
    return (int64_t)(b->x - a->x) * (y - a->y) -
           (int64_t)(b->y - a->y) * (x - a->x);
}

static void ps2_gs_draw_triangle(PS2GSState *s, const PS2GSContext *c,
                                 const PS2GSVertex *v0, const PS2GSVertex *v1,
                                 const PS2GSVertex *v2)
{
    int64_t area = ps2_gs_edge(v0, v1, v2->x, v2->y);
    int32_t x0 = (MIN(MIN(v0->x, v1->x), v2->x) + 15) >> 4;
    int32_t x1 = (MAX(MAX(v0->x, v1->x), v2->x) + 15) >> 4;
    int32_t y0 = (MIN(MIN(v0->y, v1->y), v2->y) + 15) >> 4;
    int32_t y1 = (MAX(MAX(v0->y, v1->y), v2->y) + 15) >> 4;
    const PS2GSVertex *v[3] = { v0, v1, v2 };

    if (!area || !ps2_gs_clip(c, &x0, &y0, &x1, &y1)) {
        return;
    }

    for (int32_t y = y0; y < y1; y++) {
        for (int32_t x = x0; x < x1; x++) {
            int64_t w[3] = {
                ps2_gs_edge(v1, v2, x * 16, y * 16),
                ps2_gs_edge(v2, v0, x * 16, y * 16),
                ps2_gs_edge(v0, v1, x * 16, y * 16),
            };
            float tu = 0, tv = 0, tq = 0, tf = 0;
            double tz = 0;
            uint32_t rgba = v2->rgba;

            if (area < 0) {
                w[0] = -w[0];
                w[1] = -w[1];
                w[2] = -w[2];
            }
            if (w[0] < 0 || w[1] < 0 || w[2] < 0) {
                continue;
            }
            if (c->prim & GS_PRIM_IIP) {
                rgba = 0;
                for (unsigned int i = 0; i < 32; i += 8) {
                    int64_t ch = 0;

                    for (unsigned int k = 0; k < 3; k++) {
                        ch += w[k] * ps2_gs_channel(v[k]->rgba, i);
                    }
                    rgba |= (uint32_t)(ch / llabs(area)) << i;
                }
            }
            for (unsigned int k = 0; k < 3; k++) {
                double f = (double)w[k] / llabs(area);

                tu += f * v[k]->u;
                tv += f * v[k]->v;
                tq += f * v[k]->q;
                tz += f * v[k]->z;
                tf += f * v[k]->f;
            }
            ps2_gs_fragment(s, c, x, y, rgba, ps2_gs_texel_coord(tu, tq),
                            ps2_gs_texel_coord(tv, tq),
                            MIN(tz + 0.5, (double)UINT32_MAX), tf + 0.5f);
        }
    }
}

/* Queue a vertex, and draw the primitive it completes if @draw is set. */
static void ps2_gs_vertex_kick(PS2GSState *s, uint64_t xyz, bool draw)
{
    unsigned int type = s->regs[GS_PRIM] & 7;
    unsigned int n = s->vtx_count;
    PS2GSVertex v[3];
    PS2GSContext c;
    unsigned int need;

    s->vtx_xyz[n] = xyz;
    s->vtx_fog[n] = s->regs[GS_FOG];
    s->vtx_rgbaq[n] = s->regs[GS_RGBAQ];
    s->vtx_st[n] = s->regs[GS_ST];
    s->vtx_uv[n] = s->regs[GS_UV];
    s->vtx_count = ++n;

    switch (type) {
    case GS_PRIM_POINT:
        need = 1;
        break;
    case GS_PRIM_LINE:
    case GS_PRIM_LINE_STRIP:
    case GS_PRIM_SPRITE:
        need = 2;
        break;
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_TRIANGLE_STRIP:
    case GS_PRIM_TRIANGLE_FAN:
        need = 3;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: invalid primitive %u\n",
                      __func__, type);
        s->vtx_count = 0;
        return;
    }
    if (n < need) {
        return;
    }

    if (draw) {
        ps2_gs_context(s, &c);
        for (unsigned int i = 0; i < n; i++) {
            ps2_gs_vertex(s, &c, i, &v[i]);
        }
        switch (type) {
        case GS_PRIM_POINT:
            ps2_gs_draw_point(s, &c, &v[0]);
            break;
        case GS_PRIM_LINE:
        case GS_PRIM_LINE_STRIP:
            ps2_gs_draw_line(s, &c, &v[0], &v[1]);
            break;
        case GS_PRIM_SPRITE:
            ps2_gs_draw_sprite(s, &c, &v[0], &v[1]);
            break;
        default:
            ps2_gs_draw_triangle(s, &c, &v[0], &v[1], &v[2]);
            break;
        }
    }

    switch (type) {
    case GS_PRIM_LINE_STRIP:
    case GS_PRIM_TRIANGLE_STRIP:
        /* The last vertices start the next primitive. */
        for (unsigned int i = 1; i < n; i++) {
            s->vtx_xyz[i - 1] = s->vtx_xyz[i];
            s->vtx_fog[i - 1] = s->vtx_fog[i];
            s->vtx_rgbaq[i - 1] = s->vtx_rgbaq[i];
            s->vtx_st[i - 1] = s->vtx_st[i];
            s->vtx_uv[i - 1] = s->vtx_uv[i];
        }
        s->vtx_count = n - 1;
        break;
    case GS_PRIM_TRIANGLE_FAN:
        /* The first vertex is shared, the last one starts the next. */
        s->vtx_xyz[1] = s->vtx_xyz[2];
        s->vtx_fog[1] = s->vtx_fog[2];
        s->vtx_rgbaq[1] = s->vtx_rgbaq[2];
        s->vtx_st[1] = s->vtx_st[2];
        s->vtx_uv[1] = s->vtx_uv[2];
        s->vtx_count = 2;
        break;
    default:
        s->vtx_count = 0;
        break;
    }
}

static void ps2_gs_transfer_pixel(PS2GSState *s, uint32_t value)
{
    uint64_t bitbltbuf = s->regs[GS_BITBLTBUF];
    uint64_t trxpos = s->regs[GS_TRXPOS];
    uint64_t trxreg = s->regs[GS_TRXREG];
    uint32_t x = extract64(trxpos, 32, 11) + s->trx_x;
    uint32_t y = extract64(trxpos, 48, 11) + s->trx_y;

    ps2_gs_write_pixel(s, extract64(bitbltbuf, 56, 6),
                       extract64(bitbltbuf, 32, 14),
                       extract64(bitbltbuf, 48, 6),
                       x & GS_COORD_MASK, y & GS_COORD_MASK, value);

    if (++s->trx_x >= extract64(trxreg, 0, 12)) {
        s->trx_x = 0;
        if (++s->trx_y >= extract64(trxreg, 32, 12)) {
            s->trx_active = false;
        }
    }
}

/* Image data of a host to local transfer, from IMAGE packets or HWREG. */
static void ps2_gs_transfer_data(PS2GSState *s, const uint8_t *buf,
                                 uint32_t len)
{
    unsigned int bpp =
        ps2_gs_transfer_bpp(extract64(s->regs[GS_BITBLTBUF], 56, 6));

    for (uint32_t i = 0; i < len && s->trx_active; i++) {
        if (bpp == 4) {
            ps2_gs_transfer_pixel(s, buf[i] & 0xf);
            if (s->trx_active) {
                ps2_gs_transfer_pixel(s, buf[i] >> 4);
            }
            continue;
        }
        s->trx_acc |= buf[i] << (s->trx_acc_len * 8);
        if (++s->trx_acc_len == bpp / 8) {
            ps2_gs_transfer_pixel(s, s->trx_acc);
            s->trx_acc = 0;
            s->trx_acc_len = 0;
        }
    }
}

static void ps2_gs_transfer_local(PS2GSState *s)
{
    uint64_t bitbltbuf = s->regs[GS_BITBLTBUF];
    uint64_t trxpos = s->regs[GS_TRXPOS];
    uint64_t trxreg = s->regs[GS_TRXREG];
    uint32_t sbp = extract64(bitbltbuf, 0, 14);
    uint32_t sbw = extract64(bitbltbuf, 16, 6);
    uint32_t spsm = extract64(bitbltbuf, 24, 6);
    uint32_t dbp = extract64(bitbltbuf, 32, 14);
    uint32_t dbw = extract64(bitbltbuf, 48, 6);
    uint32_t dpsm = extract64(bitbltbuf, 56, 6);
    uint32_t sx = extract64(trxpos, 0, 11), sy = extract64(trxpos, 16, 11);
    uint32_t dx = extract64(trxpos, 32, 11), dy = extract64(trxpos, 48, 11);
    unsigned int dir = extract64(trxpos, 59, 2);
    uint32_t w = extract64(trxreg, 0, 12), h = extract64(trxreg, 32, 12);

    if (ps2_gs_transfer_bpp(spsm) != ps2_gs_transfer_bpp(dpsm)) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: PSM 0x%x to 0x%x\n", __func__,
                      spsm, dpsm);
        return;
    }

    /* Go in the direction that keeps overlapping areas intact. */
    for (uint32_t j = 0; j < h; j++) {
        uint32_t y = (dir & 1) ? h - 1 - j : j;

        for (uint32_t i = 0; i < w; i++) {
            uint32_t x = (dir & 2) ? w - 1 - i : i;
            uint32_t v = ps2_gs_read_pixel(s, spsm, sbp, sbw,
                                           (sx + x) & GS_COORD_MASK,
                                           (sy + y) & GS_COORD_MASK);

            ps2_gs_write_pixel(s, dpsm, dbp, dbw, (dx + x) & GS_COORD_MASK,
                               (dy + y) & GS_COORD_MASK, v);
        }
    }
}

/* The next pixel of a local to host transfer. */
static uint32_t ps2_gs_transfer_read_pixel(PS2GSState *s)
{
    uint64_t bitbltbuf = s->regs[GS_BITBLTBUF];
    uint64_t trxpos = s->regs[GS_TRXPOS];
    uint64_t trxreg = s->regs[GS_TRXREG];
    uint32_t x = extract64(trxpos, 0, 11) + s->trx_x;
    uint32_t y = extract64(trxpos, 16, 11) + s->trx_y;
    uint32_t v;

    v = ps2_gs_read_pixel(s, extract64(bitbltbuf, 24, 6),
                          extract64(bitbltbuf, 0, 14),
                          extract64(bitbltbuf, 16, 6),
                          x & GS_COORD_MASK, y & GS_COORD_MASK);

    if (++s->trx_x >= extract64(trxreg, 0, 12)) {
        s->trx_x = 0;
        if (++s->trx_y >= extract64(trxreg, 32, 12)) {
            s->trx_read_active = false;
        }
    }
    return v;
}

uint32_t ps2_gs_read_local(PS2GSState *s, uint8_t *buf, uint32_t len)
{
    unsigned int bpp =
        ps2_gs_transfer_bpp(extract64(s->regs[GS_BITBLTBUF], 24, 6));
    uint32_t n = 0;

    while (n < len && s->trx_read_active) {
        uint32_t v = ps2_gs_transfer_read_pixel(s);

        if (bpp == 4) {
            buf[n] = v & 0xf;
            if (s->trx_read_active) {
                buf[n] |= (ps2_gs_transfer_read_pixel(s) & 0xf) << 4;
            }
            n++;
            continue;
        }
        for (unsigned int i = 0; i < bpp / 8 && n < len; i++) {
            buf[n++] = v >> (i * 8);
        }
    }
    memset(buf + n, 0, len - n);
    return n;
}

static void ps2_gs_start_transfer(PS2GSState *s, unsigned int xdir)
{
    s->trx_active = false;
    s->trx_read_active = false;
    s->trx_x = 0;
    s->trx_y = 0;
    s->trx_acc = 0;
    s->trx_acc_len = 0;

    switch (xdir) {
    case GS_XDIR_HOST_LOCAL:
        if (!ps2_gs_psm_valid(extract64(s->regs[GS_BITBLTBUF], 56, 6))) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: bad transfer PSM\n",
                          __func__);
            return;
        }
        s->trx_active = extract64(s->regs[GS_TRXREG], 0, 12) &&
                        extract64(s->regs[GS_TRXREG], 32, 12);
        break;
    case GS_XDIR_LOCAL_HOST:
        if (!ps2_gs_psm_valid(extract64(s->regs[GS_BITBLTBUF], 24, 6))) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: bad transfer PSM\n",
                          __func__);
            return;
        }
        s->trx_read_active = extract64(s->regs[GS_TRXREG], 0, 12) &&
                             extract64(s->regs[GS_TRXREG], 32, 12);
        break;
    case GS_XDIR_LOCAL_LOCAL:
        ps2_gs_transfer_local(s);
        break;
    }
}

static void ps2_gs_write_reg(PS2GSState *s, unsigned int addr, uint64_t data)
{
    uint64_t *tex0;

    addr &= PS2_GS_NUM_REGS - 1;
    switch (addr) {
    case GS_PRIM:
        s->vtx_count = 0;
        break;
    case GS_XYZF2:
    case GS_XYZ2:
    case GS_XYZF3:
    case GS_XYZ3:
        s->regs[addr] = data;
        if (addr == GS_XYZF2 || addr == GS_XYZF3) {
            /* A 24-bit Z with the fog coefficient above it. */
            s->regs[GS_FOG] = data & 0xff00000000000000ULL;
            data &= ~0xff00000000000000ULL;
        }
        ps2_gs_vertex_kick(s, data, addr == GS_XYZF2 || addr == GS_XYZ2);
        return;
    case GS_TEX2_1:
    case GS_TEX2_2:
        /* The PSM and CLUT fields of TEX0. */
        tex0 = &s->regs[GS_TEX0_1 + addr - GS_TEX2_1];
        *tex0 = deposit64(*tex0, 20, 6, extract64(data, 20, 6));
        *tex0 = deposit64(*tex0, 37, 27, extract64(data, 37, 27));
        break;
    case GS_TRXDIR:
        s->regs[addr] = data;
        ps2_gs_start_transfer(s, data & 3);
        return;
    case GS_HWREG:
        if (s->trx_active) {
            uint8_t buf[8];

            stq_le_p(buf, data);
            ps2_gs_transfer_data(s, buf, sizeof(buf));
        }
        return;
    case GS_SIGNAL:
        s->siglblid = deposit64(s->siglblid, 0, 32,
                                (s->siglblid & ~(data >> 32)) |
                                (data & (data >> 32)));
        s->csr |= PS2_GS_CSR_SIGNAL;
        ps2_gs_update_irq(s);
        return;
    case GS_FINISH:
        s->csr |= PS2_GS_CSR_FINISH;
        ps2_gs_update_irq(s);
        return;
    case GS_LABEL:
        s->siglblid = deposit64(s->siglblid, 32, 32,
                                ((s->siglblid >> 32) & ~(data >> 32)) |
                                (data & (data >> 32)));
        return;
    }
    s->regs[addr] = data;
}

/* Unpack a PACKED mode quadword for register descriptor @reg. */
static void ps2_gs_write_packed(PS2GSState *s, unsigned int reg,
                                uint64_t lo, uint64_t hi)
{
    uint64_t data;

    switch (reg) {
    case GIF_REG_PRIM:
        ps2_gs_write_reg(s, GS_PRIM, lo & 0x7ff);
        break;
    case GIF_REG_RGBAQ:
        data = extract64(lo, 0, 8) | (extract64(lo, 32, 8) << 8) |
               (extract64(hi, 0, 8) << 16) | (extract64(hi, 32, 8) << 24) |
               ((uint64_t)s->q << 32);
        ps2_gs_write_reg(s, GS_RGBAQ, data);
        break;
    case GIF_REG_ST:
        s->q = hi;
        ps2_gs_write_reg(s, GS_ST, lo);
        break;
    case GIF_REG_UV:
        ps2_gs_write_reg(s, GS_UV, extract64(lo, 0, 14) |
                         (extract64(lo, 32, 14) << 16));
        break;
    case GIF_REG_XYZF2:
    case GIF_REG_XYZF3:
        data = extract64(lo, 0, 16) | (extract64(lo, 32, 16) << 16) |
               (extract64(hi, 4, 24) << 32) | (extract64(hi, 36, 8) << 56);
        ps2_gs_write_reg(s, extract64(hi, 47, 1) || reg == GIF_REG_XYZF3 ?
                         GS_XYZF3 : GS_XYZF2, data);
        break;
    case GIF_REG_XYZ2:
    case GIF_REG_XYZ3:
        data = extract64(lo, 0, 16) | (extract64(lo, 32, 16) << 16) |
               (extract64(hi, 0, 32) << 32);
        ps2_gs_write_reg(s, extract64(hi, 47, 1) || reg == GIF_REG_XYZ3 ?
                         GS_XYZ3 : GS_XYZ2, data);
        break;
    case GIF_REG_FOG:
        ps2_gs_write_reg(s, GS_FOG, extract64(hi, 36, 8) << 56);
        break;
    case GIF_REG_AD:
        ps2_gs_write_reg(s, hi & 0xff, lo);
        break;
    case GIF_REG_NOP:
        break;
    default:
        /* TEX0, CLAMP and the reserved descriptor go through as they are. */
        ps2_gs_write_reg(s, reg, lo);
        break;
    }
}

static unsigned int ps2_gs_gif_reg(PS2GSState *s)
{
    unsigned int reg = extract64(s->gif_regs, s->gif_reg * 4, 4);

    if (++s->gif_reg == s->gif_nreg) {
        s->gif_reg = 0;
        s->gif_nloop--;
    }
    return reg;
}

static void ps2_gs_gif_qword(PS2GSState *s, const uint8_t *qw)
{
    uint64_t lo = ldq_le_p(qw), hi = ldq_le_p(qw + 8);
    unsigned int reg;

    if (!s->gif_nloop) {
        /* A GIF tag. */
        s->gif_nloop = extract64(lo, 0, 15);
        s->gif_flg = extract64(lo, 58, 2);
        s->gif_nreg = extract64(lo, 60, 4) ?: 16;
        s->gif_regs = hi;
        s->gif_reg = 0;
        s->q = 0x3f800000;
        if (s->gif_flg == GIF_FLG_PACKED && extract64(lo, 46, 1)) {
            ps2_gs_write_reg(s, GS_PRIM, extract64(lo, 47, 11));
        }
        return;
    }

    switch (s->gif_flg) {
    case GIF_FLG_PACKED:
        ps2_gs_write_packed(s, ps2_gs_gif_reg(s), lo, hi);
        break;
    case GIF_FLG_REGLIST:
        /* Two registers a quadword; an odd count leaves a pad. */
        reg = ps2_gs_gif_reg(s);
        if (reg < GIF_REG_AD) {
            ps2_gs_write_reg(s, reg, lo);
        }
        if (s->gif_nloop) {
            reg = ps2_gs_gif_reg(s);
            if (reg < GIF_REG_AD) {
                ps2_gs_write_reg(s, reg, hi);
            }
        }
        break;
    default:
        /* IMAGE mode */
        if (s->trx_active) {
            ps2_gs_transfer_data(s, qw, 16);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: IMAGE data without a "
                          "transfer\n", __func__);
        }
        s->gif_nloop--;
        break;
    }
}

void ps2_gs_gif_write(PS2GSState *s, const uint8_t *buf, uint32_t len)
{
    while (len) {
        uint32_t n = MIN(len, sizeof(s->gif_qw) - s->gif_qw_len);

        memcpy(&s->gif_qw[s->gif_qw_len], buf, n);
        s->gif_qw_len += n;
        buf += n;
        len -= n;
        if (s->gif_qw_len == sizeof(s->gif_qw)) {
            ps2_gs_gif_qword(s, s->gif_qw);
            s->gif_qw_len = 0;
        }
    }
}

static void ps2_gs_dma_sink(void *opaque, const uint8_t *buf, uint32_t len)
{
    ps2_gs_gif_write(PS2_GS(opaque), buf, len);
}

static const PS2DMACChannelOps ps2_gs_dma_ops = {
    .sink = ps2_gs_dma_sink,
};

const PS2DMACChannelOps *ps2_gs_get_dma_ops(void)
{
    return &ps2_gs_dma_ops;
}

/* The area a read circuit outputs, in pixels of the frame buffer. */
typedef struct PS2GSCircuit {
    uint32_t fbp;
    uint32_t fbw;
    uint32_t psm;
    uint32_t dbx;
    uint32_t dby;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
} PS2GSCircuit;

static bool ps2_gs_circuit(PS2GSState *s, unsigned int n, PS2GSCircuit *c)
{
    uint64_t dispfb = s->disp[(n ? PS2_GS_DISPFB2 : PS2_GS_DISPFB1) >> 4];
    uint64_t display = s->disp[(n ? PS2_GS_DISPLAY2 : PS2_GS_DISPLAY1) >> 4];
    uint32_t magh = extract64(display, 23, 4) + 1;
    uint32_t magv = extract64(display, 27, 2) + 1;

    if (!extract64(s->disp[PS2_GS_PMODE >> 4], n, 1)) {
        return false;
    }
    c->fbp = extract64(dispfb, 0, 9) * GS_BLOCKS_PER_PAGE;
    c->fbw = extract64(dispfb, 9, 6);
    c->psm = extract64(dispfb, 15, 5);
    c->dbx = extract64(dispfb, 32, 11);
    c->dby = extract64(dispfb, 43, 11);
    c->x = extract64(display, 0, 12) / magh;
    c->y = extract64(display, 12, 11) / magv;
    c->w = (extract64(display, 32, 12) + 1) / magh;
    c->h = (extract64(display, 44, 11) + 1) / magv;
    return c->w > 0 && c->h > 0;
}

static void ps2_gs_draw_circuit(PS2GSState *s, DisplaySurface *surface,
                                const PS2GSCircuit *c, int32_t x0, int32_t y0)
{
    int32_t w = MIN(c->w, surface_width(surface) - (c->x - x0));
    int32_t h = MIN(c->h, surface_height(surface) - (c->y - y0));
    int stride = surface_stride(surface) / 4;
    uint32_t *row = (uint32_t *)surface_data(surface) +
                    (c->y - y0) * stride + (c->x - x0);

    for (int32_t y = 0; y < h; y++, row += stride) {
        for (int32_t x = 0; x < w; x++) {
            uint32_t v = ps2_gs_read_pixel(s, c->psm, c->fbp, c->fbw,
                                           (c->dbx + x) & GS_COORD_MASK,
                                           (c->dby + y) & GS_COORD_MASK);

            v = ps2_gs_to_rgba(c->psm, v, 0);
            row[x] = (ps2_gs_channel(v, 0) << 16) |
                     (ps2_gs_channel(v, 8) << 8) | ps2_gs_channel(v, 16);
        }
    }
}

static bool ps2_gs_update_display(void *opaque)
{
    PS2GSState *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    PS2GSCircuit c[2];
    bool on[2];
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = 0, y1 = 0;

    for (unsigned int i = 0; i < 2; i++) {
        on[i] = ps2_gs_circuit(s, i, &c[i]);
        if (on[i]) {
            x0 = MIN(x0, c[i].x);
            y0 = MIN(y0, c[i].y);
            x1 = MAX(x1, c[i].x + c[i].w);
            y1 = MAX(y1, c[i].y + c[i].h);
        }
    }
    if (!on[0] && !on[1]) {
        return true;
    }
    x1 = MIN(x1 - x0, 2048);
    y1 = MIN(y1 - y0, 2048);

    if (x1 != surface_width(surface) || y1 != surface_height(surface)) {
        qemu_console_resize(s->con, x1, y1);
        surface = qemu_console_surface(s->con);
        s->dirty = true;
    }
    if (!s->dirty) {
        return true;
    }
    s->dirty = false;

    memset(surface_data(surface), 0, surface_stride(surface) * y1);
    /* The first read circuit is in front. */
    for (int i = 1; i >= 0; i--) {
        if (on[i]) {
            ps2_gs_draw_circuit(s, surface, &c[i], x0, y0);
        }
    }
    qemu_console_update(s->con, 0, 0, x1, y1);
    return true;
}

static void ps2_gs_invalidate_display(void *opaque)
{
    PS2GSState *s = opaque;

    s->dirty = true;
}

static const GraphicHwOps ps2_gs_gfx_ops = {
    .invalidate = ps2_gs_invalidate_display,
    .gfx_update = ps2_gs_update_display,
};

static uint64_t ps2_gs_priv_read(void *opaque, hwaddr addr, unsigned int size)
{
    PS2GSState *s = opaque;
    hwaddr reg = addr & 0x1ff0;
    unsigned int shift = (addr & 4) * 8;
    uint64_t value;

    switch (reg) {
    case PS2_GS_CSR:
        value = s->csr | PS2_GS_CSR_FIFO_EMPTY |
                ((uint64_t)PS2_GS_CSR_REV << 16) |
                ((uint64_t)PS2_GS_CSR_ID << 24);
        break;
    case PS2_GS_IMR:
        value = s->imr;
        break;
    case PS2_GS_BUSDIR:
        value = s->busdir;
        break;
    case PS2_GS_SIGLBLID:
        value = s->siglblid;
        break;
    default:
        if (reg <= PS2_GS_BGCOLOR) {
            value = s->disp[reg >> 4];
        } else {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx
                          "\n", __func__, addr);
            value = 0;
        }
        break;
    }
    return size == 8 ? value : extract64(value, shift, size * 8);
}

static void ps2_gs_priv_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    PS2GSState *s = opaque;
    hwaddr reg = addr & 0x1ff0;
    uint64_t *r;

    switch (reg) {
    case PS2_GS_CSR:
        if (addr & 4) {
            return;
        }
        s->csr &= ~(value & PS2_GS_CSR_EVENTS);
        if (value & PS2_GS_CSR_RESET) {
            s->csr &= ~PS2_GS_CSR_EVENTS;
        }
        ps2_gs_update_irq(s);
        return;
    case PS2_GS_IMR:
        r = &s->imr;
        break;
    case PS2_GS_BUSDIR:
        r = &s->busdir;
        break;
    case PS2_GS_SIGLBLID:
        r = &s->siglblid;
        break;
    default:
        if (reg > PS2_GS_BGCOLOR) {
            qemu_log_mask(LOG_GUEST_ERROR, "%s: bad offset 0x%" HWADDR_PRIx
                          "\n", __func__, addr);
            return;
        }
        r = &s->disp[reg >> 4];
        s->dirty = true;
        break;
    }

    if (size == 8) {
        *r = value;
    } else {
        *r = deposit64(*r, (addr & 4) * 8, size * 8, value);
    }
    if (r == &s->imr) {
        ps2_gs_update_irq(s);
    }
}

static const MemoryRegionOps ps2_gs_priv_ops = {
    .read = ps2_gs_priv_read,
    .write = ps2_gs_priv_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

static void ps2_gs_gif_reset(PS2GSState *s)
{
    s->gif_nloop = 0;
    s->gif_qw_len = 0;
    s->trx_active = false;
    s->trx_read_active = false;
}

static uint64_t ps2_gif_read(void *opaque, hwaddr addr, unsigned int size)
{
    PS2GSState *s = opaque;

    switch (addr) {
    case PS2_GIF_CTRL:
        return s->gif_ctrl;
    case PS2_GIF_MODE:
        return s->gif_mode;
    case PS2_GIF_STAT:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        return 0;
    }
}

static void ps2_gif_write(void *opaque, hwaddr addr, uint64_t value,
                          unsigned int size)
{
    PS2GSState *s = opaque;

    switch (addr) {
    case PS2_GIF_CTRL:
        if (value & PS2_GIF_CTRL_RST) {
            ps2_gs_gif_reset(s);
        }
        s->gif_ctrl = value & ~PS2_GIF_CTRL_RST;
        break;
    case PS2_GIF_MODE:
        s->gif_mode = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented offset 0x%" HWADDR_PRIx
                      "\n", __func__, addr);
        break;
    }
}

static const MemoryRegionOps ps2_gif_ops = {
    .read = ps2_gif_read,
    .write = ps2_gif_write,
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

static void ps2_gs_reset_hold(Object *obj, ResetType type)
{
    PS2GSState *s = PS2_GS(obj);

    memset(s->disp, 0, sizeof(s->disp));
    s->csr = 0;
    s->imr = 0x7f00;
    s->busdir = 0;
    s->siglblid = 0;
    s->gif_ctrl = 0;
    s->gif_mode = 0;
    ps2_gs_gif_reset(s);
    memset(s->regs, 0, sizeof(s->regs));
    s->q = 0x3f800000;
    s->vtx_count = 0;
    memset(s->vram, 0, sizeof(s->vram));
    s->dirty = true;
    ps2_gs_update_irq(s);
    timer_del(s->vblank_end_timer);
    timer_mod(s->vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              ps2_gs_frame_ns(s));
}

static void ps2_gs_init(Object *obj)
{
    PS2GSState *s = PS2_GS(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->priv_iomem, obj, &ps2_gs_priv_ops, s,
                          TYPE_PS2_GS, PS2_GS_PRIV_SIZE);
    sysbus_init_mmio(sbd, &s->priv_iomem);
    memory_region_init_io(&s->gif_iomem, obj, &ps2_gif_ops, s,
                          TYPE_PS2_GS "-gif", PS2_GIF_SIZE);
    sysbus_init_mmio(sbd, &s->gif_iomem);

    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->vblank_start);
    sysbus_init_irq(sbd, &s->vblank_end);

    s->vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ps2_gs_vblank, s);
    s->vblank_end_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                       ps2_gs_vblank_end, s);
}

static void ps2_gs_realize(DeviceState *dev, Error **errp)
{
    PS2GSState *s = PS2_GS(dev);

    s->con = qemu_graphic_console_create(dev, 0, &ps2_gs_gfx_ops, s);
}

static void ps2_gs_finalize(Object *obj)
{
    PS2GSState *s = PS2_GS(obj);

    timer_free(s->vblank_timer);
    timer_free(s->vblank_end_timer);
}

static int ps2_gs_post_load(void *opaque, int version_id)
{
    PS2GSState *s = opaque;

    if (s->gif_qw_len >= sizeof(s->gif_qw) || s->gif_reg >= s->gif_nreg ||
        s->gif_nreg > 16 || s->vtx_count >= ARRAY_SIZE(s->vtx_xyz) ||
        s->trx_acc_len >= 4) {
        return -EINVAL;
    }
    s->dirty = true;
    return 0;
}

static const VMStateDescription vmstate_ps2_gs = {
    .name = TYPE_PS2_GS,
    .version_id = 3,
    .minimum_version_id = 3,
    .post_load = ps2_gs_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(disp, PS2GSState, PS2_GS_NUM_DISP_REGS),
        VMSTATE_UINT64(csr, PS2GSState),
        VMSTATE_UINT64(imr, PS2GSState),
        VMSTATE_UINT64(busdir, PS2GSState),
        VMSTATE_UINT64(siglblid, PS2GSState),
        VMSTATE_UINT32(gif_ctrl, PS2GSState),
        VMSTATE_UINT32(gif_mode, PS2GSState),
        VMSTATE_UINT64(gif_regs, PS2GSState),
        VMSTATE_UINT32(gif_nloop, PS2GSState),
        VMSTATE_UINT32(gif_flg, PS2GSState),
        VMSTATE_UINT32(gif_nreg, PS2GSState),
        VMSTATE_UINT32(gif_reg, PS2GSState),
        VMSTATE_UINT8_ARRAY(gif_qw, PS2GSState, 16),
        VMSTATE_UINT32(gif_qw_len, PS2GSState),
        VMSTATE_UINT64_ARRAY(regs, PS2GSState, PS2_GS_NUM_REGS),
        VMSTATE_UINT32(q, PS2GSState),
        VMSTATE_UINT64_ARRAY(vtx_xyz, PS2GSState, 3),
        VMSTATE_UINT64_ARRAY(vtx_fog, PS2GSState, 3),
        VMSTATE_UINT64_ARRAY(vtx_rgbaq, PS2GSState, 3),
        VMSTATE_UINT64_ARRAY(vtx_st, PS2GSState, 3),
        VMSTATE_UINT64_ARRAY(vtx_uv, PS2GSState, 3),
        VMSTATE_UINT32(vtx_count, PS2GSState),
        VMSTATE_BOOL(trx_active, PS2GSState),
        VMSTATE_UINT32(trx_x, PS2GSState),
        VMSTATE_UINT32(trx_y, PS2GSState),
        VMSTATE_UINT32(trx_acc, PS2GSState),
        VMSTATE_UINT32(trx_acc_len, PS2GSState),
        VMSTATE_BOOL(trx_read_active, PS2GSState),
        VMSTATE_BUFFER(vram, PS2GSState),
        VMSTATE_TIMER_PTR(vblank_timer, PS2GSState),
        VMSTATE_TIMER_PTR(vblank_end_timer, PS2GSState),
        VMSTATE_END_OF_LIST()
    },
};

static void ps2_gs_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->realize = ps2_gs_realize;
    rc->phases.hold = ps2_gs_reset_hold;
    dc->vmsd = &vmstate_ps2_gs;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo ps2_gs_types[] = {
    {
        .name = TYPE_PS2_GS,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PS2GSState),
        .instance_init = ps2_gs_init,
        .instance_finalize = ps2_gs_finalize,
        .class_init = ps2_gs_class_init,
    },
};

DEFINE_TYPES(ps2_gs_types);
