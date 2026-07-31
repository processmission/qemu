/*
 * MCS-51 family CPU QOM declarations
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_MCS51_CPU_QOM_H
#define TARGET_MCS51_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_MCS51_CPU "mcs51-cpu"
#define TYPE_MCS251_CPU "mcs251-cpu"

OBJECT_DECLARE_CPU_TYPE(MCS251CPU, MCS251CPUClass, MCS51_CPU)

typedef MCS251CPU MCS51CPU;
#define MCS251_CPU(obj) MCS51_CPU(obj)
#define MCS251_CPU_CLASS(klass) MCS51_CPU_CLASS(klass)
#define MCS251_CPU_GET_CLASS(obj) MCS51_CPU_GET_CLASS(obj)

#endif
