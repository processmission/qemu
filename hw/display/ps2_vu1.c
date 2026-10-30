/*
 * Sony PlayStation 2 vector unit 1 (VU1) and its interface (VIF1)
 *
 * VIF1 takes VIFcodes from DMAC channel 1 or its FIFO: it loads VU1
 * programs and data, starts VU1 and passes GIF packets to the GS (PATH2).
 * In the other direction, it reads the GS local memory for DMAC channel 1.
 * A VU1 program runs to completion when it is started.  Each instruction
 * pair takes one step; the upper instruction sees the registers as they
 * were before the lower one.  FDIV and EFU results come after a latency
 * and XGKICK sends its GIF packets to the GS at once (PATH1).  Floating
 * point follows the VU in clamping infinities and denormals, but rounds
 * the way the host does.
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/display/ps2_vu1.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include <math.h>

#define VIF_REG(off)            ((off) >> 4)

#define VU1_DATA_QWORDS         (PS2_VU1_MEM_SIZE / 16)
#define VU1_MICRO_PAIRS         (PS2_VU1_MEM_SIZE / 8)
/* Stop a program that does not end. */
#define VU1_MAX_STEPS           (16 * 1024 * 1024)

/* Upper instruction bits. */
#define VU1_UPPER_I             (1u << 31)
#define VU1_UPPER_E             (1u << 30)

/* MAC flags: zero and sign, with x in the highest bit of each nibble. */
#define VU1_MAC_Z(f)            (1u << (3 - (f)))
#define VU1_MAC_S(f)            (1u << (7 - (f)))
#define VU1_STATUS_Z            (1u << 0)
#define VU1_STATUS_S            (1u << 1)
#define VU1_STATUS_STICKY_SHIFT 6

/* FDIV latencies, in instruction pairs. */
#define VU1_DIV_LATENCY         7
#define VU1_RSQRT_LATENCY       13
#define VU1_EFU_LATENCY         12

enum {
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4a,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
    VIF_UNPACK = 0x60,
};

/* The arithmetic of upper instructions. */
enum {
    VU_ADD,
    VU_SUB,
    VU_MADD,
    VU_MSUB,
    VU_MUL,
    VU_MAX,
    VU_MINI,
};

/* The result of an upper instruction, committed after the lower one. */
typedef struct VU1Result {
    uint32_t v[4];
    uint32_t dest;
    /* A VF register, or -1 for ACC, or -2 for nothing. */
    int reg;
    bool flags;
    bool clip;
    uint32_t clip_flag;
} VU1Result;

#define VU1_RESULT_ACC          (-1)
#define VU1_RESULT_NONE         (-2)

/* VU floats have no infinities, NaNs or denormals. */
static float vu_f(uint32_t v)
{
    uint32_t exp = extract32(v, 23, 8);

    if (exp == 0) {
        v &= 0x80000000;
    } else if (exp == 0xff) {
        v = (v & 0x80000000) | 0x7f7fffff;
    }
    return *(float *)&v;
}

static uint32_t vu_u(float f)
{
    uint32_t v = *(uint32_t *)&f;
    uint32_t exp = extract32(v, 23, 8);

    if (exp == 0) {
        v &= 0x80000000;
    } else if (exp == 0xff) {
        v = (v & 0x80000000) | 0x7f7fffff;
    }
    return v;
}

static uint32_t *vu1_data_qword(PS2VU1State *s, uint32_t addr)
{
    uint32_t *mem = memory_region_get_ram_ptr(&s->data);

    return mem + (addr % VU1_DATA_QWORDS) * 4;
}

static void vu1_set_vi(PS2VU1State *s, unsigned int reg, uint32_t v)
{
    if (reg) {
        s->vi[reg] = v;
    }
}

static void vu1_set_vf(PS2VU1State *s, unsigned int reg, unsigned int dest,
                       const uint32_t *v)
{
    if (!reg) {
        return;
    }
    for (int f = 0; f < 4; f++) {
        if (dest & (8 >> f)) {
            s->vf[reg][f] = v[f];
        }
    }
}

static float vu_arith(int op, float acc, float a, float b)
{
    switch (op) {
    case VU_ADD:
        return a + b;
    case VU_SUB:
        return a - b;
    case VU_MADD:
        return acc + a * b;
    case VU_MSUB:
        return acc - a * b;
    case VU_MUL:
        return a * b;
    case VU_MAX:
        return a > b ? a : b;
    case VU_MINI:
        return a < b ? a : b;
    }
    g_assert_not_reached();
}

/* An upper arithmetic instruction: fs op b, with b by field. */
static void vu1_upper_arith(PS2VU1State *s, VU1Result *r, int op,
                            const uint32_t *a, const uint32_t *b)
{
    for (int f = 0; f < 4; f++) {
        if (r->dest & (8 >> f)) {
            r->v[f] = vu_u(vu_arith(op, vu_f(s->acc[f]), vu_f(a[f]),
                                    vu_f(b[f])));
        }
    }
    r->flags = op != VU_MAX && op != VU_MINI;
}

static void vu1_commit_flags(PS2VU1State *s, const VU1Result *r)
{
    uint32_t mac = 0, status;

    for (int f = 0; f < 4; f++) {
        if (!(r->dest & (8 >> f))) {
            continue;
        }
        if (!(r->v[f] & 0x7fffffff)) {
            mac |= VU1_MAC_Z(f);
        }
        if (r->v[f] & 0x80000000) {
            mac |= VU1_MAC_S(f);
        }
    }
    s->mac = mac;
    status = (mac & 0xf ? VU1_STATUS_Z : 0) | (mac & 0xf0 ? VU1_STATUS_S : 0);
    s->status = (s->status & ~0x3f) | status |
                (status << VU1_STATUS_STICKY_SHIFT);
}

