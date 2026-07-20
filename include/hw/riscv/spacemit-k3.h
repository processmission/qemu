/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SpacemiT K3 SoC and Pico-ITX machine
 */

#ifndef HW_SPACEMIT_K3_H
#define HW_SPACEMIT_K3_H

#include "exec/hwaddr.h"
#include "hw/char/serial-mm.h"
#include "hw/core/boards.h"
#include "hw/cpu/cluster.h"
#include "hw/misc/spacemit-k3.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/sd/spacemit-k3-sdhci.h"

#define K3_PICO_ITX_NUM_CLUSTERS       2
#define K3_PICO_ITX_HARTS_PER_CLUSTER  4
#define K3_PICO_ITX_NUM_HARTS          8

#define K3_PICO_ITX_TIMEBASE_FREQ      24000000
#define K3_PICO_ITX_APLIC_NUM_SOURCES  512
#define K3_PICO_ITX_APLIC_IPRIO_BITS   8
#define K3_PICO_ITX_IMSIC_NUM_IDS      511
#define K3_PICO_ITX_IOMMU_IRQ          234
#define K3_PICO_ITX_UART0_IRQ          42
#define K3_PICO_ITX_SDHCI0_IRQ         99

enum {
    K3_DEV_SRAM,
    K3_DEV_DDR_TRAINING,
    K3_DEV_IOMMU,
    K3_DEV_UART0,
    K3_DEV_SDHCI0,
    K3_DEV_APMU,
    K3_DEV_CIU,
    K3_DEV_S_IMSIC,
    K3_DEV_S_APLIC,
    K3_DEV_M_IMSIC,
    K3_DEV_M_APLIC,
    K3_DEV_M_CLINT,
    K3_DEV_FIRMWARE,
    K3_DEV_DRAM,
};

extern const MemMapEntry spacemit_k3_memmap[];

#define TYPE_SPACEMIT_K3_SOC "spacemit.k3.soc"
OBJECT_DECLARE_SIMPLE_TYPE(SpacemitK3SoCState, SPACEMIT_K3_SOC)

struct SpacemitK3SoCState {
    DeviceState parent_obj;

    CPUClusterState clusters[K3_PICO_ITX_NUM_CLUSTERS];
    RISCVHartArrayState cpus[K3_PICO_ITX_NUM_CLUSTERS];
    MemoryRegion sram;
    MemoryRegion ddr_training;
    MemoryRegion firmware;
    MemoryRegion uart0_mem;
    SpacemitK3APMUState apmu;
    SpacemitK3CIUState ciu;
    SpacemitK3SDHCIState sdhci0;
    DeviceState *m_imsic[K3_PICO_ITX_NUM_HARTS];
    DeviceState *s_imsic[K3_PICO_ITX_NUM_HARTS];
    DeviceState *m_aplic;
    DeviceState *s_aplic;
    DeviceState *iommu;
    DeviceState *swi;
    DeviceState *mtimer;
    SerialMM *uart0;
};

#define TYPE_K3_PICO_ITX_MACHINE MACHINE_TYPE_NAME("k3-pico-itx")
OBJECT_DECLARE_SIMPLE_TYPE(K3PicoITXState, K3_PICO_ITX_MACHINE)

struct K3PicoITXState {
    MachineState parent_obj;

    SpacemitK3SoCState soc;
    int fdt_size;
};

#endif
