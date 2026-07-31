/*
 * STC32G MCS-251 disassembler
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "disas/dis-asm.h"
#include "cpu.h"

int mcs251_print_insn(bfd_vma addr, disassemble_info *info)
{
    bfd_byte opcode;
    int status;

    status = info->read_memory_func(addr, &opcode, 1, info);
    if (status) {
        info->memory_error_func(status, addr, info);
        return -1;
    }

    switch (opcode) {
    case 0x00:
        info->fprintf_func(info->stream, "nop");
        break;
    case 0x22:
        info->fprintf_func(info->stream, "ret");
        break;
    case 0x32:
        info->fprintf_func(info->stream, "reti");
        break;
    case 0xa5:
        info->fprintf_func(info->stream, "esc");
        break;
    case 0xaa:
        info->fprintf_func(info->stream, "eret");
        break;
    default:
        info->fprintf_func(info->stream, ".byte 0x%02x", opcode);
        break;
    }
    return 1;
}