static void vu1_commit(PS2VU1State *s, const VU1Result *r)
{
    if (r->reg == VU1_RESULT_ACC) {
        for (int f = 0; f < 4; f++) {
            if (r->dest & (8 >> f)) {
                s->acc[f] = r->v[f];
            }
        }
    } else if (r->reg >= 0) {
        vu1_set_vf(s, r->reg, r->dest, r->v);
    }
    if (r->flags) {
        vu1_commit_flags(s, r);
    }
    if (r->clip) {
        s->clip = r->clip_flag;
    }
}

static void vu1_upper(PS2VU1State *s, uint32_t insn, VU1Result *r)
{
    unsigned int op = extract32(insn, 0, 6);
    unsigned int ft = extract32(insn, 16, 5);
    unsigned int fs = extract32(insn, 11, 5);
    unsigned int fd = extract32(insn, 6, 5);
    unsigned int bc = extract32(insn, 0, 2);
    const uint32_t *a = s->vf[fs];
    uint32_t b[4], a2[4];
    static const int bc_ops[] = {
        VU_ADD, VU_SUB, VU_MADD, VU_MSUB, VU_MAX, VU_MINI, VU_MUL,
    };
    static const int qi_ops[] = {
        [0x20] = VU_ADD, [0x21] = VU_MADD, [0x22] = VU_ADD, [0x23] = VU_MADD,
        [0x24] = VU_SUB, [0x25] = VU_MSUB, [0x26] = VU_SUB, [0x27] = VU_MSUB,
    };
    static const int reg_ops[] = {
        VU_ADD, VU_MADD, VU_MUL, VU_MAX, VU_SUB, VU_MSUB, -1, VU_MINI,
    };
    static const int acc_qi_ops[] = {
        VU_ADD, VU_MADD, VU_ADD, VU_MADD, VU_SUB, VU_MSUB, VU_SUB, VU_MSUB,
    };
    unsigned int row, col;

    r->dest = extract32(insn, 21, 4);
    r->reg = fd;
    r->flags = false;
    r->clip = false;

    if (op < 0x1c) {
        for (int f = 0; f < 4; f++) {
            b[f] = s->vf[ft][bc];
        }
        vu1_upper_arith(s, r, bc_ops[op >> 2], a, b);
        return;
    }
    if (op < 0x28) {
        bool use_i;
        int aop;

        switch (op) {
        case 0x1c:
            use_i = false;
            aop = VU_MUL;
            break;
        case 0x1d:
            use_i = true;
            aop = VU_MAX;
            break;
        case 0x1e:
            use_i = true;
            aop = VU_MUL;
            break;
        case 0x1f:
            use_i = true;
            aop = VU_MINI;
            break;
        default:
            use_i = op & 2;
            aop = qi_ops[op];
            break;
        }
        for (int f = 0; f < 4; f++) {
            b[f] = use_i ? s->i : s->q;
        }
        vu1_upper_arith(s, r, aop, a, b);
        return;
    }
    if (op < 0x30) {
        if (op == 0x2e) {
            /* OPMSUB: ACC minus the cross product terms. */
            a2[0] = a[1];
            a2[1] = a[2];
            a2[2] = a[0];
            b[0] = s->vf[ft][2];
            b[1] = s->vf[ft][0];
            b[2] = s->vf[ft][1];
            a2[3] = b[3] = 0;
            vu1_upper_arith(s, r, VU_MSUB, a2, b);
        } else {
            vu1_upper_arith(s, r, reg_ops[op - 0x28], a, s->vf[ft]);
        }
        return;
    }
    if (op < 0x3c) {
        qemu_log_mask(LOG_GUEST_ERROR, "ps2-vu1: bad upper 0x%08x\n", insn);
        r->reg = VU1_RESULT_NONE;
        return;
    }

    row = extract32(insn, 6, 5);
    col = bc;
    r->reg = VU1_RESULT_ACC;
    switch (row) {
    case 0: case 1: case 2: case 3: case 6:
        /* ADDAbc, SUBAbc, MADDAbc, MSUBAbc, MULAbc */
        for (int f = 0; f < 4; f++) {
            b[f] = s->vf[ft][col];
        }
        vu1_upper_arith(s, r, row == 6 ? VU_MUL : bc_ops[row], a, b);
        break;
    case 4:
    case 5: {
        static const float scale[] = { 1.0f, 16.0f, 4096.0f, 32768.0f };

        /* ITOFn and FTOIn */
        r->reg = ft;
        for (int f = 0; f < 4; f++) {
            if (row == 4) {
                r->v[f] = vu_u((float)(int32_t)a[f] / scale[col]);
            } else {
                float x = vu_f(a[f]) * scale[col];

                if (x >= 2147483647.0f) {
                    r->v[f] = INT32_MAX;
                } else if (x <= -2147483648.0f) {
                    r->v[f] = INT32_MIN;
                } else {
                    r->v[f] = (int32_t)x;
                }
            }
        }
        break;
    }
    case 7:
        switch (col) {
        case 0:
        case 2:
            /* MULAq, MULAi */
            for (int f = 0; f < 4; f++) {
                b[f] = col ? s->i : s->q;
            }
            vu1_upper_arith(s, r, VU_MUL, a, b);
            break;
        case 1:
            /* ABS */
            r->reg = ft;
            for (int f = 0; f < 4; f++) {
                r->v[f] = vu_u(vu_f(a[f])) & 0x7fffffff;
            }
            break;
        case 3: {
            /* CLIPw: compare x, y and z against +-|w| of ft. */
            float w = fabsf(vu_f(s->vf[ft][3]));
            uint32_t judge = 0;

            for (int f = 0; f < 3; f++) {
                float x = vu_f(a[f]);

                if (x > w) {
                    judge |= 1 << (f * 2);
                }
                if (x < -w) {
                    judge |= 2 << (f * 2);
                }
            }
            r->reg = VU1_RESULT_NONE;
            r->clip = true;
            r->clip_flag = ((s->clip << 6) | judge) & 0xffffff;
            break;
        }
        }
        break;
    case 8:
    case 9:
        /* ADDAq, MADDAq, ADDAi, MADDAi, SUBAq, MSUBAq, SUBAi, MSUBAi */
        for (int f = 0; f < 4; f++) {
            b[f] = col & 2 ? s->i : s->q;
        }
        vu1_upper_arith(s, r, acc_qi_ops[(row - 8) * 4 + col], a, b);
        break;
    case 10:
    case 11:
        if (row == 11 && col == 3) {
            /* NOP */
            r->reg = VU1_RESULT_NONE;
        } else if (row == 11 && col == 2) {
            /* OPMULA */
            a2[0] = a[1];
            a2[1] = a[2];
            a2[2] = a[0];
            b[0] = s->vf[ft][2];
            b[1] = s->vf[ft][0];
            b[2] = s->vf[ft][1];
            a2[3] = b[3] = 0;
            vu1_upper_arith(s, r, VU_MUL, a2, b);
        } else if (row == 10 && col == 3) {
            r->reg = VU1_RESULT_NONE;
        } else {
            static const int ops[] = {
                VU_ADD, VU_MADD, VU_MUL, -1, VU_SUB, VU_MSUB,
            };

            /* ADDA, MADDA, MULA, SUBA, MSUBA */
            vu1_upper_arith(s, r, ops[(row - 10) * 4 + col], a, s->vf[ft]);
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "ps2-vu1: bad upper 0x%08x\n", insn);
        r->reg = VU1_RESULT_NONE;
        break;
    }
}

