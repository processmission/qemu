/*
 * MCS-51 clock conversion helpers
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MCS51_CLOCK_H
#define HW_MCS51_CLOCK_H

#include "hw/core/clock.h"

/*
 * Convert elapsed virtual time to clock cycles without discarding the
 * fractional part.  @remainder is a 2^-64 fraction of one source-clock
 * cycle, so the phase is retained when the clock period changes.
 */
static inline uint64_t mcs51_clock_elapsed_cycles(Clock *clock,
                                                   uint64_t elapsed_ns,
                                                   uint64_t *remainder)
{
    uint64_t low;
    uint64_t high;
    uint64_t remainder_low;
    uint64_t remainder_high;
    uint64_t residue;
    uint64_t cycles;

    if (!clock_is_enabled(clock)) {
        return 0;
    }

    mulu64(&low, &high, elapsed_ns, 1ULL << 32);
    mulu64(&remainder_low, &remainder_high, clock_get(clock), *remainder);
    remainder_low = remainder_high;
    if (low + remainder_low < low) {
        high++;
    }
    low += remainder_low;
    residue = divu128(&low, &high, clock_get(clock));
    cycles = low;
    low = 0;
    high = residue;
    divu128(&low, &high, clock_get(clock));
    *remainder = low;
    return cycles;
}

/*
 * Return the ceiling duration required to complete @cycles, accounting for
 * the fractional cycle already present in @remainder.
 */
static inline uint64_t mcs51_clock_cycles_to_ns(Clock *clock,
                                                 uint64_t cycles,
                                                 uint64_t remainder)
{
    uint64_t low;
    uint64_t high;
    uint64_t remainder_low;
    uint64_t remainder_high;
    uint64_t ns;

    if (!cycles || !clock_is_enabled(clock)) {
        return 0;
    }
    mulu64(&low, &high, clock_get(clock), cycles);
    mulu64(&remainder_low, &remainder_high, clock_get(clock), remainder);
    remainder_low = remainder_high;
    if (low < remainder_low) {
        high--;
    }
    low -= remainder_low;
    if (high & MAKE_64BIT_MASK(31, 33)) {
        return INT64_MAX;
    }
    ns = low >> 32 | high << 32;
    return ns + (extract32(low, 0, 32) != 0);
}

#endif
