/*
 * STC32G MCS-251 CPU QOM declarations
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TARGET_MCS251_CPU_QOM_H
#define TARGET_MCS251_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_MCS251_CPU "mcs251-cpu"

OBJECT_DECLARE_CPU_TYPE(MCS251CPU, MCS251CPUClass, MCS251_CPU)

#define MCS251_CPU_TYPE_NAME(name) (name "-cpu")
#define TYPE_STC32G_CPU MCS251_CPU_TYPE_NAME("stc32g")

#endif