/* Send the GIF packets at a VU1 data address to the GS, up to EOP. */
static void vu1_xgkick(PS2VU1State *s, uint32_t addr)
{
    uint32_t sent = 0;
    bool eop = false;

    while (!eop && sent < VU1_DATA_QWORDS) {
        uint32_t *tag = vu1_data_qword(s, addr);
        uint32_t nloop = extract32(tag[0], 0, 15);
        uint32_t flg = extract32(tag[1], 26, 2);
        uint32_t nreg = extract32(tag[1], 28, 4);
        uint32_t qwords;

        eop = extract32(tag[0], 15, 1);
        if (!nreg) {
            nreg = 16;
        }
        switch (flg) {
        case 0:
            qwords = nloop * nreg;
            break;
        case 1:
            qwords = DIV_ROUND_UP(nloop * nreg, 2);
            break;
        default:
            qwords = nloop;
            break;
        }
        for (uint32_t n = 0; n <= qwords && sent < VU1_DATA_QWORDS; n++) {
            uint8_t qw[16];
            uint32_t *src = vu1_data_qword(s, addr + n);

            for (int w = 0; w < 4; w++) {
                stl_le_p(qw + w * 4, src[w]);
            }
            ps2_gs_gif_write(s->gs, qw, sizeof(qw));
            sent++;
        }
        addr += qwords + 1;
    }
}

static float vu1_efu(unsigned int row, unsigned int col, const uint32_t *v,
                     unsigned int fsf)
{
    float x = vu_f(v[0]), y = vu_f(v[1]), z = vu_f(v[2]), w = vu_f(v[3]);
    float e = vu_f(v[fsf]);

    switch (row << 2 | col) {
    case 0x1c << 2 | 0:
        return x * x + y * y + z * z;           /* ESADD */
    case 0x1c << 2 | 1:
        return 1.0f / (x * x + y * y + z * z);  /* ERSADD */
    case 0x1c << 2 | 2:
        return sqrtf(x * x + y * y + z * z);    /* ELENG */
    case 0x1c << 2 | 3:
        return 1.0f / sqrtf(x * x + y * y + z * z); /* ERLENG */
    case 0x1d << 2 | 0:
        return atanf(y / x);                    /* EATANxy */
    case 0x1d << 2 | 1:
        return atanf(z / x);                    /* EATANxz */
    case 0x1d << 2 | 2:
        return x + y + z + w;                   /* ESUM */
    case 0x1e << 2 | 0:
        return sqrtf(fabsf(e));                 /* ESQRT */
    case 0x1e << 2 | 1:
        return 1.0f / sqrtf(fabsf(e));          /* ERSQRT */
    case 0x1e << 2 | 2:
        return 1.0f / e;                        /* ERCPR */
    case 0x1f << 2 | 0:
        return sinf(e);                         /* ESIN */
    case 0x1f << 2 | 1:
        return atanf(e);                        /* EATAN */
    case 0x1f << 2 | 2:
        return expf(-e);                        /* EEXP */
    }
    return 0;
}

static void vu1_set_q(PS2VU1State *s, float q, uint32_t latency)
{
    s->q_next = vu_u(q);
    s->q_wait = latency;
}

