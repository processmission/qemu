/*
 * MCS-51 family instruction length decoder
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "internals.h"
#include "decode.h"

typedef struct MCS251DecodeContext {
    MCS251ReadByte *read_byte;
    void *opaque;
    uint32_t pc;
    unsigned length;
} MCS251DecodeContext;

static bool mcs251_decode_read(MCS251DecodeContext *ctx, uint8_t *value)
{
    if (!ctx->read_byte(ctx->opaque, ctx->pc, value)) {
        return false;
    }
    ctx->pc = (ctx->pc + 1) & MCS_TARGET_ADDR_MASK;
    ctx->length++;
    return true;
}

static bool mcs251_decode_skip(MCS251DecodeContext *ctx, unsigned bytes)
{
    uint8_t value;

    while (bytes--) {
        if (!mcs251_decode_read(ctx, &value)) {
            return false;
        }
    }
    return true;
}

static unsigned mcs251_classic_length(uint8_t opcode)
{
    unsigned group = FIELD_EX8(opcode, MCS251_OPCODE, GROUP5);

    if (FIELD_EX8(opcode, MCS251_OPCODE, LOW5) == 0x01 ||
        FIELD_EX8(opcode, MCS251_OPCODE, LOW5) == 0x11) {
        return 2;
    }

    switch (group) {
    case 0x0f:
    case 0x11:
    case 0x15:
        return 2;
    case 0x17:
        return 3;
    case 0x1b:
        return 2;
    default:
        break;
    }

    switch (opcode) {
    case 0x02:
    case 0x12:
    case 0x10:
    case 0x20:
    case 0x30:
    case 0x43:
    case 0x53:
    case 0x63:
    case 0x75:
    case 0x85:
    case 0x90:
    case 0xb4:
    case 0xb5:
    case 0xb6:
    case 0xb7:
    case 0xd5:
        return 3;
    case 0x05:
    case 0x15:
    case 0x24:
    case 0x25:
    case 0x34:
    case 0x35:
    case 0x40:
    case 0x42:
    case 0x44:
    case 0x45:
    case 0x50:
    case 0x52:
    case 0x54:
    case 0x55:
    case 0x60:
    case 0x62:
    case 0x64:
    case 0x65:
    case 0x70:
    case 0x72:
    case 0x74:
    case 0x76:
    case 0x77:
    case 0x80:
    case 0x82:
    case 0x86:
    case 0x87:
    case 0x92:
    case 0x94:
    case 0x95:
    case 0xa0:
    case 0xa2:
    case 0xa6:
    case 0xa7:
    case 0xb0:
    case 0xb2:
    case 0xc0:
    case 0xc2:
    case 0xc5:
    case 0xd0:
    case 0xd2:
    case 0xe5:
    case 0xf5:
        return 2;
    default:
        return 1;
    }
}

static bool mcs251_decode_source(MCS251DecodeContext *ctx, uint8_t opcode)
{
    uint8_t specifier;
    unsigned mode;
    unsigned extra = 0;

    if (FIELD_EX8(opcode, MCS251_OPCODE, LOW_NIBBLE) <= 5) {
        return mcs251_decode_skip(ctx, mcs251_classic_length(opcode) - 1);
    }

    switch (opcode) {
    case 0x08:
    case 0x18:
    case 0x28:
    case 0x38:
    case 0x48:
    case 0x58:
    case 0x68:
    case 0x78:
        extra = 1;
        break;
    case 0x09:
    case 0x19:
    case 0x29:
    case 0x39:
    case 0x49:
    case 0x59:
    case 0x69:
    case 0x79:
        extra = 3;
        break;
    case 0x0a:
    case 0x1a:
    case 0x0e:
    case 0x1e:
    case 0x2c:
    case 0x2d:
    case 0x2f:
    case 0x3e:
    case 0x4c:
    case 0x4d:
    case 0x5c:
    case 0x5d:
    case 0x6c:
    case 0x6d:
    case 0x7c:
    case 0x7d:
    case 0x7f:
    case 0x89:
    case 0x8c:
    case 0x8d:
    case 0x99:
    case 0x9c:
    case 0x9d:
    case 0x9f:
    case 0xac:
    case 0xad:
    case 0xbc:
    case 0xbd:
    case 0xbf:
        extra = 1;
        break;
    case 0x8a:
    case 0x9a:
        extra = 3;
        break;
    case 0xa9:
        if (!mcs251_decode_read(ctx, &specifier)) {
            return false;
        }
        mode = FIELD_EX8(specifier, MCS251_BIT_SPECIFIER, OPERATION);
        extra = mode == MCS251_BIT_JBC ||
                mode == MCS251_BIT_JB ||
                mode == MCS251_BIT_JNB ? 2 : 1;
        break;
    case 0x0b:
    case 0x1b:
        if (!mcs251_decode_read(ctx, &specifier)) {
            return false;
        }
        mode = FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
        extra = mode == MCS251_MODE_DWORD_ZERO_IMMEDIATE ||
                mode == MCS251_MODE_WORD_INDIRECT_DR ? 1 : 0;
        break;
    case 0x2e:
    case 0x4e:
    case 0x5e:
    case 0x6e:
    case 0x7e:
    case 0x9e:
    case 0xbe:
        if (!mcs251_decode_read(ctx, &specifier)) {
            return false;
        }
        mode = FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
        switch (mode) {
        case MCS251_MODE_BYTE_IMMEDIATE:
        case MCS251_MODE_BYTE_DIRECT8:
        case MCS251_MODE_WORD_DIRECT8:
        case MCS251_MODE_BYTE_INDIRECT_WR:
        case MCS251_MODE_BYTE_INDIRECT_DR:
        case MCS251_MODE_DWORD_DIRECT8:
            extra = 1;
            break;
        case MCS251_MODE_BYTE_DIRECT16:
        case MCS251_MODE_WORD_IMMEDIATE:
        case MCS251_MODE_WORD_DIRECT16:
        case MCS251_MODE_DWORD_ZERO_IMMEDIATE:
        case MCS251_MODE_DWORD_SIGNED_IMMEDIATE:
        case MCS251_MODE_DWORD_DIRECT16:
            extra = 2;
            break;
        default:
            break;
        }
        break;
    case 0x7a:
        if (!mcs251_decode_read(ctx, &specifier)) {
            return false;
        }
        mode = FIELD_EX8(specifier, MCS251_SPECIFIER, MODE);
        switch (mode) {
        case MCS251_MODE_BYTE_DIRECT8:
        case MCS251_MODE_WORD_DIRECT8:
        case MCS251_MODE_BYTE_INDIRECT_WR:
        case MCS251_MODE_BYTE_INDIRECT_DR:
        case MCS251_MODE_DWORD_DIRECT8:
            extra = 1;
            break;
        case MCS251_MODE_BYTE_DIRECT16:
        case MCS251_MODE_WORD_DIRECT16:
        case MCS251_MODE_MOVH:
        case MCS251_MODE_DWORD_DIRECT16:
            extra = 2;
            break;
        default:
            break;
        }
        break;
    case 0xca:
    case 0xda:
        if (!mcs251_decode_read(ctx, &specifier)) {
            return false;
        }
        if (opcode == 0xca && specifier == 0x02) {
            extra = 1;
        } else if (opcode == 0xca && specifier == 0x06) {
            extra = 2;
        }
        break;
    default:
        break;
    }

    return mcs251_decode_skip(ctx, extra);
}

bool mcs251_decode_insn(uint32_t pc, bool source_mode, bool escape_enabled,
                        MCS251ReadByte *read_byte, void *opaque,
                        MCS251DecodedInsn *decoded)
{
    MCS251DecodeContext ctx = {
        .read_byte = read_byte,
        .opaque = opaque,
        .pc = pc,
    };
    uint8_t opcode;

    if (!mcs251_decode_read(&ctx, &opcode)) {
        return false;
    }
    decoded->first_opcode = opcode;
    decoded->prefixes = 0;

    while (escape_enabled && opcode == MCS251_OPCODE_ESCAPE) {
        decoded->prefixes++;
        source_mode = !source_mode;
        if (!mcs251_decode_read(&ctx, &opcode)) {
            return false;
        }
    }

    decoded->opcode = opcode;
    decoded->source_mode = source_mode;
    if (source_mode) {
        if (!mcs251_decode_source(&ctx, opcode)) {
            return false;
        }
    } else if (!mcs251_decode_skip(&ctx,
                                   mcs251_classic_length(opcode) - 1)) {
        return false;
    }

    decoded->length = ctx.length;
    return true;
}
