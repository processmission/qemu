/*
 * MCS-51 family disassembler
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "disas/dis-asm.h"
#include "internals.h"
#include "decode.h"

typedef struct MCS251DisasContext {
    disassemble_info *info;
    bfd_vma cursor;
    int status;
    bfd_vma error_address;
} MCS251DisasContext;

static bool mcs251_disas_read_at(void *opaque, uint32_t address,
                                 uint8_t *value)
{
    MCS251DisasContext *ctx = opaque;

    ctx->status =
        ctx->info->read_memory_func(address, value, 1, ctx->info);
    if (ctx->status) {
        ctx->error_address = address;
        return false;
    }
    return true;
}

static uint8_t mcs251_disas_read8(MCS251DisasContext *ctx)
{
    uint8_t value = 0;

    if (!ctx->status &&
        !mcs251_disas_read_at(ctx, ctx->cursor & MCS_TARGET_ADDR_MASK,
                             &value)) {
        return 0;
    }
    ctx->cursor++;
    return value;
}

static uint16_t mcs251_disas_read16(MCS251DisasContext *ctx)
{
    uint16_t value = deposit32(0, 8, 8, mcs251_disas_read8(ctx));

    return deposit32(value, 0, 8, mcs251_disas_read8(ctx));
}

static uint32_t mcs251_disas_read24(MCS251DisasContext *ctx)
{
    uint32_t value = deposit32(0, 16, 8, mcs251_disas_read8(ctx));

    value = deposit32(value, 8, 8, mcs251_disas_read8(ctx));
    return deposit32(value, 0, 8, mcs251_disas_read8(ctx));
}

static uint32_t mcs251_disas_relative(MCS251DisasContext *ctx)
{
    int8_t displacement = mcs251_disas_read8(ctx);

    return (ctx->cursor + displacement) & MCS_TARGET_ADDR_MASK;
}

static void mcs251_disas_reg(char *buffer, size_t size, unsigned reg)
{
    snprintf(buffer, size, "r%u", reg);
}

static void mcs251_disas_wr(char *buffer, size_t size, unsigned code)
{
    snprintf(buffer, size, "wr%u", code * 2);
}

static bool mcs251_disas_dr(char *buffer, size_t size, unsigned code)
{
    unsigned position;

    if (code < MCS251_REG_BANK_COUNT) {
        position = code * 4;
    } else if (code >= MCS251_DPTR_DR_CODE) {
        position = MCS251_REG_DPTR_FIRST +
                   (code - MCS251_DPTR_DR_CODE) * 4;
    } else {
        return false;
    }
    snprintf(buffer, size, "dr%u", position);
    return true;
}

static void mcs251_disas_acc_source(MCS251DisasContext *ctx,
                                    uint8_t opcode)
{
    disassemble_info *info = ctx->info;

    switch (FIELD_EX8(opcode, MCS251_OPCODE, LOW_NIBBLE)) {
    case 4:
        info->fprintf_func(info->stream, "#0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 5:
        info->fprintf_func(info->stream, "0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 6:
    case 7:
        info->fprintf_func(
            info->stream, "@r%u",
            FIELD_EX8(opcode, MCS251_OPCODE, RI));
        break;
    default:
        info->fprintf_func(
            info->stream, "r%u",
            FIELD_EX8(opcode, MCS251_OPCODE, RN));
        break;
    }
}

static void mcs251_disas_classic(MCS251DisasContext *ctx, uint8_t opcode)
{
    disassemble_info *info = ctx->info;
    unsigned group = FIELD_EX8(opcode, MCS251_OPCODE, GROUP5);
    unsigned reg = FIELD_EX8(opcode, MCS251_OPCODE, RN);
    uint8_t direct;
    uint8_t immediate;
    uint32_t target;

    if (FIELD_EX8(opcode, MCS251_OPCODE, LOW5) == 0x01) {
        uint8_t low = mcs251_disas_read8(ctx);

        target = deposit32(ctx->cursor, 0, 11, 0);
        target = deposit32(
            target, 8, 3,
            FIELD_EX8(opcode, MCS251_OPCODE, PAGE));
        target = deposit32(target, 0, 8, low);
        info->fprintf_func(info->stream, "ajmp 0x%06x", target);
        return;
    }
    if (FIELD_EX8(opcode, MCS251_OPCODE, LOW5) == 0x11) {
        uint8_t low = mcs251_disas_read8(ctx);

        target = deposit32(ctx->cursor, 0, 11, 0);
        target = deposit32(
            target, 8, 3,
            FIELD_EX8(opcode, MCS251_OPCODE, PAGE));
        target = deposit32(target, 0, 8, low);
        info->fprintf_func(info->stream, "acall 0x%06x", target);
        return;
    }

    switch (group) {
    case 0x01:
        info->fprintf_func(info->stream, "inc r%u", reg);
        return;
    case 0x03:
        info->fprintf_func(info->stream, "dec r%u", reg);
        return;
    case 0x05:
        info->fprintf_func(info->stream, "add a,r%u", reg);
        return;
    case 0x07:
        info->fprintf_func(info->stream, "addc a,r%u", reg);
        return;
    case 0x09:
        info->fprintf_func(info->stream, "orl a,r%u", reg);
        return;
    case 0x0b:
        info->fprintf_func(info->stream, "anl a,r%u", reg);
        return;
    case 0x0d:
        info->fprintf_func(info->stream, "xrl a,r%u", reg);
        return;
    case 0x0f:
        info->fprintf_func(info->stream, "mov r%u,#0x%02x", reg,
                           mcs251_disas_read8(ctx));
        return;
    case 0x11:
        info->fprintf_func(info->stream, "mov 0x%02x,r%u",
                           mcs251_disas_read8(ctx), reg);
        return;
    case 0x13:
        info->fprintf_func(info->stream, "subb a,r%u", reg);
        return;
    case 0x15:
        info->fprintf_func(info->stream, "mov r%u,0x%02x", reg,
                           mcs251_disas_read8(ctx));
        return;
    case 0x17:
        immediate = mcs251_disas_read8(ctx);
        target = mcs251_disas_relative(ctx);
        info->fprintf_func(info->stream,
                           "cjne r%u,#0x%02x,0x%06x",
                           reg, immediate, target);
        return;
    case 0x19:
        info->fprintf_func(info->stream, "xch a,r%u", reg);
        return;
    case 0x1b:
        target = mcs251_disas_relative(ctx);
        info->fprintf_func(info->stream, "djnz r%u,0x%06x",
                           reg, target);
        return;
    case 0x1d:
        info->fprintf_func(info->stream, "mov a,r%u", reg);
        return;
    case 0x1f:
        info->fprintf_func(info->stream, "mov r%u,a", reg);
        return;
    default:
        break;
    }

    switch (opcode) {
    case 0x00:
        info->fprintf_func(info->stream, "nop");
        break;
    case 0x02: {
        uint16_t low = mcs251_disas_read16(ctx);

        target = deposit32(ctx->cursor, 0, 16, low);
        info->fprintf_func(info->stream, "ljmp 0x%06x", target);
        break;
    }
    case 0x03:
        info->fprintf_func(info->stream, "rr a");
        break;
    case 0x04:
        info->fprintf_func(info->stream, "inc a");
        break;
    case 0x05:
        info->fprintf_func(info->stream, "inc 0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 0x06:
    case 0x07:
        info->fprintf_func(
            info->stream, "inc @r%u",
            FIELD_EX8(opcode, MCS251_OPCODE, RI));
        break;
    case 0x10:
        direct = mcs251_disas_read8(ctx);
        target = mcs251_disas_relative(ctx);
        info->fprintf_func(info->stream, "jbc 0x%02x,0x%06x",
                           direct, target);
        break;
    case 0x12: {
        uint16_t low = mcs251_disas_read16(ctx);

        target = deposit32(ctx->cursor, 0, 16, low);
        info->fprintf_func(info->stream, "lcall 0x%06x", target);
        break;
    }
    case 0x13:
        info->fprintf_func(info->stream, "rrc a");
        break;
    case 0x14:
        info->fprintf_func(info->stream, "dec a");
        break;
    case 0x15:
        info->fprintf_func(info->stream, "dec 0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 0x16:
    case 0x17:
        info->fprintf_func(
            info->stream, "dec @r%u",
            FIELD_EX8(opcode, MCS251_OPCODE, RI));
        break;
    case 0x20:
    case 0x30:
        direct = mcs251_disas_read8(ctx);
        target = mcs251_disas_relative(ctx);
        info->fprintf_func(info->stream, "%s 0x%02x,0x%06x",
                           opcode == 0x20 ? "jb" : "jnb",
                           direct, target);
        break;
    case 0x22:
        info->fprintf_func(info->stream, "ret");
        break;
    case 0x23:
        info->fprintf_func(info->stream, "rl a");
        break;
    case 0x24:
    case 0x25:
    case 0x26:
    case 0x27:
        info->fprintf_func(info->stream, "add a,");
        mcs251_disas_acc_source(ctx, opcode);
        break;
    case 0x32:
        info->fprintf_func(info->stream, "reti");
        break;
    case 0x33:
        info->fprintf_func(info->stream, "rlc a");
        break;
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37:
        info->fprintf_func(info->stream, "addc a,");
        mcs251_disas_acc_source(ctx, opcode);
        break;
    case 0x40:
    case 0x50:
    case 0x60:
    case 0x70: {
        static const char * const names[] = {
            [0x4] = "jc",
            [0x5] = "jnc",
            [0x6] = "jz",
            [0x7] = "jnz",
        };

        target = mcs251_disas_relative(ctx);
        info->fprintf_func(
            info->stream, "%s 0x%06x",
            names[FIELD_EX8(opcode, MCS251_OPCODE, HIGH_NIBBLE)],
            target);
        break;
    }
    case 0x42:
    case 0x52:
    case 0x62: {
        static const char * const names[] = {
            [0x4] = "orl",
            [0x5] = "anl",
            [0x6] = "xrl",
        };

        info->fprintf_func(
            info->stream, "%s 0x%02x,a",
            names[FIELD_EX8(opcode, MCS251_OPCODE, HIGH_NIBBLE)],
            mcs251_disas_read8(ctx));
        break;
    }
    case 0x43:
    case 0x53:
    case 0x63: {
        static const char * const names[] = {
            [0x4] = "orl",
            [0x5] = "anl",
            [0x6] = "xrl",
        };

        direct = mcs251_disas_read8(ctx);
        immediate = mcs251_disas_read8(ctx);
        info->fprintf_func(
            info->stream, "%s 0x%02x,#0x%02x",
            names[FIELD_EX8(opcode, MCS251_OPCODE, HIGH_NIBBLE)],
            direct, immediate);
        break;
    }
    case 0x44:
    case 0x45:
    case 0x46:
    case 0x47:
    case 0x54:
    case 0x55:
    case 0x56:
    case 0x57:
    case 0x64:
    case 0x65:
    case 0x66:
    case 0x67: {
        static const char * const names[] = {
            [0x4] = "orl",
            [0x5] = "anl",
            [0x6] = "xrl",
        };

        info->fprintf_func(
            info->stream, "%s a,",
            names[FIELD_EX8(opcode, MCS251_OPCODE, HIGH_NIBBLE)]);
        mcs251_disas_acc_source(ctx, opcode);
        break;
    }
    case 0x72:
    case 0x82:
    case 0xa0:
    case 0xb0:
        direct = mcs251_disas_read8(ctx);
        if (opcode == 0x72 || opcode == 0xa0) {
            info->fprintf_func(info->stream, "orl c,%s0x%02x",
                               opcode == 0xa0 ? "/" : "", direct);
        } else {
            info->fprintf_func(info->stream, "anl c,%s0x%02x",
                               opcode == 0xb0 ? "/" : "", direct);
        }
        break;
    case 0x73:
        info->fprintf_func(info->stream, "jmp @a+dptr");
        break;
    case 0x74:
        info->fprintf_func(info->stream, "mov a,#0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 0x75:
        direct = mcs251_disas_read8(ctx);
        info->fprintf_func(info->stream, "mov 0x%02x,#0x%02x",
                           direct, mcs251_disas_read8(ctx));
        break;
    case 0x76:
    case 0x77:
        info->fprintf_func(
            info->stream, "mov @r%u,#0x%02x",
            FIELD_EX8(opcode, MCS251_OPCODE, RI),
            mcs251_disas_read8(ctx));
        break;
    case 0x80:
        target = mcs251_disas_relative(ctx);
        info->fprintf_func(info->stream, "sjmp 0x%06x", target);
        break;
    case 0x83:
        info->fprintf_func(info->stream, "movc a,@a+pc");
        break;
    case 0x84:
        info->fprintf_func(info->stream, "div a,b");
        break;
    case 0x85: {
        uint8_t source = mcs251_disas_read8(ctx);

        info->fprintf_func(info->stream, "mov 0x%02x,0x%02x",
                           mcs251_disas_read8(ctx), source);
        break;
    }
    case 0x86:
    case 0x87:
        info->fprintf_func(
            info->stream, "mov 0x%02x,@r%u",
            mcs251_disas_read8(ctx),
            FIELD_EX8(opcode, MCS251_OPCODE, RI));
        break;
    case 0x90:
        info->fprintf_func(info->stream, "mov dptr,#0x%04x",
                           mcs251_disas_read16(ctx));
        break;
    case 0x92:
        info->fprintf_func(info->stream, "mov 0x%02x,c",
                           mcs251_disas_read8(ctx));
        break;
    case 0x93:
        info->fprintf_func(info->stream, "movc a,@a+dptr");
        break;
    case 0x94:
    case 0x95:
    case 0x96:
    case 0x97:
        info->fprintf_func(info->stream, "subb a,");
        mcs251_disas_acc_source(ctx, opcode);
        break;
    case 0xa2:
        info->fprintf_func(info->stream, "mov c,0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 0xa3:
        info->fprintf_func(info->stream, "inc dptr");
        break;
    case 0xa4:
        info->fprintf_func(info->stream, "mul a,b");
        break;
    case 0xa5:
        info->fprintf_func(info->stream,
                           info->mach == MCS251_DISAS_MCS51 ?
                           "nop" : "esc");
        break;
    case 0xa6:
    case 0xa7:
        info->fprintf_func(
            info->stream, "mov @r%u,0x%02x",
            FIELD_EX8(opcode, MCS251_OPCODE, RI),
            mcs251_disas_read8(ctx));
        break;
    case 0xb2:
        info->fprintf_func(info->stream, "cpl 0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 0xb3:
        info->fprintf_func(info->stream, "cpl c");
        break;
    case 0xb4:
        immediate = mcs251_disas_read8(ctx);
        target = mcs251_disas_relative(ctx);
        info->fprintf_func(info->stream,
                           "cjne a,#0x%02x,0x%06x",
                           immediate, target);
        break;
    case 0xb5:
        direct = mcs251_disas_read8(ctx);
        target = mcs251_disas_relative(ctx);
        info->fprintf_func(info->stream, "cjne a,0x%02x,0x%06x",
                           direct, target);
        break;
    case 0xb6:
    case 0xb7:
        immediate = mcs251_disas_read8(ctx);
        target = mcs251_disas_relative(ctx);
        info->fprintf_func(
            info->stream, "cjne @r%u,#0x%02x,0x%06x",
            FIELD_EX8(opcode, MCS251_OPCODE, RI),
            immediate, target);
        break;
    case 0xc0:
        info->fprintf_func(info->stream, "push 0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 0xc2:
        info->fprintf_func(info->stream, "clr 0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 0xc3:
        info->fprintf_func(info->stream, "clr c");
        break;
    case 0xc4:
        info->fprintf_func(info->stream, "swap a");
        break;
    case 0xc5:
        info->fprintf_func(info->stream, "xch a,0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 0xc6:
    case 0xc7:
        info->fprintf_func(
            info->stream, "xch a,@r%u",
            FIELD_EX8(opcode, MCS251_OPCODE, RI));
        break;
    case 0xd0:
        info->fprintf_func(info->stream, "pop 0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 0xd2:
        info->fprintf_func(info->stream, "setb 0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 0xd3:
        info->fprintf_func(info->stream, "setb c");
        break;
    case 0xd4:
        info->fprintf_func(info->stream, "da a");
        break;
    case 0xd5:
        direct = mcs251_disas_read8(ctx);
        target = mcs251_disas_relative(ctx);
        info->fprintf_func(info->stream, "djnz 0x%02x,0x%06x",
                           direct, target);
        break;
    case 0xd6:
    case 0xd7:
        info->fprintf_func(
            info->stream, "xchd a,@r%u",
            FIELD_EX8(opcode, MCS251_OPCODE, RI));
        break;
    case 0xe0:
        info->fprintf_func(info->stream, "movx a,@dptr");
        break;
    case 0xe2:
    case 0xe3:
        info->fprintf_func(
            info->stream, "movx a,@r%u",
            FIELD_EX8(opcode, MCS251_OPCODE, RI));
        break;
    case 0xe4:
        info->fprintf_func(info->stream, "clr a");
        break;
    case 0xe5:
        info->fprintf_func(info->stream, "mov a,0x%02x",
                           mcs251_disas_read8(ctx));
        break;
    case 0xe6:
    case 0xe7:
        info->fprintf_func(
            info->stream, "mov a,@r%u",
            FIELD_EX8(opcode, MCS251_OPCODE, RI));
        break;
    case 0xf0:
        info->fprintf_func(info->stream, "movx @dptr,a");
        break;
    case 0xf2:
    case 0xf3:
        info->fprintf_func(
            info->stream, "movx @r%u,a",
            FIELD_EX8(opcode, MCS251_OPCODE, RI));
        break;
    case 0xf4:
        info->fprintf_func(info->stream, "cpl a");
        break;
    case 0xf5:
        info->fprintf_func(info->stream, "mov 0x%02x,a",
                           mcs251_disas_read8(ctx));
        break;
    case 0xf6:
    case 0xf7:
        info->fprintf_func(
            info->stream, "mov @r%u,a",
            FIELD_EX8(opcode, MCS251_OPCODE, RI));
        break;
    default:
        info->fprintf_func(info->stream, "nop");
        break;
    }
}

static const char *mcs251_native_name(uint8_t opcode)
{
    switch (opcode) {
    case 0x2c:
    case 0x2d:
    case 0x2e:
    case 0x2f:
        return "add";
    case 0x4c:
    case 0x4d:
    case 0x4e:
        return "orl";
    case 0x5c:
    case 0x5d:
    case 0x5e:
        return "anl";
    case 0x6c:
    case 0x6d:
    case 0x6e:
        return "xrl";
    case 0x7c:
    case 0x7d:
    case 0x7e:
    case 0x7f:
        return "mov";
    case 0x9c:
    case 0x9d:
    case 0x9e:
    case 0x9f:
        return "sub";
    case 0xbc:
    case 0xbd:
    case 0xbe:
    case 0xbf:
        return "cmp";
    default:
        return "nop";
    }
}

static void mcs251_disas_native_register(MCS251DisasContext *ctx,
                                         uint8_t opcode)
{
    disassemble_info *info = ctx->info;
    uint8_t specifier = mcs251_disas_read8(ctx);
    unsigned destination =
        FIELD_EX8(specifier, MCS251_SPECIFIER, CODE);
    unsigned source = FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
    char destination_name[16];
    char source_name[16];
    bool valid = true;

    switch (FIELD_EX8(opcode, MCS251_OPCODE, WIDTH)) {
    case MCS251_WIDTH_BYTE:
        mcs251_disas_reg(destination_name, sizeof(destination_name),
                         destination);
        mcs251_disas_reg(source_name, sizeof(source_name), source);
        break;
    case MCS251_WIDTH_WORD:
        mcs251_disas_wr(destination_name, sizeof(destination_name),
                        destination);
        mcs251_disas_wr(source_name, sizeof(source_name), source);
        break;
    case MCS251_WIDTH_DWORD:
        valid =
            mcs251_disas_dr(destination_name, sizeof(destination_name),
                           destination) &&
            mcs251_disas_dr(source_name, sizeof(source_name), source);
        break;
    default:
        valid = false;
        break;
    }

    if (valid) {
        info->fprintf_func(info->stream, "%s %s,%s",
                           mcs251_native_name(opcode),
                           destination_name, source_name);
    } else {
        info->fprintf_func(info->stream, "nop");
    }
}

static bool mcs251_native_mode_allowed(uint8_t opcode, unsigned mode)
{
    static const uint16_t arithmetic_modes =
        BIT(MCS251_MODE_BYTE_IMMEDIATE) |
        BIT(MCS251_MODE_BYTE_DIRECT8) |
        BIT(MCS251_MODE_BYTE_DIRECT16) |
        BIT(MCS251_MODE_WORD_IMMEDIATE) |
        BIT(MCS251_MODE_WORD_DIRECT8) |
        BIT(MCS251_MODE_WORD_DIRECT16) |
        BIT(MCS251_MODE_DWORD_ZERO_IMMEDIATE) |
        BIT(MCS251_MODE_BYTE_INDIRECT_WR) |
        BIT(MCS251_MODE_BYTE_INDIRECT_DR);
    static const uint16_t logical_modes =
        BIT(MCS251_MODE_BYTE_IMMEDIATE) |
        BIT(MCS251_MODE_BYTE_DIRECT8) |
        BIT(MCS251_MODE_BYTE_DIRECT16) |
        BIT(MCS251_MODE_WORD_IMMEDIATE) |
        BIT(MCS251_MODE_WORD_DIRECT8) |
        BIT(MCS251_MODE_WORD_DIRECT16) |
        BIT(MCS251_MODE_BYTE_INDIRECT_WR) |
        BIT(MCS251_MODE_BYTE_INDIRECT_DR);
    static const uint16_t move_modes =
        BIT(MCS251_MODE_BYTE_IMMEDIATE) |
        BIT(MCS251_MODE_BYTE_DIRECT8) |
        BIT(MCS251_MODE_BYTE_DIRECT16) |
        BIT(MCS251_MODE_WORD_IMMEDIATE) |
        BIT(MCS251_MODE_WORD_DIRECT8) |
        BIT(MCS251_MODE_WORD_DIRECT16) |
        BIT(MCS251_MODE_DWORD_ZERO_IMMEDIATE) |
        BIT(MCS251_MODE_BYTE_INDIRECT_WR) |
        BIT(MCS251_MODE_BYTE_INDIRECT_DR) |
        BIT(MCS251_MODE_DWORD_SIGNED_IMMEDIATE) |
        BIT(MCS251_MODE_DWORD_DIRECT8) |
        BIT(MCS251_MODE_DWORD_DIRECT16);
    uint16_t modes;

    switch (opcode) {
    case 0x2e:
    case 0x9e:
        modes = arithmetic_modes;
        break;
    case 0x4e:
    case 0x5e:
    case 0x6e:
        modes = logical_modes;
        break;
    case 0x7e:
        modes = move_modes;
        break;
    case 0xbe:
        modes =
            arithmetic_modes | BIT(MCS251_MODE_DWORD_SIGNED_IMMEDIATE);
        break;
    default:
        return false;
    }
    return modes & BIT(mode);
}

static void mcs251_disas_native_generic(MCS251DisasContext *ctx,
                                        uint8_t opcode)
{
    disassemble_info *info = ctx->info;
    uint8_t specifier = mcs251_disas_read8(ctx);
    unsigned code = FIELD_EX8(specifier, MCS251_SPECIFIER, CODE);
    unsigned mode = FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
    char destination[16];
    char source[32];
    bool valid = mcs251_native_mode_allowed(opcode, mode);

    switch (mode) {
    case MCS251_MODE_BYTE_IMMEDIATE:
        mcs251_disas_reg(destination, sizeof(destination), code);
        snprintf(source, sizeof(source), "#0x%02x",
                 mcs251_disas_read8(ctx));
        break;
    case MCS251_MODE_WORD_IMMEDIATE:
        mcs251_disas_wr(destination, sizeof(destination), code);
        snprintf(source, sizeof(source), "#0x%04x",
                 mcs251_disas_read16(ctx));
        break;
    case MCS251_MODE_DWORD_ZERO_IMMEDIATE:
    case MCS251_MODE_DWORD_SIGNED_IMMEDIATE:
        valid &= mcs251_disas_dr(destination, sizeof(destination), code);
        snprintf(source, sizeof(source), "#0x%04x",
                 mcs251_disas_read16(ctx));
        break;
    case MCS251_MODE_BYTE_DIRECT8:
        mcs251_disas_reg(destination, sizeof(destination), code);
        snprintf(source, sizeof(source), "0x%02x",
                 mcs251_disas_read8(ctx));
        break;
    case MCS251_MODE_WORD_DIRECT8:
        mcs251_disas_wr(destination, sizeof(destination), code);
        snprintf(source, sizeof(source), "0x%02x",
                 mcs251_disas_read8(ctx));
        break;
    case MCS251_MODE_DWORD_DIRECT8:
        valid &= mcs251_disas_dr(destination, sizeof(destination), code);
        snprintf(source, sizeof(source), "0x%02x",
                 mcs251_disas_read8(ctx));
        break;
    case MCS251_MODE_BYTE_DIRECT16:
        mcs251_disas_reg(destination, sizeof(destination), code);
        snprintf(source, sizeof(source), "0x%04x",
                 mcs251_disas_read16(ctx));
        break;
    case MCS251_MODE_WORD_DIRECT16:
        mcs251_disas_wr(destination, sizeof(destination), code);
        snprintf(source, sizeof(source), "0x%04x",
                 mcs251_disas_read16(ctx));
        break;
    case MCS251_MODE_DWORD_DIRECT16:
        valid &= mcs251_disas_dr(destination, sizeof(destination), code);
        snprintf(source, sizeof(source), "0x%04x",
                 mcs251_disas_read16(ctx));
        break;
    case MCS251_MODE_BYTE_INDIRECT_WR:
    case MCS251_MODE_BYTE_INDIRECT_DR: {
        uint8_t register_specifier = mcs251_disas_read8(ctx);
        unsigned destination_code =
            FIELD_EX8(register_specifier, MCS251_SPECIFIER, CODE);

        valid &= !FIELD_EX8(register_specifier,
                            MCS251_SPECIFIER, MODE);
        mcs251_disas_reg(destination, sizeof(destination),
                         destination_code);
        if (mode == MCS251_MODE_BYTE_INDIRECT_WR) {
            char pointer[16];

            mcs251_disas_wr(pointer, sizeof(pointer), code);
            snprintf(source, sizeof(source), "@%s", pointer);
        } else {
            char pointer[16];

            valid &= mcs251_disas_dr(pointer, sizeof(pointer), code);
            snprintf(source, sizeof(source), "@%s", pointer);
        }
        break;
    }
    default:
        destination[0] = '\0';
        source[0] = '\0';
        valid = false;
        break;
    }

    if (valid) {
        info->fprintf_func(info->stream, "%s %s,%s",
                           mcs251_native_name(opcode),
                           destination, source);
    } else {
        info->fprintf_func(info->stream, "nop");
    }
}

static void mcs251_disas_native_incdec(MCS251DisasContext *ctx,
                                       uint8_t opcode)
{
    disassemble_info *info = ctx->info;
    uint8_t specifier = mcs251_disas_read8(ctx);
    unsigned code = FIELD_EX8(specifier, MCS251_SPECIFIER, CODE);
    unsigned mode = FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
    const char *name = opcode == 0x0b ? "inc" : "dec";
    char reg[16];
    char pointer[16];
    bool valid = true;

    if (mode == MCS251_MODE_WORD_INDIRECT_WR ||
        mode == MCS251_MODE_WORD_INDIRECT_DR) {
        uint8_t register_specifier = mcs251_disas_read8(ctx);
        unsigned register_code =
            FIELD_EX8(register_specifier, MCS251_SPECIFIER, CODE);

        valid =
            !FIELD_EX8(register_specifier, MCS251_SPECIFIER, MODE);
        mcs251_disas_wr(reg, sizeof(reg), register_code);
        if (mode == MCS251_MODE_WORD_INDIRECT_WR) {
            mcs251_disas_wr(pointer, sizeof(pointer), code);
        } else {
            valid &= mcs251_disas_dr(pointer, sizeof(pointer), code);
        }
        if (valid && opcode == 0x0b) {
            info->fprintf_func(info->stream, "mov %s,@%s",
                               reg, pointer);
        } else if (valid) {
            info->fprintf_func(info->stream, "mov @%s,%s",
                               pointer, reg);
        } else {
            info->fprintf_func(info->stream, "nop");
        }
        return;
    }

    switch (FIELD_EX8(mode, MCS251_INCDEC_MODE, WIDTH)) {
    case MCS251_WIDTH_BYTE:
        mcs251_disas_reg(reg, sizeof(reg), code);
        break;
    case MCS251_WIDTH_WORD:
        mcs251_disas_wr(reg, sizeof(reg), code);
        break;
    case MCS251_WIDTH_DWORD:
        valid = mcs251_disas_dr(reg, sizeof(reg), code);
        break;
    default:
        valid = false;
        break;
    }
    valid &= FIELD_EX8(mode, MCS251_INCDEC_MODE, STEP) !=
             MCS251_WIDTH_DWORD;
    if (valid) {
        info->fprintf_func(info->stream, "%s %s,#%u", name, reg,
                           BIT(FIELD_EX8(mode, MCS251_INCDEC_MODE, STEP)));
    } else {
        info->fprintf_func(info->stream, "nop");
    }
}

static void mcs251_disas_native_displacement(MCS251DisasContext *ctx,
                                             uint8_t opcode)
{
    disassemble_info *info = ctx->info;
    uint8_t specifier = mcs251_disas_read8(ctx);
    unsigned data_code =
        FIELD_EX8(specifier, MCS251_SPECIFIER, CODE);
    unsigned pointer_code =
        FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
    uint16_t displacement = mcs251_disas_read16(ctx);
    char reg[16];
    char pointer[16];
    bool valid = true;

    if (FIELD_EX8(opcode, MCS251_OPCODE, WIDE)) {
        mcs251_disas_wr(reg, sizeof(reg), data_code);
    } else {
        mcs251_disas_reg(reg, sizeof(reg), data_code);
    }
    if (FIELD_EX8(opcode, MCS251_OPCODE, LONG_POINTER)) {
        valid = mcs251_disas_dr(pointer, sizeof(pointer), pointer_code);
    } else {
        mcs251_disas_wr(pointer, sizeof(pointer), pointer_code);
    }

    if (!valid) {
        info->fprintf_func(info->stream, "nop");
    } else if (FIELD_EX8(opcode, MCS251_OPCODE, STORE)) {
        info->fprintf_func(info->stream, "mov @%s+0x%04x,%s",
                           pointer, displacement, reg);
    } else {
        info->fprintf_func(info->stream, "mov %s,@%s+0x%04x",
                           reg, pointer, displacement);
    }
}

static void mcs251_disas_native_move_store(MCS251DisasContext *ctx)
{
    disassemble_info *info = ctx->info;
    uint8_t specifier = mcs251_disas_read8(ctx);
    unsigned code = FIELD_EX8(specifier, MCS251_SPECIFIER, CODE);
    unsigned mode = FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
    char reg[16];
    char pointer[16];
    bool valid = true;

    if (mode == MCS251_MODE_MOVH) {
        uint16_t immediate = mcs251_disas_read16(ctx);

        valid = mcs251_disas_dr(reg, sizeof(reg), code);
        if (valid) {
            info->fprintf_func(info->stream, "movh %s,#0x%04x",
                               reg, immediate);
        } else {
            info->fprintf_func(info->stream, "nop");
        }
        return;
    }

    if (mode == MCS251_MODE_BYTE_INDIRECT_WR ||
        mode == MCS251_MODE_BYTE_INDIRECT_DR) {
        uint8_t source = mcs251_disas_read8(ctx);

        valid = !FIELD_EX8(source, MCS251_SPECIFIER, MODE);
        mcs251_disas_reg(
            reg, sizeof(reg),
            FIELD_EX8(source, MCS251_SPECIFIER, CODE));
        if (mode == MCS251_MODE_BYTE_INDIRECT_WR) {
            mcs251_disas_wr(pointer, sizeof(pointer), code);
        } else {
            valid &= mcs251_disas_dr(pointer, sizeof(pointer), code);
        }
        if (valid) {
            info->fprintf_func(info->stream, "mov @%s,%s",
                               pointer, reg);
        } else {
            info->fprintf_func(info->stream, "nop");
        }
        return;
    }

    switch (mode) {
    case MCS251_MODE_BYTE_DIRECT8:
        mcs251_disas_reg(reg, sizeof(reg), code);
        info->fprintf_func(info->stream, "mov 0x%02x,%s",
                           mcs251_disas_read8(ctx), reg);
        return;
    case MCS251_MODE_BYTE_DIRECT16:
        mcs251_disas_reg(reg, sizeof(reg), code);
        info->fprintf_func(info->stream, "mov 0x%04x,%s",
                           mcs251_disas_read16(ctx), reg);
        return;
    case MCS251_MODE_WORD_DIRECT8:
        mcs251_disas_wr(reg, sizeof(reg), code);
        info->fprintf_func(info->stream, "mov 0x%02x,%s",
                           mcs251_disas_read8(ctx), reg);
        return;
    case MCS251_MODE_WORD_DIRECT16:
        mcs251_disas_wr(reg, sizeof(reg), code);
        info->fprintf_func(info->stream, "mov 0x%04x,%s",
                           mcs251_disas_read16(ctx), reg);
        return;
    case MCS251_MODE_DWORD_DIRECT8: {
        uint8_t direct = mcs251_disas_read8(ctx);

        valid = mcs251_disas_dr(reg, sizeof(reg), code);
        if (valid) {
            info->fprintf_func(info->stream, "mov 0x%02x,%s",
                               direct, reg);
        }
        break;
    }
    case MCS251_MODE_DWORD_DIRECT16: {
        uint16_t address = mcs251_disas_read16(ctx);

        valid = mcs251_disas_dr(reg, sizeof(reg), code);
        if (valid) {
            info->fprintf_func(info->stream, "mov 0x%04x,%s",
                               address, reg);
        }
        break;
    }
    default:
        valid = false;
        break;
    }
    if (!valid) {
        info->fprintf_func(info->stream, "nop");
    }
}

static void mcs251_disas_native_bit(MCS251DisasContext *ctx)
{
    disassemble_info *info = ctx->info;
    uint8_t specifier = mcs251_disas_read8(ctx);
    unsigned operation =
        FIELD_EX8(specifier, MCS251_BIT_SPECIFIER, OPERATION);
    unsigned bit = FIELD_EX8(specifier, MCS251_BIT_SPECIFIER, BIT);
    uint8_t direct = mcs251_disas_read8(ctx);
    bool branch = operation == MCS251_BIT_JBC ||
                  operation == MCS251_BIT_JB ||
                  operation == MCS251_BIT_JNB;
    uint32_t target = 0;

    if (branch) {
        target = mcs251_disas_relative(ctx);
    }
    if (direct < 0x20) {
        info->fprintf_func(info->stream, "nop");
        return;
    }

    switch (operation) {
    case MCS251_BIT_JBC:
        info->fprintf_func(info->stream, "jbc 0x%02x.%u,0x%06x",
                           direct, bit, target);
        break;
    case MCS251_BIT_JB:
        info->fprintf_func(info->stream, "jb 0x%02x.%u,0x%06x",
                           direct, bit, target);
        break;
    case MCS251_BIT_JNB:
        info->fprintf_func(info->stream, "jnb 0x%02x.%u,0x%06x",
                           direct, bit, target);
        break;
    case MCS251_BIT_ORL:
        info->fprintf_func(info->stream, "orl c,0x%02x.%u",
                           direct, bit);
        break;
    case MCS251_BIT_ANL:
        info->fprintf_func(info->stream, "anl c,0x%02x.%u",
                           direct, bit);
        break;
    case MCS251_BIT_MOV_FROM_C:
        info->fprintf_func(info->stream, "mov 0x%02x.%u,c",
                           direct, bit);
        break;
    case MCS251_BIT_MOV_TO_C:
        info->fprintf_func(info->stream, "mov c,0x%02x.%u",
                           direct, bit);
        break;
    case MCS251_BIT_CPL:
        info->fprintf_func(info->stream, "cpl 0x%02x.%u",
                           direct, bit);
        break;
    case MCS251_BIT_CLR:
        info->fprintf_func(info->stream, "clr 0x%02x.%u",
                           direct, bit);
        break;
    case MCS251_BIT_SETB:
        info->fprintf_func(info->stream, "setb 0x%02x.%u",
                           direct, bit);
        break;
    case MCS251_BIT_ORL_NOT:
        info->fprintf_func(info->stream, "orl c,/0x%02x.%u",
                           direct, bit);
        break;
    case MCS251_BIT_ANL_NOT:
        info->fprintf_func(info->stream, "anl c,/0x%02x.%u",
                           direct, bit);
        break;
    default:
        info->fprintf_func(info->stream, "nop");
        break;
    }
}

static void mcs251_disas_native_pushpop(MCS251DisasContext *ctx,
                                        uint8_t opcode)
{
    disassemble_info *info = ctx->info;
    uint8_t specifier = mcs251_disas_read8(ctx);
    unsigned code = FIELD_EX8(specifier, MCS251_SPECIFIER, CODE);
    unsigned mode = FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
    const char *name = opcode == 0xca ? "push" : "pop";
    char reg[16];
    bool valid = true;

    if (opcode == 0xca && specifier == 0x02) {
        info->fprintf_func(info->stream, "push #0x%02x",
                           mcs251_disas_read8(ctx));
        return;
    }
    if (opcode == 0xca && specifier == 0x06) {
        info->fprintf_func(info->stream, "push #0x%04x",
                           mcs251_disas_read16(ctx));
        return;
    }

    switch (mode) {
    case MCS251_STACK_BYTE_REGISTER:
        mcs251_disas_reg(reg, sizeof(reg), code);
        break;
    case MCS251_STACK_WORD_REGISTER:
        mcs251_disas_wr(reg, sizeof(reg), code);
        break;
    case MCS251_STACK_DWORD_REGISTER:
        valid = mcs251_disas_dr(reg, sizeof(reg), code);
        break;
    default:
        valid = false;
        break;
    }

    if (valid) {
        info->fprintf_func(info->stream, "%s %s", name, reg);
    } else {
        info->fprintf_func(info->stream, "nop");
    }
}

static void mcs251_disas_source(MCS251DisasContext *ctx, uint8_t opcode)
{
    disassemble_info *info = ctx->info;
    uint8_t specifier;
    unsigned code;
    unsigned mode;
    char destination[16];
    char source[16];
    bool valid;

    switch (opcode) {
    case 0x08:
    case 0x18:
    case 0x28:
    case 0x38:
    case 0x48:
    case 0x58:
    case 0x68:
    case 0x78: {
        static const char * const names[] = {
            [0] = "jsle",
            [1] = "jsg",
            [2] = "jle",
            [3] = "jg",
            [4] = "jsl",
            [5] = "jsge",
            [6] = "je",
            [7] = "jne",
        };
        uint32_t target = mcs251_disas_relative(ctx);

        info->fprintf_func(
            info->stream, "%s 0x%06x",
            names[FIELD_EX8(opcode, MCS251_OPCODE, HIGH_NIBBLE)],
            target);
        break;
    }
    case 0x09:
    case 0x19:
    case 0x29:
    case 0x39:
    case 0x49:
    case 0x59:
    case 0x69:
    case 0x79:
        mcs251_disas_native_displacement(ctx, opcode);
        break;
    case 0x0a:
    case 0x1a:
        specifier = mcs251_disas_read8(ctx);
        mcs251_disas_wr(
            destination, sizeof(destination),
            FIELD_EX8(specifier, MCS251_SPECIFIER, CODE));
        mcs251_disas_reg(
            source, sizeof(source),
            FIELD_EX8(specifier, MCS251_SPECIFIER, MODE));
        info->fprintf_func(info->stream, "%s %s,%s",
                           opcode == 0x0a ? "movz" : "movs",
                           destination, source);
        break;
    case 0x0b:
    case 0x1b:
        mcs251_disas_native_incdec(ctx, opcode);
        break;
    case 0x0e:
    case 0x1e:
    case 0x3e:
        specifier = mcs251_disas_read8(ctx);
        code = FIELD_EX8(specifier, MCS251_SPECIFIER, CODE);
        mode = FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
        valid = mode == 0 || mode == 4;
        if (mode == 0) {
            mcs251_disas_reg(destination, sizeof(destination), code);
        } else {
            mcs251_disas_wr(destination, sizeof(destination), code);
        }
        if (valid) {
            info->fprintf_func(
                info->stream, "%s %s",
                opcode == 0x0e ? "sra" :
                opcode == 0x1e ? "srl" : "sll",
                destination);
        } else {
            info->fprintf_func(info->stream, "nop");
        }
        break;
    case 0x2c:
    case 0x2d:
    case 0x2f:
    case 0x4c:
    case 0x4d:
    case 0x5c:
    case 0x5d:
    case 0x6c:
    case 0x6d:
    case 0x7c:
    case 0x7d:
    case 0x7f:
    case 0x9c:
    case 0x9d:
    case 0x9f:
    case 0xbc:
    case 0xbd:
    case 0xbf:
        mcs251_disas_native_register(ctx, opcode);
        break;
    case 0x2e:
    case 0x4e:
    case 0x5e:
    case 0x6e:
    case 0x7e:
    case 0x9e:
    case 0xbe:
        mcs251_disas_native_generic(ctx, opcode);
        break;
    case 0x7a:
        mcs251_disas_native_move_store(ctx);
        break;
    case 0x89:
    case 0x99:
        specifier = mcs251_disas_read8(ctx);
        code = FIELD_EX8(specifier, MCS251_SPECIFIER, CODE);
        mode = FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
        if (mode == 4) {
            mcs251_disas_wr(source, sizeof(source), code);
            info->fprintf_func(info->stream, "%s @%s",
                               opcode == 0x89 ? "ljmp" : "lcall",
                               source);
        } else if (mode == 8 &&
                   mcs251_disas_dr(source, sizeof(source), code)) {
            info->fprintf_func(info->stream, "%s @%s",
                               opcode == 0x89 ? "ejmp" : "ecall",
                               source);
        } else {
            info->fprintf_func(info->stream, "nop");
        }
        break;
    case 0x8a:
        info->fprintf_func(info->stream, "ejmp 0x%06x",
                           mcs251_disas_read24(ctx));
        break;
    case 0x8c:
    case 0x8d:
    case 0xac:
    case 0xad:
        specifier = mcs251_disas_read8(ctx);
        code = FIELD_EX8(specifier, MCS251_SPECIFIER, CODE);
        mode = FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
        if (FIELD_EX8(opcode, MCS251_OPCODE, RI)) {
            mcs251_disas_wr(destination, sizeof(destination), code);
            mcs251_disas_wr(source, sizeof(source), mode);
        } else {
            mcs251_disas_reg(destination, sizeof(destination), code);
            mcs251_disas_reg(source, sizeof(source), mode);
        }
        info->fprintf_func(info->stream, "%s %s,%s",
                           opcode == 0x8c || opcode == 0x8d ?
                           "div" : "mul",
                           destination, source);
        break;
    case 0x9a:
        info->fprintf_func(info->stream, "ecall 0x%06x",
                           mcs251_disas_read24(ctx));
        break;
    case 0xa9:
        mcs251_disas_native_bit(ctx);
        break;
    case 0xaa:
        info->fprintf_func(info->stream, "eret");
        break;
    case 0xb9:
        info->fprintf_func(info->stream, "trap");
        break;
    case 0xca:
    case 0xda:
        mcs251_disas_native_pushpop(ctx, opcode);
        break;
    default:
        info->fprintf_func(info->stream, "nop");
        break;
    }
}

int mcs251_print_insn(bfd_vma addr, disassemble_info *info)
{
    MCS251DisasContext ctx = {
        .info = info,
        .cursor = addr,
    };
    MCS251DecodedInsn decoded;
    unsigned prefix;
    bool mcs51 = info->mach == MCS251_DISAS_MCS51;
    bool source_mode = info->mach == MCS251_DISAS_SOURCE;

    if (!mcs251_decode_insn(addr & MCS_TARGET_ADDR_MASK, source_mode, !mcs51,
                            mcs251_disas_read_at, &ctx, &decoded)) {
        info->memory_error_func(ctx.status, ctx.error_address, info);
        return -1;
    }

    ctx.cursor = addr + decoded.prefixes + 1;
    for (prefix = 0; prefix < decoded.prefixes; prefix++) {
        info->fprintf_func(info->stream, "esc ");
    }

    if (decoded.source_mode &&
        FIELD_EX8(decoded.opcode, MCS251_OPCODE, LOW_NIBBLE) > 5) {
        mcs251_disas_source(&ctx, decoded.opcode);
    } else {
        mcs251_disas_classic(&ctx, decoded.opcode);
    }

    if (ctx.status) {
        info->memory_error_func(ctx.status, ctx.error_address, info);
        return -1;
    }
    return decoded.length;
}