/* The special lower instructions of opcode 0x40 with function 0x3c-0x3f. */
static void vu1_lower_t3(PS2VU1State *s, uint32_t insn)
{
    unsigned int row = extract32(insn, 6, 5);
    unsigned int col = extract32(insn, 0, 2);
    unsigned int dest = extract32(insn, 21, 4);
    unsigned int ft = extract32(insn, 16, 5);
    unsigned int fs = extract32(insn, 11, 5);
    unsigned int ftf = extract32(insn, 23, 2);
    unsigned int fsf = extract32(insn, 21, 2);
    uint32_t v[4], *mem;

    switch (row << 2 | col) {
    case 0x0c << 2 | 0:
        /* MOVE */
        vu1_set_vf(s, ft, dest, s->vf[fs]);
        break;
    case 0x0c << 2 | 1:
        /* MR32 */
        v[0] = s->vf[fs][1];
        v[1] = s->vf[fs][2];
        v[2] = s->vf[fs][3];
        v[3] = s->vf[fs][0];
        vu1_set_vf(s, ft, dest, v);
        break;
    case 0x0d << 2 | 0:
        /* LQI ft, (is++) */
        vu1_set_vf(s, ft, dest, vu1_data_qword(s, s->vi[fs & 15]));
        vu1_set_vi(s, fs & 15, s->vi[fs & 15] + 1);
        break;
    case 0x0d << 2 | 1:
        /* SQI fs, (it++) */
        mem = vu1_data_qword(s, s->vi[ft & 15]);
        for (int f = 0; f < 4; f++) {
            if (dest & (8 >> f)) {
                mem[f] = s->vf[fs][f];
            }
        }
        vu1_set_vi(s, ft & 15, s->vi[ft & 15] + 1);
        break;
    case 0x0d << 2 | 2:
        /* LQD ft, (--is) */
        vu1_set_vi(s, fs & 15, s->vi[fs & 15] - 1);
        vu1_set_vf(s, ft, dest, vu1_data_qword(s, s->vi[fs & 15]));
        break;
    case 0x0d << 2 | 3:
        /* SQD fs, (--it) */
        vu1_set_vi(s, ft & 15, s->vi[ft & 15] - 1);
        mem = vu1_data_qword(s, s->vi[ft & 15]);
        for (int f = 0; f < 4; f++) {
            if (dest & (8 >> f)) {
                mem[f] = s->vf[fs][f];
            }
        }
        break;
    case 0x0e << 2 | 0:
        /* DIV */
        vu1_set_q(s, vu_f(s->vf[fs][fsf]) / vu_f(s->vf[ft][ftf]),
                  VU1_DIV_LATENCY);
        break;
    case 0x0e << 2 | 1:
        /* SQRT */
        vu1_set_q(s, sqrtf(fabsf(vu_f(s->vf[ft][ftf]))), VU1_DIV_LATENCY);
        break;
    case 0x0e << 2 | 2:
        /* RSQRT */
        vu1_set_q(s, vu_f(s->vf[fs][fsf]) /
                  sqrtf(fabsf(vu_f(s->vf[ft][ftf]))), VU1_RSQRT_LATENCY);
        break;
    case 0x0e << 2 | 3:
        /* WAITQ */
        if (s->q_wait) {
            s->q = s->q_next;
            s->q_wait = 0;
        }
        break;
    case 0x0f << 2 | 0:
        /* MTIR */
        vu1_set_vi(s, ft & 15, s->vf[fs][fsf] & 0xffff);
        break;
    case 0x0f << 2 | 1:
        /* MFIR */
        for (int f = 0; f < 4; f++) {
            v[f] = (int16_t)s->vi[fs & 15];
        }
        vu1_set_vf(s, ft, dest, v);
        break;
    case 0x0f << 2 | 2:
        /* ILWR */
        mem = vu1_data_qword(s, s->vi[fs & 15]);
        for (int f = 0; f < 4; f++) {
            if (dest & (8 >> f)) {
                vu1_set_vi(s, ft & 15, mem[f] & 0xffff);
                break;
            }
        }
        break;
    case 0x0f << 2 | 3:
        /* ISWR */
        mem = vu1_data_qword(s, s->vi[fs & 15]);
        for (int f = 0; f < 4; f++) {
            if (dest & (8 >> f)) {
                mem[f] = s->vi[ft & 15];
            }
        }
        break;
    case 0x10 << 2 | 0:
        /* RNEXT */
        s->r = 0x3f800000 | (((s->r << 1) |
               (extract32(s->r, 4, 1) ^ extract32(s->r, 22, 1))) & 0x7fffff);
        /* fall through */
    case 0x10 << 2 | 1:
        /* RGET */
        for (int f = 0; f < 4; f++) {
            v[f] = s->r;
        }
        vu1_set_vf(s, ft, dest, v);
        break;
    case 0x10 << 2 | 2:
        /* RINIT */
        s->r = 0x3f800000 | (s->vf[fs][fsf] & 0x7fffff);
        break;
    case 0x10 << 2 | 3:
        /* RXOR */
        s->r = 0x3f800000 | ((s->r ^ s->vf[fs][fsf]) & 0x7fffff);
        break;
    case 0x19 << 2 | 0:
        /* MFP */
        for (int f = 0; f < 4; f++) {
            v[f] = s->p;
        }
        vu1_set_vf(s, ft, dest, v);
        break;
    case 0x1a << 2 | 0:
        /* XTOP */
        vu1_set_vi(s, ft & 15, s->regs[VIF_REG(PS2_VIF1_TOP)]);
        break;
    case 0x1a << 2 | 1:
        /* XITOP */
        vu1_set_vi(s, ft & 15, s->regs[VIF_REG(PS2_VIF1_ITOP)]);
        break;
    case 0x1b << 2 | 0:
        /* XGKICK */
        vu1_xgkick(s, s->vi[fs & 15]);
        break;
    case 0x1e << 2 | 3:
        /* WAITP */
        if (s->p_wait) {
            s->p = s->p_next;
            s->p_wait = 0;
        }
        break;
    default:
        if (row >= 0x1c) {
            s->p_next = vu_u(vu1_efu(row, col, s->vf[fs], fsf));
            s->p_wait = VU1_EFU_LATENCY;
            break;
        }
        qemu_log_mask(LOG_GUEST_ERROR, "ps2-vu1: bad lower 0x%08x\n", insn);
        break;
    }
}

/*
 * Execute a lower instruction at pair pc.  Returns the target of a taken
 * branch, or -1.
 */
