/*
 * MCS-51 family instruction length decoder
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_MCS51_DECODE_H
#define TARGET_MCS51_DECODE_H

typedef bool MCS251ReadByte(void *opaque, uint32_t address, uint8_t *value);

typedef struct MCS251DecodedInsn {
    unsigned length;
    unsigned prefixes;
    uint8_t first_opcode;
    uint8_t opcode;
    bool source_mode;
} MCS251DecodedInsn;

bool mcs251_decode_insn(uint32_t pc, bool source_mode, bool escape_enabled,
                        MCS251ReadByte *read_byte, void *opaque,
                        MCS251DecodedInsn *decoded);

#endif
