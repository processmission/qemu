/*
 * K230 security register block
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_K230_SECURITY_H
#define HW_MISC_K230_SECURITY_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_K230_SECURITY "riscv.k230.security"
OBJECT_DECLARE_SIMPLE_TYPE(K230SecurityState, K230_SECURITY)

#define K230_SECURITY_SIZE 0x8000
#define K230_OTP_SIZE 0x300

struct K230SecurityState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    uint8_t regs[K230_SECURITY_SIZE];
    uint8_t otp[K230_OTP_SIZE];
};

#endif /* HW_MISC_K230_SECURITY_H */