static int vu1_lower(PS2VU1State *s, uint32_t insn, uint32_t pc)
{
    unsigned int op = extract32(insn, 25, 7);
    unsigned int dest = extract32(insn, 21, 4);
    unsigned int it = extract32(insn, 16, 5) & 15;
    unsigned int is = extract32(insn, 11, 5) & 15;
    unsigned int id = extract32(insn, 6, 5) & 15;
    int32_t imm11 = sextract32(insn, 0, 11);
    uint32_t imm15 = extract32(insn, 21, 4) << 11 | extract32(insn, 0, 11);
    uint32_t imm24 = extract32(insn, 0, 24);
    uint32_t imm12 = extract32(insn, 21, 1) << 11 | extract32(insn, 0, 11);
    int target = (pc + 1 + imm11) % VU1_MICRO_PAIRS;
    int16_t a = s->vi[is], b = s->vi[it];
    uint32_t *mem;

    switch (op) {
    case 0x00:
        /* LQ ft, imm(is) */
        vu1_set_vf(s, extract32(insn, 16, 5), dest,
                   vu1_data_qword(s, s->vi[is] + imm11));
        break;
    case 0x01:
        /* SQ fs, imm(it) */
        mem = vu1_data_qword(s, s->vi[it] + imm11);
        for (int f = 0; f < 4; f++) {
            if (dest & (8 >> f)) {
                mem[f] = s->vf[extract32(insn, 11, 5)][f];
            }
        }
        break;
    case 0x04:
        /* ILW it, imm(is) */
        mem = vu1_data_qword(s, s->vi[is] + imm11);
        for (int f = 0; f < 4; f++) {
            if (dest & (8 >> f)) {
                vu1_set_vi(s, it, mem[f] & 0xffff);
                break;
            }
        }
        break;
    case 0x05:
        /* ISW it, imm(is) */
        mem = vu1_data_qword(s, s->vi[is] + imm11);
        for (int f = 0; f < 4; f++) {
            if (dest & (8 >> f)) {
                mem[f] = s->vi[it];
            }
        }
        break;
    case 0x08:
        vu1_set_vi(s, it, s->vi[is] + imm15);   /* IADDIU */
        break;
    case 0x09:
        vu1_set_vi(s, it, s->vi[is] - imm15);   /* ISUBIU */
        break;
    case 0x10:
        s->vi[1] = (s->clip & 0xffffff) == imm24; /* FCEQ */
        break;
    case 0x11:
        s->clip = imm24;                        /* FCSET */
        break;
    case 0x12:
        s->vi[1] = (s->clip & imm24) != 0;      /* FCAND */
        break;
    case 0x13:
        s->vi[1] = ((s->clip | imm24) & 0xffffff) == 0xffffff; /* FCOR */
        break;
    case 0x14:
        vu1_set_vi(s, it, (s->status & 0xfff) == imm12); /* FSEQ */
        break;
    case 0x15:
        s->status = (s->status & 0x3f) | (imm12 & 0xfc0); /* FSSET */
        break;
    case 0x16:
        vu1_set_vi(s, it, s->status & imm12);   /* FSAND */
        break;
    case 0x17:
        vu1_set_vi(s, it, ((s->status | imm12) & 0xfff) == 0xfff); /* FSOR */
        break;
    case 0x18:
        vu1_set_vi(s, it, (s->mac & 0xffff) == s->vi[is]); /* FMEQ */
        break;
    case 0x1a:
        vu1_set_vi(s, it, s->mac & s->vi[is]);  /* FMAND */
        break;
    case 0x1b:
        vu1_set_vi(s, it, ((s->mac | s->vi[is]) & 0xffff) == 0xffff); /* FMOR */
        break;
    case 0x1c:
        vu1_set_vi(s, it, s->clip & 0xfff);     /* FCGET */
        break;
    case 0x20:
        return target;                          /* B */
    case 0x21:
        vu1_set_vi(s, it, pc + 2);              /* BAL */
        return target;
    case 0x24:
        return s->vi[is] % VU1_MICRO_PAIRS;     /* JR */
    case 0x25:
        target = s->vi[is] % VU1_MICRO_PAIRS;   /* JALR */
        vu1_set_vi(s, it, pc + 2);
        return target;
    case 0x28:
        return a == b ? target : -1;            /* IBEQ */
    case 0x29:
        return a != b ? target : -1;            /* IBNE */
    case 0x2c:
        return a < 0 ? target : -1;             /* IBLTZ */
    case 0x2d:
        return a > 0 ? target : -1;             /* IBGTZ */
    case 0x2e:
        return a <= 0 ? target : -1;            /* IBLEZ */
    case 0x2f:
        return a >= 0 ? target : -1;            /* IBGEZ */
    case 0x40:
        switch (extract32(insn, 0, 6)) {
        case 0x30:
            vu1_set_vi(s, id, s->vi[is] + s->vi[it]); /* IADD */
            break;
        case 0x31:
            vu1_set_vi(s, id, s->vi[is] - s->vi[it]); /* ISUB */
            break;
        case 0x32:
            vu1_set_vi(s, it, s->vi[is] + sextract32(insn, 6, 5)); /* IADDI */
            break;
        case 0x34:
            vu1_set_vi(s, id, s->vi[is] & s->vi[it]); /* IAND */
            break;
        case 0x35:
            vu1_set_vi(s, id, s->vi[is] | s->vi[it]); /* IOR */
            break;
        case 0x3c ... 0x3f:
            vu1_lower_t3(s, insn);
            break;
        default:
            qemu_log_mask(LOG_GUEST_ERROR, "ps2-vu1: bad lower 0x%08x\n",
                          insn);
            break;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "ps2-vu1: bad lower 0x%08x\n", insn);
        break;
    }
    return -1;
}

/* Run a VU1 program from pair pc until it ends. */
static void vu1_run(PS2VU1State *s, uint32_t pc)
{
    uint32_t *micro = memory_region_get_ram_ptr(&s->micro);
    uint32_t next = (pc + 1) % VU1_MICRO_PAIRS;
    bool ending = false;

    for (uint32_t step = 0; step < VU1_MAX_STEPS; step++) {
        uint32_t lower = micro[pc * 2], upper = micro[pc * 2 + 1];
        uint32_t new_next = (next + 1) % VU1_MICRO_PAIRS;
        bool end = ending;
        VU1Result r;
        int target = -1;

        if (s->q_wait && !--s->q_wait) {
            s->q = s->q_next;
        }
        if (s->p_wait && !--s->p_wait) {
            s->p = s->p_next;
        }

        vu1_upper(s, upper, &r);
        if (!(upper & VU1_UPPER_I)) {
            target = vu1_lower(s, lower, pc);
        }
        vu1_commit(s, &r);
        if (upper & VU1_UPPER_I) {
            s->i = lower;
        }
        s->vi[0] = 0;

        if (target >= 0) {
            new_next = target;
        }
        if (upper & VU1_UPPER_E) {
            ending = true;
        }
        pc = next;
        next = new_next;
        if (end) {
            s->tpc = pc;
            return;
        }
    }
    qemu_log_mask(LOG_GUEST_ERROR, "ps2-vu1: program at 0x%x does not end\n",
                  pc * 8);
    s->tpc = pc;
}

static void vif1_start_vu(PS2VU1State *s, uint32_t pc)
{
    uint32_t *regs = s->regs;
    uint32_t *stat = &regs[VIF_REG(PS2_VIF1_STAT)];

    /* Swap the double buffer. */
    regs[VIF_REG(PS2_VIF1_TOP)] = regs[VIF_REG(PS2_VIF1_TOPS)];
    regs[VIF_REG(PS2_VIF1_ITOP)] = regs[VIF_REG(PS2_VIF1_ITOPS)];
    *stat ^= PS2_VIF1_STAT_DBF;
    regs[VIF_REG(PS2_VIF1_TOPS)] = regs[VIF_REG(PS2_VIF1_BASE)] +
        ((*stat & PS2_VIF1_STAT_DBF) ? regs[VIF_REG(PS2_VIF1_OFST)] : 0);
    vu1_run(s, pc % VU1_MICRO_PAIRS);
}

/* The vectors of an UNPACK that take data, with CL and WL of CYCLE. */
static uint32_t vif1_unpack_vectors(uint32_t num, uint32_t cl, uint32_t wl)
{
    if (wl <= cl) {
        return num;
    }
    return num / wl * cl + MIN(num % wl, cl);
}

static uint32_t vif1_unpack_words(PS2VU1State *s, uint32_t cmd)
{
    uint32_t cycle = s->regs[VIF_REG(PS2_VIF1_CYCLE)];
    uint32_t num = extract32(cmd, 16, 8) ?: 256;
    unsigned int vn = extract32(cmd, 26, 2), vl = extract32(cmd, 24, 2);
    uint32_t bits = vl == 3 ? 16 : (32 >> vl) * (vn + 1);
    uint32_t vectors = vif1_unpack_vectors(num, extract32(cycle, 0, 8),
                                           extract32(cycle, 8, 8) ?: 1);

    return DIV_ROUND_UP(vectors * bits, 32);
}

/* Element e of the UNPACK data, as a 32-bit value. */
static uint32_t vif1_unpack_element(PS2VU1State *s, uint32_t e,
                                    unsigned int vl, bool usn)
{
    uint32_t size = 32 >> vl;
    uint32_t bit = e * size;
    uint32_t w = bit / 32;
    uint32_t v;

    if (w >= s->unpack_len) {
        return 0;
    }
    v = extract32(s->unpack_data[w], bit % 32, size);
    if (size < 32 && !usn) {
        v = sextract32(v, 0, size);
    }
    return v;
}

static void vif1_unpack(PS2VU1State *s)
{
    uint32_t cmd = s->cmd;
    uint32_t *regs = s->regs;
    uint32_t cycle = regs[VIF_REG(PS2_VIF1_CYCLE)];
    uint32_t cl = extract32(cycle, 0, 8), wl = extract32(cycle, 8, 8) ?: 1;
    uint32_t num = extract32(cmd, 16, 8) ?: 256;
    unsigned int vn = extract32(cmd, 26, 2), vl = extract32(cmd, 24, 2);
    bool usn = extract32(cmd, 14, 1), masked = extract32(cmd, 28, 1);
    uint32_t mode = regs[VIF_REG(PS2_VIF1_MODE)] & 3;
    uint32_t mask = regs[VIF_REG(PS2_VIF1_MASK)];
    uint32_t addr = extract32(cmd, 0, 10);
    uint32_t vec = 0;

    if (extract32(cmd, 15, 1)) {
        addr += regs[VIF_REG(PS2_VIF1_TOPS)];
    }

    for (uint32_t n = 0; n < num; n++) {
        uint32_t pos = n % wl;
        uint32_t row = MIN(pos, 3);
        uint32_t qaddr = wl <= cl ? addr + n / wl * cl + pos : addr + n;
        uint32_t *mem = vu1_data_qword(s, qaddr);
        bool has_data = wl <= cl || pos < cl;
        uint32_t d[4] = { 0 };

        if (has_data) {
            if (vl == 3) {
                uint32_t c = vif1_unpack_element(s, vec, 1, true);

                d[0] = extract32(c, 0, 5) << 3;
                d[1] = extract32(c, 5, 5) << 3;
                d[2] = extract32(c, 10, 5) << 3;
                d[3] = extract32(c, 15, 1) << 7;
            } else if (vn == 0) {
                d[0] = vif1_unpack_element(s, vec, vl, usn);
                d[1] = d[2] = d[3] = d[0];
            } else {
                /* V2 repeats x and y; V3 takes w from the next element. */
                uint32_t comps = vn + 1;

                for (int f = 0; f < 4; f++) {
                    uint32_t e = vec * comps + (vn == 1 ? f % 2 : f);

                    d[f] = vif1_unpack_element(s, e, vl, usn);
                }
            }
            vec++;
        }

        for (int f = 0; f < 4; f++) {
            unsigned int m = masked ? extract32(mask, (row * 4 + f) * 2, 2) : 0;
            uint32_t *r = &regs[VIF_REG(PS2_VIF1_R0) + f * 1];

            if (!has_data && m == 0) {
                m = 1;
            }
            switch (m) {
            case 0:
                if (mode == 1) {
                    d[f] += *r;
                } else if (mode == 2) {
                    *r += d[f];
                    d[f] = *r;
                }
                mem[f] = d[f];
                break;
            case 1:
                mem[f] = *r;
                break;
            case 2:
                mem[f] = regs[VIF_REG(PS2_VIF1_C0) + row];
                break;
            }
        }
    }
}

/* Take one word of the data of the current VIFcode. */
static void vif1_data(PS2VU1State *s, uint32_t word)
{
    unsigned int op = extract32(s->cmd, 24, 7);
    uint32_t *regs = s->regs;
    uint8_t buf[4];

    s->cmd_left--;
    switch (op) {
    case VIF_STMASK:
        regs[VIF_REG(PS2_VIF1_MASK)] = word;
        break;
    case VIF_STROW:
        regs[VIF_REG(PS2_VIF1_R0) + 3 - s->cmd_left] = word;
        break;
    case VIF_STCOL:
        regs[VIF_REG(PS2_VIF1_C0) + 3 - s->cmd_left] = word;
        break;
    case VIF_MPG: {
        uint32_t *micro = memory_region_get_ram_ptr(&s->micro);

        micro[s->cmd_addr++ % (VU1_MICRO_PAIRS * 2)] = word;
        memory_region_set_dirty(&s->micro, 0, PS2_VU1_MEM_SIZE);
        break;
    }
    case VIF_DIRECT:
    case VIF_DIRECTHL:
        stl_le_p(buf, word);
        ps2_gs_gif_write(s->gs, buf, sizeof(buf));
        break;
    default:
        /* UNPACK */
        s->unpack_data[s->unpack_len++] = word;
        if (!s->cmd_left) {
            vif1_unpack(s);
        }
        break;
    }
}

static void vif1_command(PS2VU1State *s, uint32_t cmd)
{
    unsigned int op = extract32(cmd, 24, 7);
    uint32_t imm = extract32(cmd, 0, 16);
    uint32_t num = extract32(cmd, 16, 8);
    uint32_t *regs = s->regs;

    s->cmd = cmd;
    s->cmd_left = 0;
    regs[VIF_REG(PS2_VIF1_CODE)] = cmd;

    if (op >= VIF_UNPACK) {
        s->cmd_left = vif1_unpack_words(s, cmd);
        s->unpack_len = 0;
        regs[VIF_REG(PS2_VIF1_NUM)] = num;
        return;
    }

    switch (op) {
    case VIF_NOP:
    case VIF_MSKPATH3:
    case VIF_FLUSHE:
    case VIF_FLUSH:
    case VIF_FLUSHA:
        break;
    case VIF_STCYCL:
        regs[VIF_REG(PS2_VIF1_CYCLE)] = imm;
        break;
    case VIF_OFFSET:
        regs[VIF_REG(PS2_VIF1_OFST)] = imm & 0x3ff;
        regs[VIF_REG(PS2_VIF1_STAT)] &= ~PS2_VIF1_STAT_DBF;
        regs[VIF_REG(PS2_VIF1_TOPS)] = regs[VIF_REG(PS2_VIF1_BASE)];
        break;
    case VIF_BASE:
        regs[VIF_REG(PS2_VIF1_BASE)] = imm & 0x3ff;
        break;
    case VIF_ITOP:
        regs[VIF_REG(PS2_VIF1_ITOPS)] = imm & 0x3ff;
        break;
    case VIF_STMOD:
        regs[VIF_REG(PS2_VIF1_MODE)] = imm & 3;
        break;
    case VIF_MARK:
        regs[VIF_REG(PS2_VIF1_MARK)] = imm;
        regs[VIF_REG(PS2_VIF1_STAT)] |= PS2_VIF1_STAT_MRK;
        break;
    case VIF_MSCAL:
    case VIF_MSCALF:
        vif1_start_vu(s, imm);
        break;
    case VIF_MSCNT:
        vif1_start_vu(s, s->tpc);
        break;
    case VIF_STMASK:
        s->cmd_left = 1;
        break;
    case VIF_STROW:
    case VIF_STCOL:
        s->cmd_left = 4;
        break;
    case VIF_MPG:
        s->cmd_left = (num ?: 256) * 2;
        s->cmd_addr = imm * 2;
        break;
    case VIF_DIRECT:
    case VIF_DIRECTHL:
        s->cmd_left = (imm ?: 0x10000) * 4;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "ps2-vu1: bad VIFcode 0x%08x\n", cmd);
        break;
    }
}

static void vif1_write_word(PS2VU1State *s, uint32_t word)
{
    if (s->cmd_left) {
        vif1_data(s, word);
    } else {
        vif1_command(s, word);
    }
}

static void vif1_write(PS2VU1State *s, const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i + 4 <= len; i += 4) {
        vif1_write_word(s, ldl_le_p(buf + i));
    }
}

static void vif1_reset(PS2VU1State *s)
{
    s->cmd = 0;
    s->cmd_left = 0;
    s->unpack_len = 0;
    s->regs[VIF_REG(PS2_VIF1_STAT)] = 0;
}

static uint64_t vif1_read(void *opaque, hwaddr addr, unsigned int size)
{
    PS2VU1State *s = opaque;

    if (addr & 0xf) {
        return 0;
    }
    switch (addr) {
    case PS2_VIF1_FBRST:
        return 0;
    default:
        /* The VIF and VU1 are always idle with an empty FIFO. */
        if (addr < PS2_VIF1_NUM_REGS * 16) {
            return s->regs[VIF_REG(addr)];
        }
        return 0;
    }
}

static void vif1_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned int size)
{
    PS2VU1State *s = opaque;
    uint32_t *stat = &s->regs[VIF_REG(PS2_VIF1_STAT)];

    switch (addr) {
    case PS2_VIF1_STAT:
        *stat = (*stat & ~PS2_VIF1_STAT_FDR) | (val & PS2_VIF1_STAT_FDR);
        break;
    case PS2_VIF1_FBRST:
        if (val & PS2_VIF1_FBRST_RST) {
            vif1_reset(s);
        }
        break;
    case PS2_VIF1_MARK:
        s->regs[VIF_REG(PS2_VIF1_MARK)] = val & 0xffff;
        *stat &= ~PS2_VIF1_STAT_MRK;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "ps2-vu1: write of 0x%" PRIx64
                      " to VIF1 register 0x%" HWADDR_PRIx "\n", val, addr);
        break;
    }
}

static const MemoryRegionOps vif1_ops = {
    .read = vif1_read,
    .write = vif1_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static uint64_t vif1_fifo_read(void *opaque, hwaddr addr, unsigned int size)
{
    return 0;
}

static void vif1_fifo_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned int size)
{
    vif1_write_word(opaque, val);
}

static const MemoryRegionOps vif1_fifo_ops = {
    .read = vif1_fifo_read,
    .write = vif1_fifo_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void vif1_dma_sink(void *opaque, const uint8_t *buf, uint32_t len)
{
    vif1_write(opaque, buf, len);
}

/* DMAC channel 1 to memory: read the GS local to host transfer. */
static void vif1_dma_start(void *opaque)
{
    PS2VU1State *s = opaque;
    PS2DMACChannel *c = &s->dmac->ch[PS2_DMAC_VIF1];
    uint32_t len = c->qwc * 16;
    g_autofree uint8_t *buf = g_malloc(len);

    ps2_gs_read_local(s->gs, buf, len);
    if (dma_memory_write(&address_space_memory, c->madr & 0x7ffffff0, buf,
                         len, MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR, "ps2-vu1: bad DMA address 0x%x\n",
                      c->madr);
    }
    c->madr += len;
    ps2_dmac_channel_done(s->dmac, PS2_DMAC_VIF1);
}

static const PS2DMACChannelOps vif1_dma_ops = {
    .sink = vif1_dma_sink,
    .start = vif1_dma_start,
};

static void ps2_vu1_reset_hold(Object *obj, ResetType type)
{
    PS2VU1State *s = PS2_VU1(obj);

    memset(s->regs, 0, sizeof(s->regs));
    vif1_reset(s);
    s->cmd_addr = 0;
    s->tpc = 0;
    memset(s->vf, 0, sizeof(s->vf));
    s->vf[0][3] = 0x3f800000;
    memset(s->acc, 0, sizeof(s->acc));
    memset(s->vi, 0, sizeof(s->vi));
    s->q = s->p = s->i = 0;
    s->q_next = s->q_wait = s->p_next = s->p_wait = 0;
    s->r = 0x3f800000;
    s->clip = s->mac = s->status = 0;
}

static void ps2_vu1_init(Object *obj)
{
    PS2VU1State *s = PS2_VU1(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->vif_iomem, obj, &vif1_ops, s, "ps2.vif1",
                          PS2_VIF1_SIZE);
    sysbus_init_mmio(sbd, &s->vif_iomem);
    memory_region_init_io(&s->fifo_iomem, obj, &vif1_fifo_ops, s,
                          "ps2.vif1-fifo", PS2_VIF1_FIFO_SIZE);
    sysbus_init_mmio(sbd, &s->fifo_iomem);
}

static void ps2_vu1_realize(DeviceState *dev, Error **errp)
{
    PS2VU1State *s = PS2_VU1(dev);

    if (!s->dmac || !s->gs) {
        error_setg(errp, "%s: the dmac and gs links must be set",
                   TYPE_PS2_VU1);
        return;
    }
    if (!memory_region_init_ram(&s->micro, OBJECT(dev), "ps2.vu1-micro",
                                PS2_VU1_MEM_SIZE, errp) ||
        !memory_region_init_ram(&s->data, OBJECT(dev), "ps2.vu1-data",
                                PS2_VU1_MEM_SIZE, errp)) {
        return;
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->micro);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->data);
    ps2_dmac_set_channel_ops(s->dmac, PS2_DMAC_VIF1, &vif1_dma_ops, s);
}

static const Property ps2_vu1_properties[] = {
    DEFINE_PROP_LINK("dmac", PS2VU1State, dmac, TYPE_PS2_DMAC,
                     PS2DMACState *),
    DEFINE_PROP_LINK("gs", PS2VU1State, gs, TYPE_PS2_GS, PS2GSState *),
};

static int ps2_vu1_post_load(void *opaque, int version_id)
{
    PS2VU1State *s = opaque;

    if (s->unpack_len > PS2_VIF1_UNPACK_MAX ||
        s->cmd_left > 0x10000 * 4 || s->tpc >= VU1_MICRO_PAIRS) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_ps2_vu1 = {
    .name = TYPE_PS2_VU1,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ps2_vu1_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, PS2VU1State, PS2_VIF1_NUM_REGS),
        VMSTATE_UINT32(cmd, PS2VU1State),
        VMSTATE_UINT32(cmd_left, PS2VU1State),
        VMSTATE_UINT32(cmd_addr, PS2VU1State),
        VMSTATE_UINT32_ARRAY(unpack_data, PS2VU1State, PS2_VIF1_UNPACK_MAX),
        VMSTATE_UINT32(unpack_len, PS2VU1State),
        VMSTATE_UINT32(tpc, PS2VU1State),
        VMSTATE_UINT32_2DARRAY(vf, PS2VU1State, 32, 4),
        VMSTATE_UINT32_ARRAY(acc, PS2VU1State, 4),
        VMSTATE_UINT16_ARRAY(vi, PS2VU1State, 16),
        VMSTATE_UINT32(q, PS2VU1State),
        VMSTATE_UINT32(p, PS2VU1State),
        VMSTATE_UINT32(q_next, PS2VU1State),
        VMSTATE_UINT32(q_wait, PS2VU1State),
        VMSTATE_UINT32(p_next, PS2VU1State),
        VMSTATE_UINT32(p_wait, PS2VU1State),
        VMSTATE_UINT32(i, PS2VU1State),
        VMSTATE_UINT32(r, PS2VU1State),
        VMSTATE_UINT32(clip, PS2VU1State),
        VMSTATE_UINT32(mac, PS2VU1State),
        VMSTATE_UINT32(status, PS2VU1State),
        VMSTATE_END_OF_LIST()
    },
};

static void ps2_vu1_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = ps2_vu1_reset_hold;
    dc->realize = ps2_vu1_realize;
    dc->vmsd = &vmstate_ps2_vu1;
    device_class_set_props(dc, ps2_vu1_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo ps2_vu1_types[] = {
    {
        .name = TYPE_PS2_VU1,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(PS2VU1State),
        .instance_init = ps2_vu1_init,
        .class_init = ps2_vu1_class_init,
    },
};

DEFINE_TYPES(ps2_vu1_types);
