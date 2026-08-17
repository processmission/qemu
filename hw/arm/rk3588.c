/*
 * Local-only Rockchip RK3588 board machine models
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "system/block-backend-io.h"
#include "exec/hwaddr.h"
#include "exec/translation-block.h"
#include "system/device_tree.h"
#include "system/kvm.h"
#include "system/memory.h"
#include "system/numa.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/system.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/linux-boot-if.h"
#include "rk3588-internal.h"
#include "hw/gpio/rockchip_gpio.h"
#include "hw/misc/rockchip_crypto_v2.h"
#include "hw/misc/rockchip_iommu.h"
#include "hw/misc/rk3588_rng.h"
#include "hw/misc/rk3588_tsadc.h"
#include "hw/misc/rk3588_rknpu.h"
#include "hw/misc/rockchip_syscon.h"
#include "hw/misc/rk3588_atf_ddr.h"
#include "hw/misc/rk3588_ddr.h"
#include "hw/misc/rk3588_firmware_mmio.h"
#include "hw/misc/rk3588_grf.h"
#include "hw/misc/rk3588_scmi.h"
#include "hw/net/dwmac4.h"
#include "hw/nvram/rk3588_secure_otp.h"
#include "hw/pci-host/designware.h"
#include "hw/sd/dw_mmc.h"
#include "hw/sd/rockchip_dwcmshc.h"
#include "hw/sd/sd.h"
#include "hw/sd/sdhci.h"
#include "hw/i2c/rk3x_i2c.h"
#include "hw/rtc/hym8563.h"
#include "hw/ssi/rk806.h"
#include "hw/ssi/rockchip_sfc.h"
#include "hw/ssi/rockchip_spi.h"
#include "hw/misc/rk3588_pl330.h"
#include "hw/ssi/rockchip_sfc.h"
#include "hw/timer/rockchip_stimer.h"
#include "hw/usb/rk3588_dwc3_udc.h"
#include "hw/usb/rk3588_usb2_host.h"
#include "net/net.h"
#include "system/block-backend.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/dw-apb-uart-vendor.h"
#include "hw/char/serial-mm.h"
#include "hw/core/boards.h"
#include "hw/core/cpu.h"
#include "hw/core/or-irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/intc/arm_gicv3_its_common.h"
#include "hw/misc/rk3588_cru.h"
#include "hw/pci-host/rockchip_pcie.h"
#include "qobject/qlist.h"
#include "target/arm/cpu.h"
#include "target/arm/cpu-qom.h"
#include "target/arm/gtimer.h"
#include "target/arm/internals.h"

#include <libfdt.h>

OBJECT_DECLARE_SIMPLE_TYPE(RK3588MachineState, RK3588_MACHINE)

#define RK3588_MAX_CPUS 8
#define RK3588_NUM_SPI_IRQS 512
#define RK3588_GTIMER_HZ 24000000
#define RK3588_UART_BAUDBASE 1500000
#define RK3588_DEFAULT_RAM_BASE 0x00200000ULL
/*
 * The low RAM window ends where the first PCIe config window starts
 * (0xf0000000); RAM above it lives in the high window at 4 GiB.
 */
#define RK3588_LOW_RAM_END 0xf0000000ULL
#define RK3588_HIGH_RAM_BASE 0x100000000ULL
#define RK3588_MAX_RAM_SIZE (16 * GiB)
#define RK3588_ZEPHYR_RAM_BASE 0x10000000ULL
#define RK3588_SRAM_SIZE MiB
#define RK3588_IRAM_SIZE 0x00ff0000
#define RK3588_ATAGS_SIZE (8 * KiB)
#define RK3588_ZVM_LOW_RAM_BASE 0x68000000ULL
#define RK3588_ZVM_LOW_RAM_LIMIT 0xf0000000ULL
#define RK3588_ZVM_HIGH_RAM_BASE 0x100000000ULL
#define RK3588_ZVM_HIGH_RAM_SIZE 0x100000000ULL
#define RK3588_BROM_TRAMPOLINE 0xffff0000ULL
#define RK3588_TPL_LOAD_ADDR 0xff001000ULL
#define RK3588_BROM_SMC_NEXT_STAGE 0xc2003588
#define RK3588_QEMU_SMC_UBOOT_HANDOFF 0xc2003589
#define RK3588_QEMU_SMC_ATF_ENTRY 0x358a
#define RK3588_QEMU_SMC_BL31_EXIT 0x358b
/*
 * Rockchip SIP services consumed by the vendor fiq-debugger console
 * (drivers/soc/rockchip/fiq_debugger + drivers/firmware/rockchip_sip.c).
 * The guest's ttyFIQ0 console writes UART2 registers directly, but its
 * probe calls into "secure" firmware; answer so the console registers.
 */
#define RK3588_SIP_SHARE_MEM 0x82000009
#define RK3588_SIP_UARTDBG_CFG64 0xc2000005
#define RK3588_UARTDBG_CFG_INIT 0xf0
#define RK3588_UARTDBG_CFG_OSHDL_TO_OS 0xf1
#define RK3588_UARTDBG_CFG_CPUSW 0xf3
#define RK3588_UARTDBG_CFG_DEBUG_ENABLE 0xf4
#define RK3588_UARTDBG_CFG_DEBUG_DISABLE 0xf5
#define RK3588_UARTDBG_CFG_PRINT_PORT 0xf7
#define RK3588_UARTDBG_CFG_FIQ_ENABLE 0xf8
#define RK3588_UARTDBG_CFG_FIQ_DISABLE 0xf9
#define RK3588_SIP_RET_SUCCESS 0
/*
 * Physical address handed out as the ATF/OS shared page for the FIQ
 * debugger (2 pages).  Lives inside the firmware scratch window so the
 * guest's ioremap lands on model RAM rather than device space.
 */
#define RK3588_FIQ_SHARE_MEM_ADDR 0x00108000ULL
#define RK3588_FIQ_SHARE_MEM_SIZE (8 * KiB)
#define RK3588_RKNS_MAGIC 0x534e4b52
#define RK3588_RKNS_LBA 64
#define RK3588_RKNS_HEADER_SIZE 2048
#define RK3588_RKNS_SECTOR_SIZE 512
#define RK3588_UBOOT_LOAD_ADDR 0x00800000ULL
/*
 * U-Boot proper loads the kernel DTB from the boot media into low RAM
 * before jumping to the kernel (booti).  The vendor Radxa images put it
 * at the standard Rockchip address.  booti() rewrites /chosen/bootargs
 * from U-Boot's own environment immediately before entering the kernel,
 * so when the firmware-bootargs property is set the machine keeps
 * re-patching /chosen/bootargs (idempotently) on a short timer until
 * the kernel entry PC is seen; after that the DTB is left untouched
 * for the kernel to walk.  The unmodified official image can then be
 * booted headless without an interactive U-Boot session.
 */
#define RK3588_UBOOT_DTB_ADDR 0x08300000ULL
/*
 * The main loop polls timer deadlines on real time while TCG runs the
 * guest faster than real time, so a 500us deadline is ~2ms of guest
 * time and can miss the kernel's short low-RAM phase.  50us keeps the
 * guest-time tick period well under a millisecond.
 */
#define RK3588_FIRMWARE_BOOTARGS_INTERVAL_NS (50 * SCALE_US)
#define RK3588_FIRMWARE_BOOTARGS_DEADLINE_NS (300ULL * 1000 * SCALE_MS)
/*
 * The vendor image's U-Boot loads the kernel image at 0x00400000 (the
 * kernel's early code runs there in low RAM until __primary_switch,
 * then at high virtual addresses); the DTB sits at 0x08300000, outside
 * the range below.  U-Boot itself runs from high RAM after relocation
 * and never uses high virtual addresses.
 */
#define RK3588_KERNEL_LOAD_ADDR 0x00400000ULL
#define RK3588_KERNEL_ENTRY_RANGE 0x00400000ULL
#define RK3588_KERNEL_VA_BASE 0xffff800000000000ULL
/* Slack kept in the patched DTB so U-Boot's own booti fixups still fit. */
#define RK3588_FIRMWARE_BOOTARGS_SLACK 0x4000
#define RK3588_UBOOT_ENTRY_BRANCH 0x1400000a
#define RK3588_SPL_ATF_CALL_ADDR 0x00002a98ULL
#define RK3588_SECURE_OTP_BASE 0xfe3a0000ULL
#define RK3588_DDR_LEGACY_BASE 0xf7000000ULL
#define RK3588_DDR_LEGACY_PHY_BASE 0xfd800000ULL
#define RK3588_DDR_LEGACY_PHY_AUX_BASE 0xfe000000ULL
#define RK3588_DDR_GLOBAL_BASE 0xfd000000ULL
#define RK3588_DDR_CHANNEL_BASE 0xfd100000ULL
#define RK3588_DDRPHY_BASE 0xfd8d8000ULL
#define RK3588_DDR_PHY_GATE_BASE 0xfe0c0000ULL
#define RK3588_PMUSRAM_SKIP_ADDR 0xff101764ULL
#define RK3588_BL31_BASE 0x00060000ULL
#define RK3588_BL31_LIMIT 0x00090000ULL
#define RK3588_BL31_PMUSRAM_COPY_CALL_ADDR 0x0006f2f8ULL
#define RK3588_BL31_PMUSRAM_RUN_CALL_ADDR 0x0006f2fcULL
#define RK3588_BL31_RMR_CBZ_ADDR 0x0006f350ULL
#define RK3588_BL31_MMU_ENABLE_ADDR 0x000796d8ULL
#define RK3588_BL31_MMU_ENABLE_BODY_ADDR 0x000796e4ULL
#define RK3588_BL31_EXIT_LR_ADDR 0x00079210ULL
#define RK3588_BL31_EXIT_ERET_ADDR 0x00079214ULL
#define RK3588_BL31_PMUSRAM_COPY_CALL 0x97fffc4b
#define RK3588_BL31_PMUSRAM_RUN_CALL 0x97ffc391
#define RK3588_BL31_RMR_CBZ 0x340002f3
#define RK3588_BL31_RMR_SKIP 0x1400000f
#define RK3588_BL31_MMU_ENABLE_BRANCH 0x14000003
#define RK3588_BL31_MMU_ENABLE_TLBI 0xd50e871f
#define RK3588_BL31_EXIT_LR 0xf9407bfe
#define RK3588_BL31_EXIT_ERET 0xd69f03e0
#define RK3588_BL31_EXIT_MOVZ 0xd286b160
#define RK3588_SPL_ATF_PARAMS_MOV 0xaa1303e2
#define RK3588_SPL_ATF_ENTRY_MOVZ 0xd286b140
#define RK3588_AARCH64_SMC 0xd4000003
#define RK3588_AARCH64_NOP 0xd503201f
#define RK3588_AARCH64_RET 0xd65f03c0
#define RK3588_FIRMWARE_PATCH_INTERVAL_NS SCALE_US
#define RK3588_FIT_METADATA_MAX_SIZE MiB
#define RK3588_USB2_HOST0_EHCI_BASE 0xfc800000ULL
#define RK3588_USB2_HOST0_OHCI_BASE 0xfc840000ULL
#define RK3588_USB2_HOST1_EHCI_BASE 0xfc880000ULL
#define RK3588_USB2_HOST1_OHCI_BASE 0xfc8c0000ULL

#define FDT_GIC_SPI 0
#define FDT_GIC_PPI 1
#define FDT_IRQ_TYPE_LEVEL_HIGH 4

typedef struct RK3588BootImage {
    uint32_t size_and_off;
    uint32_t address;
    uint32_t flag;
    uint32_t counter;
    uint8_t reserved[8];
    uint8_t hash[64];
} QEMU_PACKED RK3588BootImage;

typedef struct RK3588HeaderV2 {
    uint32_t magic;
    uint8_t reserved[4];
    uint32_t size_and_nimage;
    uint32_t boot_flag;
    uint8_t reserved1[104];
    RK3588BootImage images[4];
    uint8_t reserved2[1064];
    uint8_t hash[512];
} QEMU_PACKED RK3588HeaderV2;

typedef struct RK3588BootROM {
    uint8_t *spl;
    size_t spl_size;
    hwaddr tpl_entry;
    hwaddr atf_load;
    hwaddr uboot_load;
    hwaddr uboot_entry;
    uint32_t atf_size;
    uint32_t uboot_size;
    uint32_t uboot_entry_word;
    uint32_t brom_bootsource;
    bool spl_loaded;
    bool fit_handoff_valid;
} RK3588BootROM;

typedef struct RK3588FITImage {
    hwaddr load;
    hwaddr entry;
    uint64_t media_offset;
    uint32_t size;
} RK3588FITImage;

struct RK3588MachineState {
    MachineState parent_obj;

    struct arm_boot_info bootinfo;
    const RK3588BoardConfig *board;
    ARMCPU *cpu[RK3588_MAX_CPUS];
    DeviceState *gic;
    DeviceState *its[2];
    DeviceState *sdhci;
    DeviceState *sdmmc;     /* dw_mmc - SD card controller */
    DeviceState *sfc;       /* rockchip-sfc - SPI NOR flash controller */
    DeviceState *spi2;      /* rockchip-spi - SPI2 (RK806 PMIC) */
    DeviceState *i2c6;      /* rk3x-i2c - I2C6 (hym8563 RTC) */
    DeviceState *dmac1;     /* rk3588-pl330 - DMAC1 stub */
    DeviceState *scmi;      /* SCMI clock agent (shmem + SMC responder) */
    DeviceState *pcie3x4;
    DeviceState *pcie3x2;
    DeviceState *pcie2x1l0;
    DeviceState *pcie2x1l2;
    DeviceState *gmac0;
    DeviceState *gmac1;
    DeviceState *cru;
    DeviceState *rknn[3];
    DeviceState *rknn_mmu[3];
    DeviceState *rknn_irq_or[3];
    DeviceState *gpio[5];
    DeviceState *crypto;
    DeviceState *secure_otp;
    DeviceState *tsadc;
    DeviceState *atf_ddr;
    RK3588DDRState *ddr;
    RK3588DWC3UDCState *dwc3_udc;
    RK3588USB2HostState *usb2_host;

    MemoryRegion sram;
    MemoryRegion iram;
    MemoryRegion atags;
    MemoryRegion ram_low;
    MemoryRegion ram_high;
    MemoryRegion ramoops;
    MemoryRegion zvm_low_ram;
    MemoryRegion zvm_high_ram;
    MemoryRegion bootrom;
    MemoryRegion firmware_scratch;
    QEMUTimer *firmware_patch_timer;
    bool firmware_boot;
    bool firmware_patch_done;
    bool firmware_handoff_done;
    bool firmware_atf_entered;
    bool zvm_ram;
    bool rknpu;
    bool zephyr_ram;
    bool pcie_links_up;
    bool kernel_started;
    bool kernel_entered;
    int64_t bootargs_patch_start_ns;
    char *firmware_bootargs;
    QEMUTimer *bootargs_timer;
    RK3588BootROM bootrom_state;
};

static const uint64_t rk3588_cpu_mpidr[] = {
    0x000, 0x100, 0x200, 0x300,
    0x400, 0x500, 0x600, 0x700,
};

static const char * const rk3588_cpu_types[] = {
    ARM_CPU_TYPE_NAME("cortex-a55"),
    ARM_CPU_TYPE_NAME("cortex-a55"),
    ARM_CPU_TYPE_NAME("cortex-a55"),
    ARM_CPU_TYPE_NAME("cortex-a55"),
    ARM_CPU_TYPE_NAME("cortex-a76"),
    ARM_CPU_TYPE_NAME("cortex-a76"),
    ARM_CPU_TYPE_NAME("cortex-a76"),
    ARM_CPU_TYPE_NAME("cortex-a76"),
};

G_STATIC_ASSERT(ARRAY_SIZE(rk3588_cpu_mpidr) == RK3588_MAX_CPUS);
G_STATIC_ASSERT(ARRAY_SIZE(rk3588_cpu_types) == RK3588_MAX_CPUS);

static void rk3588_firmware_patch_tick(void *opaque);
static void rk3588_arm_bootargs_patch(RK3588MachineState *s);
static bool rk3588_dynamic_fit_handoff(RK3588MachineState *s);

enum {
    RK3588_SRAM,
    RK3588_ATAGS,
    RK3588_RAM,
    RK3588_FIRMWARE_SCRATCH,
    RK3588_SCMI_SHMEM,
    RK3588_USB3OTG0,
    RK3588_PMU0_GRF,
    RK3588_PMU1_GRF,
    RK3588_USB_GRF,
    RK3588_PMU1_IOC,
    RK3588_PMU2_IOC,
    RK3588_BUS_IOC,
    RK3588_PCIE3X4_APB,
    RK3588_PCIE3X4_CFG,
    RK3588_PCIE3X4_DBI,
    RK3588_PCIE3X2_APB,
    RK3588_PCIE3X2_CFG,
    RK3588_PCIE3X2_DBI,
    RK3588_PCIE2X1L0_APB,
    RK3588_PCIE2X1L0_CFG,
    RK3588_PCIE2X1L0_DBI,
    RK3588_PCIE2X1L2_APB,
    RK3588_PCIE2X1L2_CFG,
    RK3588_PCIE2X1L2_DBI,
    RK3588_GMAC0,
    RK3588_GMAC1,
    RK3588_RKNN0_PC,
    RK3588_RKNN0_CNA,
    RK3588_RKNN0_CORE,
    RK3588_RKNN1_PC,
    RK3588_RKNN1_CNA,
    RK3588_RKNN1_CORE,
    RK3588_RKNN2_PC,
    RK3588_RKNN2_CNA,
    RK3588_RKNN2_CORE,
    RK3588_RKNN0_MMU0,
    RK3588_RKNN0_MMU1,
    RK3588_RKNN1_MMU,
    RK3588_RKNN2_MMU,
    RK3588_SDMMC,
    RK3588_SDHCI,
    RK3588_SFC,
    RK3588_GIC_DIST,
    RK3588_GIC_REDIST,
    RK3588_GIC_ITS0,
    RK3588_GIC_ITS1,
    RK3588_GPIO0,
    RK3588_GPIO1,
    RK3588_GPIO2,
    RK3588_GPIO3,
    RK3588_GPIO4,
    RK3588_SYS_GRF,
    RK3588_PHP_GRF,
    RK3588_CRU_MEM,
    RK3588_STIMER,
    RK3588_FIREWALL_DDR,
    RK3588_FIREWALL_SYSMEM,
    RK3588_CRYPTO,
    RK3588_IRAM,
    RK3588_BROM,
    RK3588_UART0,
    RK3588_UART1,
    RK3588_UART2,
    RK3588_UART3,
    RK3588_UART4,
    RK3588_UART5,
    RK3588_UART6,
    RK3588_UART7,
    RK3588_UART8,
    RK3588_UART9,
    RK3588_TSADC_MMIO,
    RK3588_SARADC,
    RK3588_COMBPHY0,
    RK3588_COMBPHY1,
    RK3588_COMBPHY2,
    RK3588_PCIE30PHY,
    RK3588_PIPE_PHY_GRF0,
    RK3588_PIPE_PHY_GRF1,
    RK3588_PIPE_PHY_GRF2,
    RK3588_SPI2,
    RK3588_I2C6,
    RK3588_DMAC1,
    RK3588_RNG_MMIO,
};

static const MemMapEntry rk3588_memmap[] = {
    [RK3588_SRAM] =         { 0x00000000, RK3588_SRAM_SIZE },
    [RK3588_ATAGS] =        { 0x001fe000, RK3588_ATAGS_SIZE },
    [RK3588_RAM] =          { RK3588_DEFAULT_RAM_BASE, 0 },
    /*
     * BL31 probes optional low-address payload metadata at 0x100000 before
     * normal DRAM starts.  Keep this as zeroed scratch RAM so that probe can
     * fail cleanly without colliding with the SCMI shmem slot below.
     */
    [RK3588_FIRMWARE_SCRATCH] = { 0x00100000, 0x0000f000 },
    /*
     * SCMI shared-memory slot (single Tx/Rx). Lives in low DRAM below
     * the kernel image so the loader does not stomp it. Backed by the
     * rk3588-scmi SysBusDevice (RAM-backed MMIO + the SCMI responder).
     */
    [RK3588_SCMI_SHMEM] =   { 0x0010f000, 0x00000100 },
    [RK3588_USB3OTG0] =     { 0xfc000000,
                              RK3588_DWC3_UDC_MMIO_SIZE },
    [RK3588_PMU0_GRF] =     { 0xfd588000, 0x00001000 },
    [RK3588_PMU1_GRF] =     { 0xfd58a000, 0x00001000 },
    [RK3588_USB_GRF] =      { 0xfd5ac000, 0x00001000 },
    [RK3588_PMU1_IOC] =     { 0xfd5f0000, 0x00001000 },
    [RK3588_PMU2_IOC] =     { 0xfd5f4000, 0x00001000 },
    [RK3588_BUS_IOC] =      { 0xfd5f8000, 0x00001000 },
    [RK3588_PCIE3X4_APB] =  { 0xfe150000, 0x00010000 },
    [RK3588_PCIE3X4_CFG] =  { 0xf0000000, 0x00100000 },
    [RK3588_PCIE3X4_DBI] =  { 0xa40000000ULL, 0x00400000 },
    [RK3588_PCIE3X2_APB] =  { 0xfe160000, 0x00010000 },
    [RK3588_PCIE3X2_CFG] =  { 0xf1000000, 0x00100000 },
    [RK3588_PCIE3X2_DBI] =  { 0xa40400000ULL, 0x00400000 },
    [RK3588_PCIE2X1L0_APB] = { 0xfe170000, 0x00010000 },
    [RK3588_PCIE2X1L0_CFG] = { 0xf2000000, 0x00100000 },
    [RK3588_PCIE2X1L0_DBI] = { 0xa40800000ULL, 0x00400000 },
    [RK3588_PCIE2X1L2_APB] = { 0xfe190000, 0x00010000 },
    [RK3588_PCIE2X1L2_CFG] = { 0xf4000000, 0x00100000 },
    [RK3588_PCIE2X1L2_DBI] = { 0xa41000000ULL, 0x00400000 },
    [RK3588_GMAC0] =        { 0xfe1b0000, 0x00010000 },
    [RK3588_GMAC1] =        { 0xfe1c0000, 0x00010000 },
    [RK3588_RKNN0_PC] =     { 0xfdab0000, ROCKCHIP_RKNN_WINDOW_SIZE },
    [RK3588_RKNN0_CNA] =    { 0xfdab1000, ROCKCHIP_RKNN_WINDOW_SIZE },
    [RK3588_RKNN0_CORE] =   { 0xfdab3000, ROCKCHIP_RKNN_WINDOW_SIZE },
    [RK3588_RKNN1_PC] =     { 0xfdac0000, ROCKCHIP_RKNN_WINDOW_SIZE },
    [RK3588_RKNN1_CNA] =    { 0xfdac1000, ROCKCHIP_RKNN_WINDOW_SIZE },
    [RK3588_RKNN1_CORE] =   { 0xfdac3000, ROCKCHIP_RKNN_WINDOW_SIZE },
    [RK3588_RKNN2_PC] =     { 0xfdad0000, ROCKCHIP_RKNN_WINDOW_SIZE },
    [RK3588_RKNN2_CNA] =    { 0xfdad1000, ROCKCHIP_RKNN_WINDOW_SIZE },
    [RK3588_RKNN2_CORE] =   { 0xfdad3000, ROCKCHIP_RKNN_WINDOW_SIZE },
    [RK3588_RKNN0_MMU0] =   { 0xfdab9000, ROCKCHIP_IOMMU_WINDOW_SIZE },
    [RK3588_RKNN0_MMU1] =   { 0xfdaba000, ROCKCHIP_IOMMU_WINDOW_SIZE },
    [RK3588_RKNN1_MMU] =    { 0xfdaca000, ROCKCHIP_IOMMU_WINDOW_SIZE },
    [RK3588_RKNN2_MMU] =    { 0xfdada000, ROCKCHIP_IOMMU_WINDOW_SIZE },
    [RK3588_SDMMC] =        { 0xfe2c0000, 0x00004000 },
    [RK3588_SDHCI] =        { 0xfe2e0000, 0x00010000 },
    [RK3588_SFC] =          { 0xfe2b0000, ROCKCHIP_SFC_MMIO_SIZE },
    [RK3588_GIC_DIST] =     { 0xfe600000, 0x00010000 },
    [RK3588_GIC_ITS0] =     { 0xfe640000, 0x00020000 },
    [RK3588_GIC_ITS1] =     { 0xfe660000, 0x00020000 },
    [RK3588_GIC_REDIST] =   { 0xfe680000, 0x00100000 },
    [RK3588_GPIO0] =        { 0xfd8a0000, 0x00000100 },
    [RK3588_GPIO1] =        { 0xfec20000, 0x00000100 },
    [RK3588_GPIO2] =        { 0xfec30000, 0x00000100 },
    [RK3588_GPIO3] =        { 0xfec40000, 0x00000100 },
    [RK3588_GPIO4] =        { 0xfec50000, 0x00000100 },
    /*
     * General Register File (GRF) syscons - write-only RGMII-delay / PHY-
     * interface-select registers consumed by dwmac-rk. Dedicated RK3588 GRF
     * and reusable Rockchip syscon devices own their register storage; the
     * gmac FDT node references them through phandles.
     */
    [RK3588_SYS_GRF] =      { 0xfd58c000, 0x00001000 },
    [RK3588_PHP_GRF] =      { 0xfd5b0000, 0x00001000 },
    /*
     * CRU (clock-and-reset-unit) - the reset provider the dw-rockchip
     * PCIe driver mandates. CLK_OF_DECLARE -> clk-rk3588 binds very
     * early; the only load-bearing read is offset 0x600 (PLL lock
     * status) which MUST return 0xffffffff or init hangs before the
     * console. Everything else is RAZ/WI.
     */
    [RK3588_CRU_MEM] =      { 0xfd7c0000, 0x0005c000 },
    [RK3588_STIMER] =       { 0xfd8c8000, ROCKCHIP_STIMER_SIZE },
    [RK3588_FIREWALL_DDR] = { 0xfe030000, 0x00001000 },
    [RK3588_FIREWALL_SYSMEM] = { 0xfe038000, 0x00001000 },
    [RK3588_CRYPTO] =       { 0xfe370000,
                              ROCKCHIP_CRYPTO_V2_MMIO_SIZE },
    [RK3588_IRAM] =         { 0xff000000, RK3588_IRAM_SIZE },
    [RK3588_BROM] =         { RK3588_BROM_TRAMPOLINE, 0x00001000 },
    [RK3588_UART0] =        { 0xfd890000, 0x00000100 },
    [RK3588_UART1] =        { 0xfeb40000, 0x00000100 },
    [RK3588_UART2] =        { 0xfeb50000, 0x00000100 },
    [RK3588_UART3] =        { 0xfeb60000, 0x00000100 },
    [RK3588_UART4] =        { 0xfeb70000, 0x00000100 },
    [RK3588_UART5] =        { 0xfeb80000, 0x00000100 },
    [RK3588_UART6] =        { 0xfeb90000, 0x00000100 },
    [RK3588_UART7] =        { 0xfeba0000, 0x00000100 },
    [RK3588_UART8] =        { 0xfebb0000, 0x00000100 },
    [RK3588_UART9] =        { 0xfebc0000, 0x00000100 },
    [RK3588_TSADC_MMIO] =   { 0xfec00000, RK3588_TSADC_MMIO_SIZE },
    [RK3588_SARADC] =       { 0xfec10000, 0x00010000 },
    /*
     * Serial PHYs: naneng-combphy0/1/2 (PCIe2/USB3/SATA), the
     * snps-pcie3 PHY, and the pipe-phy GRF syscons the drivers regmap.
     * RAM-backed so the vendor phy drivers probe and power on cleanly.
     */
    [RK3588_COMBPHY0] =     { 0xfee00000, 0x00000100 },
    [RK3588_COMBPHY1] =     { 0xfee10000, 0x00000100 },
    [RK3588_COMBPHY2] =     { 0xfee20000, 0x00000100 },
    [RK3588_PCIE30PHY] =    { 0xfee80000, 0x00020000 },
    [RK3588_PIPE_PHY_GRF0] = { 0xfd5bc000, 0x00001000 },
    [RK3588_PIPE_PHY_GRF1] = { 0xfd5c0000, 0x00001000 },
    [RK3588_PIPE_PHY_GRF2] = { 0xfd5c4000, 0x00001000 },
    [RK3588_SPI2] =         { 0xfeb20000, ROCKCHIP_SPI_MMIO_SIZE },
    [RK3588_I2C6] =         { 0xfec80000, 0x00001000 },
    [RK3588_DMAC1] =        { 0xfea10000, RK3588_PL330_MMIO_SIZE },
    [RK3588_RNG_MMIO] =     { 0xfe378000, RK3588_RNG_MMIO_SIZE },
};

static hwaddr rk3588_ram_base(const RK3588MachineState *s)
{
    return s->zephyr_ram ? RK3588_ZEPHYR_RAM_BASE :
                           rk3588_memmap[RK3588_RAM].base;
}

enum {
    RK3588_GIC_MAINT_PPI = 9,
    RK3588_USB3OTG0_SPI = 220,
    RK3588_SDMMC_SPI = 203,
    RK3588_SDHCI_SPI = 205,
    RK3588_SFC_SPI = 206,
    RK3588_RKNN0_SPI = 110,
    RK3588_RKNN1_SPI = 111,
    RK3588_RKNN2_SPI = 112,
    RK3588_GMAC0_SPI = 227,
    RK3588_GMAC1_SPI = 234,
    RK3588_PCIE3X2_ERR_SPI = 254,
    RK3588_PCIE3X2_LEGACY_SPI = 255,
    RK3588_PCIE3X2_MSG_SPI = 256,
    RK3588_PCIE3X2_PMC_SPI = 257,
    RK3588_PCIE3X2_SYS_SPI = 258,
    RK3588_PCIE3X4_ERR_SPI = 259,
    RK3588_PCIE3X4_LEGACY_SPI = 260,
    RK3588_PCIE3X4_MSG_SPI = 261,
    RK3588_PCIE3X4_PMC_SPI = 262,
    RK3588_PCIE3X4_SYS_SPI = 263,
    RK3588_PCIE2X1L0_ERR_SPI = 239,
    RK3588_PCIE2X1L0_LEGACY_SPI = 240,
    RK3588_PCIE2X1L0_MSG_SPI = 241,
    RK3588_PCIE2X1L0_PMC_SPI = 242,
    RK3588_PCIE2X1L0_SYS_SPI = 243,
    RK3588_PCIE2X1L2_ERR_SPI = 249,
    RK3588_PCIE2X1L2_LEGACY_SPI = 250,
    RK3588_PCIE2X1L2_MSG_SPI = 251,
    RK3588_PCIE2X1L2_PMC_SPI = 252,
    RK3588_PCIE2X1L2_SYS_SPI = 253,
    RK3588_GPIO0_SPI = 277,
    RK3588_UART0_SPI = 331,
    RK3588_UART1_SPI = 332,
    RK3588_UART2_SPI = 333,
    RK3588_UART3_SPI = 334,
    RK3588_UART4_SPI = 335,
    RK3588_UART5_SPI = 336,
    RK3588_UART6_SPI = 337,
    RK3588_UART7_SPI = 338,
    RK3588_UART8_SPI = 339,
    RK3588_UART9_SPI = 340,
    RK3588_TSADC_SPI = 397,
    RK3588_SPI2_SPI = 328,
    RK3588_I2C6_SPI = 323,
    RK3588_DMAC1_SPI = 86,
    RK3588_RNG_SPI = 400,
};

/*
 * RK3588 UARTs (rockchip,rk3588-uart / snps,dw-apb-uart).  Every port
 * is modeled, but only the ones with I/O routed out on the ROCK 5B+
 * (40-pin header / debug console: UART1, UART2, UART3, UART4, UART7)
 * get a serial chardev; the rest exist without external output.
 * UART2 is the console: it keeps serial_hd(0) (serial_hd(1) with
 * zephyr-ram) so the existing -serial mon:stdio flow is unchanged;
 * the routed ports map to serial_hd(1..4) in table order.
 */
typedef struct RK3588UARTInfo {
    const char *name;
    int memidx;
    int spi;
    int chr_index;
} RK3588UARTInfo;

static const RK3588UARTInfo rk3588_uarts[] = {
    { "uart0", RK3588_UART0, RK3588_UART0_SPI, -1 },
    { "uart1", RK3588_UART1, RK3588_UART1_SPI, 1 },
    { "uart2", RK3588_UART2, RK3588_UART2_SPI, 0 },
    { "uart3", RK3588_UART3, RK3588_UART3_SPI, 2 },
    { "uart4", RK3588_UART4, RK3588_UART4_SPI, 3 },
    { "uart5", RK3588_UART5, RK3588_UART5_SPI, -1 },
    { "uart6", RK3588_UART6, RK3588_UART6_SPI, -1 },
    { "uart7", RK3588_UART7, RK3588_UART7_SPI, 4 },
    { "uart8", RK3588_UART8, RK3588_UART8_SPI, -1 },
    { "uart9", RK3588_UART9, RK3588_UART9_SPI, -1 },
};

static const char *rk3588_cpu_type(unsigned int n)
{
    return rk3588_cpu_types[n < RK3588_MAX_CPUS ? n : RK3588_MAX_CPUS - 1];
}

static void rk3588_fdt_add_cpu_nodes(RK3588MachineState *s, void *fdt)
{
    MachineState *ms = MACHINE(s);

    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0);

    for (int n = ms->smp.cpus - 1; n >= 0; n--) {
        g_autofree char *nodename = g_strdup_printf("/cpus/cpu@%" PRIx64,
                                                    rk3588_cpu_mpidr[n]);
        ARMCPU *cpu = s->cpu[n];

        qemu_fdt_add_subnode(fdt, nodename);
        qemu_fdt_setprop_string(fdt, nodename, "device_type", "cpu");
        qemu_fdt_setprop_string(fdt, nodename, "compatible",
                                cpu->dtb_compatible);
        qemu_fdt_setprop_cell(fdt, nodename, "reg", rk3588_cpu_mpidr[n]);
        qemu_fdt_setprop_string(fdt, nodename, "enable-method", "psci");
    }
}

static void rk3588_fdt_add_its_node(void *fdt, const char *gic,
                                     unsigned int idx, uint32_t *phandle)
{
    int mem_idx = idx == 0 ? RK3588_GIC_ITS0 : RK3588_GIC_ITS1;
    g_autofree char *node = g_strdup_printf("%s/msi-controller@%" PRIx64,
                                            gic,
                                            rk3588_memmap[mem_idx].base);

    *phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_add_subnode(fdt, node);
    qemu_fdt_setprop_string(fdt, node, "compatible", "arm,gic-v3-its");
    qemu_fdt_setprop_sized_cells(fdt, node, "reg",
                                 2, rk3588_memmap[mem_idx].base,
                                 2, rk3588_memmap[mem_idx].size);
    qemu_fdt_setprop(fdt, node, "msi-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, node, "#msi-cells", 1);
    qemu_fdt_setprop(fdt, node, "dma-noncoherent", NULL, 0);
    qemu_fdt_setprop_cell(fdt, node, "phandle", *phandle);
}

static void rk3588_fdt_add_gic_node(void *fdt, uint32_t *its0_phandle,
                                     uint32_t *its1_phandle)
{
    const char *gic = "/interrupt-controller@fe600000";
    uint32_t phandle;

    qemu_fdt_add_subnode(fdt, gic);
    qemu_fdt_setprop_string(fdt, gic, "compatible", "arm,gic-v3");
    qemu_fdt_setprop_sized_cells(fdt, gic, "reg",
                                 2, rk3588_memmap[RK3588_GIC_DIST].base,
                                 2, rk3588_memmap[RK3588_GIC_DIST].size,
                                 2, rk3588_memmap[RK3588_GIC_REDIST].base,
                                 2, rk3588_memmap[RK3588_GIC_REDIST].size);
    qemu_fdt_setprop_cells(fdt, gic, "interrupts",
                           FDT_GIC_PPI, RK3588_GIC_MAINT_PPI,
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0);
    qemu_fdt_setprop(fdt, gic, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, gic, "#interrupt-cells", 4);
    qemu_fdt_setprop_cell(fdt, gic, "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, gic, "#size-cells", 2);
    qemu_fdt_setprop(fdt, gic, "ranges", NULL, 0);
    qemu_fdt_setprop(fdt, gic, "dma-noncoherent", NULL, 0);

    phandle = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_setprop_cell(fdt, gic, "phandle", phandle);
    qemu_fdt_setprop_cell(fdt, "/", "interrupt-parent", phandle);

    rk3588_fdt_add_its_node(fdt, gic, 0, its0_phandle);
    rk3588_fdt_add_its_node(fdt, gic, 1, its1_phandle);
}

static void rk3588_fdt_add_timer_node(void *fdt)
{
    const char *timer = "/timer";

    qemu_fdt_add_subnode(fdt, timer);
    qemu_fdt_setprop_string(fdt, timer, "compatible", "arm,armv8-timer");
    qemu_fdt_setprop_cells(fdt, timer, "interrupts",
                           FDT_GIC_PPI, INTID_TO_PPI(ARCH_TIMER_S_EL1_IRQ),
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_PPI, INTID_TO_PPI(ARCH_TIMER_NS_EL1_IRQ),
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_PPI, INTID_TO_PPI(ARCH_TIMER_VIRT_IRQ),
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_PPI, INTID_TO_PPI(ARCH_TIMER_NS_EL2_IRQ),
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_PPI, INTID_TO_PPI(ARCH_TIMER_NS_EL2_VIRT_IRQ),
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0);
    qemu_fdt_setprop_cell(fdt, timer, "clock-frequency", RK3588_GTIMER_HZ);
    qemu_fdt_setprop(fdt, timer, "always-on", NULL, 0);
}

static void rk3588_fdt_add_uart_nodes(void *fdt)
{
    static const char * const compat[] = {
        "rockchip,rk3588-uart",
        "snps,dw-apb-uart",
        "ns16550a",
    };

    qemu_fdt_add_subnode(fdt, "/aliases");
    qemu_fdt_add_subnode(fdt, "/chosen");

    for (unsigned int i = 0; i < ARRAY_SIZE(rk3588_uarts); i++) {
        const RK3588UARTInfo *u = &rk3588_uarts[i];
        g_autofree char *alias = g_strdup_printf("serial%u", i);
        g_autofree char *node = g_strdup_printf("/serial@%" PRIx64,
                                                rk3588_memmap[u->memidx].base);

        qemu_fdt_setprop_string(fdt, "/aliases", alias, node);

        qemu_fdt_add_subnode(fdt, node);
        qemu_fdt_setprop_string_array(fdt, node, "compatible",
                                      (char **)&compat, ARRAY_SIZE(compat));
        qemu_fdt_setprop_sized_cells(fdt, node, "reg",
                                     2, rk3588_memmap[u->memidx].base,
                                     2, rk3588_memmap[u->memidx].size);
        qemu_fdt_setprop_cells(fdt, node, "interrupts",
                               FDT_GIC_SPI, u->spi,
                               FDT_IRQ_TYPE_LEVEL_HIGH, 0);
        qemu_fdt_setprop_cell(fdt, node, "clock-frequency", RK3588_GTIMER_HZ);
        qemu_fdt_setprop_cell(fdt, node, "current-speed", RK3588_UART_BAUDBASE);
        qemu_fdt_setprop_cell(fdt, node, "reg-shift", 2);
        qemu_fdt_setprop_cell(fdt, node, "reg-io-width", 4);
        qemu_fdt_setprop_string(fdt, node, "status", "okay");
    }

    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", "serial2:1500000n8");
}

static uint32_t rk3588_fdt_add_fixed_clock_node(void *fdt)
{
    const char *clk = "/xin24m";
    uint32_t phandle = qemu_fdt_alloc_phandle(fdt);

    qemu_fdt_add_subnode(fdt, clk);
    qemu_fdt_setprop_string(fdt, clk, "compatible", "fixed-clock");
    qemu_fdt_setprop_cell(fdt, clk, "#clock-cells", 0);
    qemu_fdt_setprop_cell(fdt, clk, "clock-frequency", RK3588_GTIMER_HZ);
    qemu_fdt_setprop_string(fdt, clk, "clock-output-names", "xin24m");
    qemu_fdt_setprop_cell(fdt, clk, "phandle", phandle);

    return phandle;
}

/*
 * SCMI firmware node + shmem reserved-memory node. Returns the
 * phandle of the scmi_clk protocol sub-node (used by the sdmmc FDT
 * node to reference its biu/ciu clocks).
 */
static uint32_t rk3588_fdt_add_scmi_nodes(void *fdt)
{
    const char *firmware = "/firmware";
    const char *scmi = "/firmware/scmi";
    const char *scmi_clk = "/firmware/scmi/protocol@14";
    const char *scmi_reset = "/firmware/scmi/protocol@16";
    const char *shmem = "/reserved-memory/scmi_shmem@10f000";
    uint32_t shmem_ph, scmi_ph, scmi_clk_ph;

    /*
     * reserved-memory/scmi_shmem - the 256-byte shmem slot the SCMI
     * SMC transport reads/writes during a synchronous command. The
     * `ranges;` property is required by the binding so the kernel's
     * reserved-memory framework can map the shmem phandle to a
     * physical address.
     */
    qemu_fdt_add_subnode(fdt, "/reserved-memory");
    qemu_fdt_setprop_cell(fdt, "/reserved-memory", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/reserved-memory", "#size-cells", 2);
    qemu_fdt_setprop(fdt, "/reserved-memory", "ranges", NULL, 0);

    shmem_ph = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_add_subnode(fdt, shmem);
    qemu_fdt_setprop_string(fdt, shmem, "compatible", "arm,scmi-shmem");
    qemu_fdt_setprop_sized_cells(fdt, shmem, "reg",
                                 2, rk3588_memmap[RK3588_SCMI_SHMEM].base,
                                 2, rk3588_memmap[RK3588_SCMI_SHMEM].size);
    qemu_fdt_setprop(fdt, shmem, "no-map", NULL, 0);
    qemu_fdt_setprop_cell(fdt, shmem, "phandle", shmem_ph);

    /*
     * /firmware/scmi - SMC transport, single shmem, smc-id 0x82000010.
     * QEMU's SMC hook (target/arm/tcg/psci.c) intercepts that function-id
     * and serves the shmem via the rk3588-scmi device.
     */
    qemu_fdt_add_subnode(fdt, firmware);
    qemu_fdt_add_subnode(fdt, scmi);
    qemu_fdt_setprop_string(fdt, scmi, "compatible", "arm,scmi-smc");
    qemu_fdt_setprop_cell(fdt, scmi, "arm,smc-id", RK3588_SCMI_SMC_ID);
    qemu_fdt_setprop_cell(fdt, scmi, "shmem", shmem_ph);
    qemu_fdt_setprop_cell(fdt, scmi, "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, scmi, "#size-cells", 0);

    scmi_ph = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_setprop_cell(fdt, scmi, "phandle", scmi_ph);

    /* CLOCK protocol (0x14). */
    scmi_clk_ph = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_add_subnode(fdt, scmi_clk);
    qemu_fdt_setprop_cell(fdt, scmi_clk, "reg", 0x14);
    qemu_fdt_setprop_cell(fdt, scmi_clk, "#clock-cells", 1);
    qemu_fdt_setprop_cell(fdt, scmi_clk, "phandle", scmi_clk_ph);

    /* RESET protocol (0x16) - referenced by trng only; advertise for FDT fidelity. */
    qemu_fdt_add_subnode(fdt, scmi_reset);
    qemu_fdt_setprop_cell(fdt, scmi_reset, "reg", 0x16);
    qemu_fdt_setprop_cell(fdt, scmi_reset, "#reset-cells", 1);

    return scmi_clk_ph;
}

static void rk3588_fdt_add_storage_nodes(void *fdt, uint32_t clk_phandle,
                                          uint32_t scmi_clk_phandle)
{
    const char *sdhci = "/mmc@fe2e0000";
    const char *sdmmc = "/mmc@fe2c0000";
    static const char * const sdhci_compat[] = {
        "rockchip,rk3588-dwcmshc",
        "snps,dwcmshc-sdhci",
    };
    static const char * const sdmmc_compat[] = {
        "rockchip,rk3588-dw-mshc",
        "rockchip,rk3288-dw-mshc",
    };
    static const char * const sdhci_clock_names[] = {
        "core", "bus", "axi", "block", "timer",
    };
    /*
     * Per rk3588-base.dtsi:2200-2213: biu=SCMI_HCLK_SD(23),
     * ciu=SCMI_CCLK_SD(9); the ciu-drive / ciu-sample phase clocks
     * are CRU-side (SCLK_SDMMC_DRV / _SAMPLE) and the model serves
     * them as fire-and-forget via the cru stub - keep them on xin24m
     * so clk_get succeeds.
     */
    static const char * const sdmmc_clock_names[] = {
        "biu", "ciu", "ciu-drive", "ciu-sample",
    };

    qemu_fdt_add_subnode(fdt, sdhci);
    qemu_fdt_setprop_string_array(fdt, sdhci, "compatible",
                                  (char **)&sdhci_compat,
                                  ARRAY_SIZE(sdhci_compat));
    qemu_fdt_setprop_sized_cells(fdt, sdhci, "reg",
                                 2, rk3588_memmap[RK3588_SDHCI].base,
                                 2, rk3588_memmap[RK3588_SDHCI].size);
    qemu_fdt_setprop_cells(fdt, sdhci, "interrupts",
                           FDT_GIC_SPI, RK3588_SDHCI_SPI,
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0);
    qemu_fdt_setprop_cells(fdt, sdhci, "clocks",
                           clk_phandle, clk_phandle, clk_phandle,
                           clk_phandle, clk_phandle);
    qemu_fdt_setprop_string_array(fdt, sdhci, "clock-names",
                                  (char **)&sdhci_clock_names,
                                  ARRAY_SIZE(sdhci_clock_names));
    qemu_fdt_setprop_cell(fdt, sdhci, "bus-width", 8);
    qemu_fdt_setprop(fdt, sdhci, "non-removable", NULL, 0);
    qemu_fdt_setprop(fdt, sdhci, "no-sdio", NULL, 0);
    qemu_fdt_setprop(fdt, sdhci, "no-sd", NULL, 0);
    qemu_fdt_setprop_cell(fdt, sdhci, "max-frequency", 200000000);
    qemu_fdt_setprop_string(fdt, sdhci, "status", "okay");

    qemu_fdt_setprop_string(fdt, "/aliases", "mmc0", sdhci);

    /* dw_mmc SD-card controller. */
    qemu_fdt_add_subnode(fdt, sdmmc);
    qemu_fdt_setprop_string_array(fdt, sdmmc, "compatible",
                                  (char **)&sdmmc_compat,
                                  ARRAY_SIZE(sdmmc_compat));
    qemu_fdt_setprop_sized_cells(fdt, sdmmc, "reg",
                                 2, rk3588_memmap[RK3588_SDMMC].base,
                                 2, rk3588_memmap[RK3588_SDMMC].size);
    qemu_fdt_setprop_cells(fdt, sdmmc, "interrupts",
                           FDT_GIC_SPI, RK3588_SDMMC_SPI,
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0);
    qemu_fdt_setprop_cells(fdt, sdmmc, "clocks",
                           scmi_clk_phandle, 23,  /* SCMI_HCLK_SD */
                           scmi_clk_phandle, 9,   /* SCMI_CCLK_SD */
                           clk_phandle, 0,        /* SCLK_SDMMC_DRV (CRU stub) */
                           clk_phandle, 0);       /* SCLK_SDMMC_SAMPLE */
    qemu_fdt_setprop_string_array(fdt, sdmmc, "clock-names",
                                  (char **)&sdmmc_clock_names,
                                  ARRAY_SIZE(sdmmc_clock_names));
    qemu_fdt_setprop_cell(fdt, sdmmc, "bus-width", 4);
    qemu_fdt_setprop_cell(fdt, sdmmc, "cap-sd-highspeed", 1);
    qemu_fdt_setprop_cell(fdt, sdmmc, "cap-mmc-highspeed", 1);
    qemu_fdt_setprop_cell(fdt, sdmmc, "max-frequency", 200000000);
    /*
     * fifo-depth = 0x100 per rk3588-base.dtsi:2206. The driver uses
     * this to size its PIO loop and to derive FIFOTH when no DTS
     * value is present.
     */
    qemu_fdt_setprop_cell(fdt, sdmmc, "fifo-depth", 0x100);
    qemu_fdt_setprop_string(fdt, sdmmc, "status", "okay");

    qemu_fdt_setprop_string(fdt, "/aliases", "mmc1", sdmmc);
}

static void rk3588_fdt_add_gpio_nodes(void *fdt, uint32_t clk_phandle)
{
    const char *pinctrl = "/pinctrl";
    uint32_t pinctrl_phandle = qemu_fdt_alloc_phandle(fdt);

    qemu_fdt_add_subnode(fdt, pinctrl);
    qemu_fdt_setprop_string(fdt, pinctrl, "compatible",
                            "rockchip,rk3588-pinctrl");
    qemu_fdt_setprop(fdt, pinctrl, "ranges", NULL, 0);
    qemu_fdt_setprop_cell(fdt, pinctrl, "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, pinctrl, "#size-cells", 2);
    qemu_fdt_setprop_cell(fdt, pinctrl, "phandle", pinctrl_phandle);

    for (unsigned int i = 0; i < ARRAY_SIZE(((RK3588MachineState *)0)->gpio);
         i++) {
        int idx = RK3588_GPIO0 + i;
        g_autofree char *node = g_strdup_printf("%s/gpio%u@%" PRIx64,
                                                pinctrl, i,
                                                rk3588_memmap[idx].base);
        g_autofree char *alias = g_strdup_printf("gpio%u", i);

        qemu_fdt_add_subnode(fdt, node);
        qemu_fdt_setprop_string(fdt, node, "compatible",
                                "rockchip,gpio-bank");
        qemu_fdt_setprop_sized_cells(fdt, node, "reg",
                                     2, rk3588_memmap[idx].base,
                                     2, rk3588_memmap[idx].size);
        qemu_fdt_setprop_cells(fdt, node, "interrupts",
                               FDT_GIC_SPI, RK3588_GPIO0_SPI + i,
                               FDT_IRQ_TYPE_LEVEL_HIGH, 0);
        qemu_fdt_setprop_cells(fdt, node, "clocks", clk_phandle,
                               clk_phandle);
        qemu_fdt_setprop(fdt, node, "gpio-controller", NULL, 0);
        qemu_fdt_setprop_cells(fdt, node, "gpio-ranges",
                               pinctrl_phandle, 0, i * ROCKCHIP_GPIO_PINS,
                               ROCKCHIP_GPIO_PINS);
        qemu_fdt_setprop(fdt, node, "interrupt-controller", NULL, 0);
        qemu_fdt_setprop_cell(fdt, node, "#gpio-cells", 2);
        qemu_fdt_setprop_cell(fdt, node, "#interrupt-cells", 2);

        qemu_fdt_setprop_string(fdt, "/aliases", alias, node);
    }
}

static void rk3588_fdt_add_grf_nodes(void *fdt, uint32_t *sys_grf_ph,
                                       uint32_t *php_grf_ph)
{
    const char *sys_grf = "/syscon@fd58c000";
    const char *php_grf = "/syscon@fd5b0000";
    static const char * const sys_grf_compat[] = {
        "rockchip,rk3588-sys-grf", "syscon", "simple-mfd",
    };
    static const char * const php_grf_compat[] = {
        "rockchip,rk3588-php-grf", "syscon",
    };

    *sys_grf_ph = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_add_subnode(fdt, sys_grf);
    qemu_fdt_setprop_string_array(fdt, sys_grf, "compatible",
                                  (char **)&sys_grf_compat,
                                  ARRAY_SIZE(sys_grf_compat));
    qemu_fdt_setprop_sized_cells(fdt, sys_grf, "reg",
                                 2, rk3588_memmap[RK3588_SYS_GRF].base,
                                 2, rk3588_memmap[RK3588_SYS_GRF].size);
    qemu_fdt_setprop_cell(fdt, sys_grf, "phandle", *sys_grf_ph);

    *php_grf_ph = qemu_fdt_alloc_phandle(fdt);
    qemu_fdt_add_subnode(fdt, php_grf);
    qemu_fdt_setprop_string_array(fdt, php_grf, "compatible",
                                  (char **)&php_grf_compat,
                                  ARRAY_SIZE(php_grf_compat));
    qemu_fdt_setprop_sized_cells(fdt, php_grf, "reg",
                                 2, rk3588_memmap[RK3588_PHP_GRF].base,
                                 2, rk3588_memmap[RK3588_PHP_GRF].size);
    qemu_fdt_setprop_cell(fdt, php_grf, "phandle", *php_grf_ph);
}

/*
 * CRU node - the clock-and-reset provider. CLK_OF_DECLARE makes
 * clk-rk3588 bind very early; the model backs offset 0x600 with
 * 0xffffffff (all PLLs locked) so the early PLL-lock-status poll
 * terminates and the rest of init can proceed.
 */
static uint32_t rk3588_fdt_add_cru_node(void *fdt)
{
    const char *cru = "/clock-reset-controller@fd7c0000";
    uint32_t phandle = qemu_fdt_alloc_phandle(fdt);

    qemu_fdt_add_subnode(fdt, cru);
    qemu_fdt_setprop_string(fdt, cru, "compatible", "rockchip,rk3588-cru");
    qemu_fdt_setprop_sized_cells(fdt, cru, "reg",
                                 2, rk3588_memmap[RK3588_CRU_MEM].base,
                                 2, rk3588_memmap[RK3588_CRU_MEM].size);
    qemu_fdt_setprop_cell(fdt, cru, "#clock-cells", 1);
    qemu_fdt_setprop_cell(fdt, cru, "#reset-cells", 1);
    qemu_fdt_setprop_string(fdt, cru, "clock-output-names", "rk3588-cru");
    qemu_fdt_setprop_cell(fdt, cru, "phandle", phandle);

    return phandle;
}

static void rk3588_fdt_add_gmac_node(void *fdt, unsigned int id,
                                      uint32_t clk_phandle,
                                      uint32_t sys_grf_ph, uint32_t php_grf_ph)
{
    const char *gmac = id ? "/ethernet@fe1c0000" : "/ethernet@fe1b0000";
    int map = id ? RK3588_GMAC1 : RK3588_GMAC0;
    int spi = id ? RK3588_GMAC1_SPI : RK3588_GMAC0_SPI;
    static const char * const compat[] = {
        "rockchip,rk3588-gmac",
        "snps,dwmac-4.20a",
    };
    static const char * const clock_names[] = {
        "stmmaceth", "clk_mac_ref", "pclk_mac", "aclk_mac", "ptp_ref",
    };

    qemu_fdt_add_subnode(fdt, gmac);
    qemu_fdt_setprop_string_array(fdt, gmac, "compatible",
                                  (char **)&compat, ARRAY_SIZE(compat));
    qemu_fdt_setprop_sized_cells(fdt, gmac, "reg",
                                 2, rk3588_memmap[map].base,
                                 2, rk3588_memmap[map].size);
    qemu_fdt_setprop_cells(fdt, gmac, "interrupts",
                           FDT_GIC_SPI, spi, FDT_IRQ_TYPE_LEVEL_HIGH, 0);
    qemu_fdt_setprop_string(fdt, gmac, "interrupt-names", "macirq");
    qemu_fdt_setprop_cells(fdt, gmac, "clocks",
                           clk_phandle, clk_phandle, clk_phandle,
                           clk_phandle, clk_phandle);
    qemu_fdt_setprop_string_array(fdt, gmac, "clock-names",
                                  (char **)&clock_names,
                                  ARRAY_SIZE(clock_names));
    /*
     * dwmac-rk.c REQUIRES rockchip,grf (probe returns -ENODEV without it)
     * and rk3588_ops also wants rockchip,php-grf for CON0/CLK_CON1. Both
     * are modelled as RAZ/WI syscons - see rk3588_create_grf_devices().
     */
    qemu_fdt_setprop_cell(fdt, gmac, "rockchip,grf", sys_grf_ph);
    qemu_fdt_setprop_cell(fdt, gmac, "rockchip,php-grf", php_grf_ph);
    qemu_fdt_setprop_string(fdt, gmac, "phy-mode",
                            id ? "rgmii-id" : "rgmii-rxid");
    qemu_fdt_setprop_string(fdt, gmac, "status", "okay");
}

static void rk3588_fdt_add_gmac_nodes(RK3588MachineState *s, void *fdt,
                                      uint32_t clk_phandle,
                                      uint32_t sys_grf_ph, uint32_t php_grf_ph)
{
    const RK3588BoardConfig *board = s->board;

    if (board->gmac_mask & BIT(0)) {
        rk3588_fdt_add_gmac_node(fdt, 0, clk_phandle,
                                 sys_grf_ph, php_grf_ph);
    }
    if (board->gmac_mask & BIT(1)) {
        rk3588_fdt_add_gmac_node(fdt, 1, clk_phandle,
                                 sys_grf_ph, php_grf_ph);
    }

    if (board->gmac_mask == (BIT(0) | BIT(1))) {
        qemu_fdt_setprop_string(fdt, "/aliases", "ethernet0",
                                board->swap_gmac_aliases ?
                                "/ethernet@fe1c0000" :
                                "/ethernet@fe1b0000");
        qemu_fdt_setprop_string(fdt, "/aliases", "ethernet1",
                                board->swap_gmac_aliases ?
                                "/ethernet@fe1b0000" :
                                "/ethernet@fe1c0000");
    } else if (board->gmac_mask & BIT(0)) {
        qemu_fdt_setprop_string(fdt, "/aliases", "ethernet0",
                                "/ethernet@fe1b0000");
    } else if (board->gmac_mask & BIT(1)) {
        qemu_fdt_setprop_string(fdt, "/aliases", "ethernet0",
                                "/ethernet@fe1c0000");
    }
}

typedef struct RK3588PCIEFDTConfig {
    const char *node;
    unsigned int dbi_map;
    unsigned int apb_map;
    unsigned int cfg_map;
    uint32_t sys_spi;
    uint32_t pmc_spi;
    uint32_t msg_spi;
    uint32_t legacy_spi;
    uint32_t err_spi;
    uint32_t power_up_reset;
    uint32_t pipe_reset;
    uint32_t domain;
    uint32_t bus_start;
    uint32_t requester_id;
    uint32_t prefetch_hi;
    uint32_t prefetch_lo;
} RK3588PCIEFDTConfig;

enum {
    RK3588_SRST_PCIE0_POWER_UP = 294,
    RK3588_SRST_P_PCIE0 = 299,
    RK3588_SRST_PCIE1_POWER_UP = 526,
    RK3588_SRST_P_PCIE1 = 541,
};

static const RK3588PCIEFDTConfig rk3588_pcie3x4_fdt = {
    .node = "/pcie@fe150000",
    .dbi_map = RK3588_PCIE3X4_DBI,
    .apb_map = RK3588_PCIE3X4_APB,
    .cfg_map = RK3588_PCIE3X4_CFG,
    .sys_spi = RK3588_PCIE3X4_SYS_SPI,
    .pmc_spi = RK3588_PCIE3X4_PMC_SPI,
    .msg_spi = RK3588_PCIE3X4_MSG_SPI,
    .legacy_spi = RK3588_PCIE3X4_LEGACY_SPI,
    .err_spi = RK3588_PCIE3X4_ERR_SPI,
    .power_up_reset = RK3588_SRST_PCIE0_POWER_UP,
    .pipe_reset = RK3588_SRST_P_PCIE0,
    .domain = 0,
    .bus_start = 0,
    .requester_id = 0,
    .prefetch_hi = 0x9,
    .prefetch_lo = 0,
};

static const RK3588PCIEFDTConfig rk3588_pcie3x2_fdt = {
    .node = "/pcie@fe160000",
    .dbi_map = RK3588_PCIE3X2_DBI,
    .apb_map = RK3588_PCIE3X2_APB,
    .cfg_map = RK3588_PCIE3X2_CFG,
    .sys_spi = RK3588_PCIE3X2_SYS_SPI,
    .pmc_spi = RK3588_PCIE3X2_PMC_SPI,
    .msg_spi = RK3588_PCIE3X2_MSG_SPI,
    .legacy_spi = RK3588_PCIE3X2_LEGACY_SPI,
    .err_spi = RK3588_PCIE3X2_ERR_SPI,
    .power_up_reset = RK3588_SRST_PCIE1_POWER_UP,
    .pipe_reset = RK3588_SRST_P_PCIE1,
    .domain = 1,
    .bus_start = 0x10,
    .requester_id = 0x1000,
    .prefetch_hi = 0x9,
    .prefetch_lo = 0x40000000,
};

/*
 * PCIe 2.0 x1 controllers.  ROCK 5B+ wires pcie2x1l0 to the RTL8125BG
 * 2.5G NIC and pcie2x1l2 to the M.2 E-Key WiFi 6 module.  The reset IDs
 * follow the vendor CRU header (SRST_PCIE2/4_POWER_UP, SRST_P_PCIE2/4).
 */
static const RK3588PCIEFDTConfig rk3588_pcie2x1l0_fdt = {
    .node = "/pcie@fe170000",
    .dbi_map = RK3588_PCIE2X1L0_DBI,
    .apb_map = RK3588_PCIE2X1L0_APB,
    .cfg_map = RK3588_PCIE2X1L0_CFG,
    .sys_spi = RK3588_PCIE2X1L0_SYS_SPI,
    .pmc_spi = RK3588_PCIE2X1L0_PMC_SPI,
    .msg_spi = RK3588_PCIE2X1L0_MSG_SPI,
    .legacy_spi = RK3588_PCIE2X1L0_LEGACY_SPI,
    .err_spi = RK3588_PCIE2X1L0_ERR_SPI,
    .power_up_reset = 0x20f,
    .pipe_reset = 0x21e,
    .domain = 2,
    .bus_start = 0x20,
    .requester_id = 0x2000,
    .prefetch_hi = 0x9,
    .prefetch_lo = 0x80000000,
};

static const RK3588PCIEFDTConfig rk3588_pcie2x1l2_fdt = {
    .node = "/pcie@fe190000",
    .dbi_map = RK3588_PCIE2X1L2_DBI,
    .apb_map = RK3588_PCIE2X1L2_APB,
    .cfg_map = RK3588_PCIE2X1L2_CFG,
    .sys_spi = RK3588_PCIE2X1L2_SYS_SPI,
    .pmc_spi = RK3588_PCIE2X1L2_PMC_SPI,
    .msg_spi = RK3588_PCIE2X1L2_MSG_SPI,
    .legacy_spi = RK3588_PCIE2X1L2_LEGACY_SPI,
    .err_spi = RK3588_PCIE2X1L2_ERR_SPI,
    .power_up_reset = 0x211,
    .pipe_reset = 0x220,
    .domain = 4,
    .bus_start = 0x40,
    .requester_id = 0x4000,
    .prefetch_hi = 0x9,
    .prefetch_lo = 0xc0000000,
};

static void rk3588_fdt_add_pcie_node(void *fdt,
                                      const RK3588PCIEFDTConfig *config,
                                      unsigned int num_lanes,
                                      unsigned int max_link_speed,
                                      uint32_t cru_phandle,
                                      uint32_t clk_phandle,
                                      uint32_t its_phandle)
{
    const char *pcie = config->node;
    uint32_t io_base = rk3588_memmap[config->cfg_map].base +
                       rk3588_memmap[config->cfg_map].size;
    uint32_t mem_base = io_base + 0x00100000;
    static const char * const compat[] = {
        "rockchip,rk3588-pcie",
        "rockchip,rk3568-pcie",
    };
    static const char * const reg_names[] = {
        "dbi", "apb", "config",
    };
    static const char * const clock_names[] = {
        "aclk_mst", "aclk_slv", "aclk_dbi", "pclk", "aux", "pipe",
    };
    static const char * const reset_names[] = {
        "pwr", "pipe",
    };
    qemu_fdt_add_subnode(fdt, pcie);
    qemu_fdt_setprop_string_array(fdt, pcie, "compatible",
                                  (char **)&compat, ARRAY_SIZE(compat));
    qemu_fdt_setprop_string(fdt, pcie, "device_type", "pci");
    qemu_fdt_setprop_sized_cells(fdt, pcie, "reg",
                                 2, rk3588_memmap[config->dbi_map].base,
                                 2, rk3588_memmap[config->dbi_map].size,
                                 2, rk3588_memmap[config->apb_map].base,
                                 2, rk3588_memmap[config->apb_map].size,
                                 2, rk3588_memmap[config->cfg_map].base,
                                 2, rk3588_memmap[config->cfg_map].size);
    qemu_fdt_setprop_string_array(fdt, pcie, "reg-names",
                                  (char **)&reg_names,
                                  ARRAY_SIZE(reg_names));
    qemu_fdt_setprop_cells(fdt, pcie, "interrupts",
                           FDT_GIC_SPI, config->sys_spi,
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_SPI, config->pmc_spi,
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_SPI, config->msg_spi,
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_SPI, config->legacy_spi,
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_SPI, config->err_spi,
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0);
    static const char * const irq_names[] = {
        "sys", "pmc", "msg", "legacy", "err",
    };
    qemu_fdt_setprop_string_array(fdt, pcie, "interrupt-names",
                                  (char **)&irq_names,
                                  ARRAY_SIZE(irq_names));
    /*
     * Clocks stay on xin24m (the always-on 24 MHz oscillator). The CRU
     * stub accepts the clk_prepare_enable gate writes fire-and-forget
     * but is NOT a real clock provider (no rate), so referencing cru
     * here makes clk_bulk_get fail with -EINVAL. xin24m is a real
     * fixed-clock and lets the driver's clk_prepare_enable calls land.
     */
    qemu_fdt_setprop_cells(fdt, pcie, "clocks",
                           clk_phandle, clk_phandle, clk_phandle,
                           clk_phandle, clk_phandle, clk_phandle);
    qemu_fdt_setprop_string_array(fdt, pcie, "clock-names",
                                  (char **)&clock_names,
                                  ARRAY_SIZE(clock_names));
    /* Both reset IDs are defined by rockchip,rk3588-cru.h. */
    qemu_fdt_setprop_cells(fdt, pcie, "resets",
                           cru_phandle, config->power_up_reset,
                           cru_phandle, config->pipe_reset);
    qemu_fdt_setprop_string_array(fdt, pcie, "reset-names",
                                  (char **)&reset_names,
                                  ARRAY_SIZE(reset_names));
    qemu_fdt_setprop_cell(fdt, pcie, "#address-cells", 3);
    qemu_fdt_setprop_cell(fdt, pcie, "#size-cells", 2);
    qemu_fdt_setprop_cell(fdt, pcie, "#interrupt-cells", 1);
    qemu_fdt_setprop_cells(fdt, pcie, "bus-range", config->bus_start,
                           config->bus_start + 0x0f);
    qemu_fdt_setprop_cell(fdt, pcie, "num-lanes", num_lanes);
    qemu_fdt_setprop_cell(fdt, pcie, "max-link-speed", max_link_speed);
    /*
     * Each host owns a disjoint 0x1000 Requester ID range routed to an
     * ITS (pcie3x hosts use ITS1, pcie2x1 hosts use ITS0, matching the
     * vendor device trees).  PCIe MSI writes then target the ITS
     * GITS_TRANSLATER doorbell directly; the host bridge line IRQs
     * above remain separate.
     */
    qemu_fdt_setprop_cells(fdt, pcie, "msi-map",
                           config->requester_id, its_phandle,
                           config->requester_id, 0x1000);
    /*
     * Bus ranges - IO/MEM/prefetch. The 1 MiB CFG window is the reg
     * "config" entry above; the designware model serves it via the outbound
     * CFG viewport once the guest programs the iATU.
     */
    qemu_fdt_setprop_cells(fdt, pcie, "ranges",
                           0x01000000, 0x0, io_base,
                                         0x0, io_base, 0x0, 0x00100000,
                           0x02000000, 0x0, mem_base,
                                         0x0, mem_base, 0x0, 0x00e00000,
                           0x03000000, config->prefetch_hi,
                                         config->prefetch_lo,
                                         config->prefetch_hi,
                                         config->prefetch_lo,
                                         0x0, 0x40000000);
    /* Refer to xin24m so the cru-of-declare path doesn't grab us. */
    qemu_fdt_setprop_cell(fdt, pcie, "linux,pci-domain", config->domain);
    qemu_fdt_setprop_string(fdt, pcie, "status", "okay");
}

static void rk3588_fdt_add_pcie_nodes(RK3588MachineState *s, void *fdt,
                                       uint32_t cru_phandle,
                                       uint32_t clk_phandle,
                                       uint32_t its0_phandle,
                                       uint32_t its1_phandle)
{
    rk3588_fdt_add_pcie_node(fdt, &rk3588_pcie3x4_fdt,
                             s->board->pcie3x4_num_lanes, 3,
                             cru_phandle, clk_phandle, its1_phandle);

    if (s->board->pcie3x2_num_lanes) {
        rk3588_fdt_add_pcie_node(fdt, &rk3588_pcie3x2_fdt,
                                 s->board->pcie3x2_num_lanes, 3,
                                 cru_phandle, clk_phandle, its1_phandle);
    }

    if (s->board->pcie2x1_mask & BIT(0)) {
        rk3588_fdt_add_pcie_node(fdt, &rk3588_pcie2x1l0_fdt, 1, 2,
                                 cru_phandle, clk_phandle, its0_phandle);
    }
    if (s->board->pcie2x1_mask & BIT(2)) {
        rk3588_fdt_add_pcie_node(fdt, &rk3588_pcie2x1l2_fdt, 1, 2,
                                 cru_phandle, clk_phandle, its0_phandle);
    }
}

static void rk3588_fdt_add_rknpu_core_nodes(void *fdt, uint32_t cru_phandle,
                                            uint32_t clk_phandle)
{
    static const int pc_memmap[] = {
        RK3588_RKNN0_PC,
        RK3588_RKNN1_PC,
        RK3588_RKNN2_PC,
    };
    static const int cna_memmap[] = {
        RK3588_RKNN0_CNA,
        RK3588_RKNN1_CNA,
        RK3588_RKNN2_CNA,
    };
    static const int core_memmap[] = {
        RK3588_RKNN0_CORE,
        RK3588_RKNN1_CORE,
        RK3588_RKNN2_CORE,
    };
    static const int iommu_memmap0[] = {
        RK3588_RKNN0_MMU0,
        RK3588_RKNN1_MMU,
        RK3588_RKNN2_MMU,
    };
    static const int iommu_memmap1[] = {
        RK3588_RKNN0_MMU1,
        -1,
        -1,
    };
    static const int irq[] = {
        RK3588_RKNN0_SPI,
        RK3588_RKNN1_SPI,
        RK3588_RKNN2_SPI,
    };
    enum {
        SRST_A_RKNN1 = 250,
        SRST_H_RKNN1 = 252,
        SRST_A_RKNN2 = 254,
        SRST_H_RKNN2 = 256,
        SRST_A_RKNN0 = 272,
        SRST_H_RKNN0 = 274,
    };
    static const int reset_a[] = {
        SRST_A_RKNN0,
        SRST_A_RKNN1,
        SRST_A_RKNN2,
    };
    static const int reset_h[] = {
        SRST_H_RKNN0,
        SRST_H_RKNN1,
        SRST_H_RKNN2,
    };
    static const char * const reg_names[] = {
        "pc", "cna", "core",
    };
    static const char * const clock_names[] = {
        "aclk", "hclk", "npu", "pclk",
    };
    static const char * const reset_names[] = {
        "srst_a", "srst_h",
    };
    static const char * const iommu_compat[] = {
        "rockchip,rk3588-iommu", "rockchip,rk3568-iommu",
    };
    static const char * const iommu_clock_names[] = {
        "aclk", "iface",
    };
    uint32_t iommu_phandle[3];

    for (unsigned int i = 0; i < ARRAY_SIZE(pc_memmap); i++) {
        uint64_t iommu_base = rk3588_memmap[iommu_memmap0[i]].base;
        g_autofree char *iommu = g_strdup_printf("/iommu@%" PRIx64,
                                                 iommu_base);

        iommu_phandle[i] = qemu_fdt_alloc_phandle(fdt);
        qemu_fdt_add_subnode(fdt, iommu);
        qemu_fdt_setprop_string_array(fdt, iommu, "compatible",
                                      (char **)&iommu_compat,
                                      ARRAY_SIZE(iommu_compat));
        if (iommu_memmap1[i] >= 0) {
            qemu_fdt_setprop_sized_cells(fdt, iommu, "reg",
                                         2, rk3588_memmap[iommu_memmap0[i]].base,
                                         2, rk3588_memmap[iommu_memmap0[i]].size,
                                         2, rk3588_memmap[iommu_memmap1[i]].base,
                                         2, rk3588_memmap[iommu_memmap1[i]].size);
        } else {
            qemu_fdt_setprop_sized_cells(fdt, iommu, "reg",
                                         2, rk3588_memmap[iommu_memmap0[i]].base,
                                         2, rk3588_memmap[iommu_memmap0[i]].size);
        }
        qemu_fdt_setprop_cells(fdt, iommu, "interrupts",
                               FDT_GIC_SPI, irq[i],
                               FDT_IRQ_TYPE_LEVEL_HIGH, 0);
        qemu_fdt_setprop_cells(fdt, iommu, "clocks",
                               clk_phandle, clk_phandle);
        qemu_fdt_setprop_string_array(fdt, iommu, "clock-names",
                                      (char **)&iommu_clock_names,
                                      ARRAY_SIZE(iommu_clock_names));
        qemu_fdt_setprop_cell(fdt, iommu, "#iommu-cells", 0);
        qemu_fdt_setprop_cell(fdt, iommu, "phandle", iommu_phandle[i]);
        qemu_fdt_setprop_string(fdt, iommu, "status", "okay");
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(pc_memmap); i++) {
        uint64_t base = rk3588_memmap[pc_memmap[i]].base;
        g_autofree char *node = g_strdup_printf("/npu@%" PRIx64,
                                                base);

        qemu_fdt_add_subnode(fdt, node);
        qemu_fdt_setprop_string(fdt, node, "compatible",
                                "rockchip,rk3588-rknn-core");
        qemu_fdt_setprop_sized_cells(fdt, node, "reg",
                                     2, rk3588_memmap[pc_memmap[i]].base,
                                     2, rk3588_memmap[pc_memmap[i]].size,
                                     2, rk3588_memmap[cna_memmap[i]].base,
                                     2, rk3588_memmap[cna_memmap[i]].size,
                                     2, rk3588_memmap[core_memmap[i]].base,
                                     2, rk3588_memmap[core_memmap[i]].size);
        qemu_fdt_setprop_string_array(fdt, node, "reg-names",
                                      (char **)&reg_names,
                                      ARRAY_SIZE(reg_names));
        qemu_fdt_setprop_cells(fdt, node, "interrupts",
                               FDT_GIC_SPI, irq[i],
                               FDT_IRQ_TYPE_LEVEL_HIGH, 0);
        qemu_fdt_setprop_cells(fdt, node, "clocks",
                               clk_phandle, clk_phandle,
                               clk_phandle, clk_phandle);
        qemu_fdt_setprop_string_array(fdt, node, "clock-names",
                                      (char **)&clock_names,
                                      ARRAY_SIZE(clock_names));
        qemu_fdt_setprop_cells(fdt, node, "resets",
                               cru_phandle, reset_a[i],
                               cru_phandle, reset_h[i]);
        qemu_fdt_setprop_string_array(fdt, node, "reset-names",
                                      (char **)&reset_names,
                                      ARRAY_SIZE(reset_names));
        qemu_fdt_setprop_cell(fdt, node, "iommus", iommu_phandle[i]);
        qemu_fdt_setprop_string(fdt, node, "status", "okay");
    }
}

static void rk3588_fdt_add_rknpu_vendor_node(void *fdt,
                                             uint32_t cru_phandle,
                                             uint32_t clk_phandle)
{
    static const int core_memmap[] = {
        RK3588_RKNN0_PC,
        RK3588_RKNN1_PC,
        RK3588_RKNN2_PC,
    };
    static const int iommu_memmap[] = {
        RK3588_RKNN0_MMU0,
        RK3588_RKNN0_MMU1,
        RK3588_RKNN1_MMU,
        RK3588_RKNN2_MMU,
    };
    static const int irq[] = {
        RK3588_RKNN0_SPI,
        RK3588_RKNN1_SPI,
        RK3588_RKNN2_SPI,
    };
    static const int reset[] = {
        486, 432, 448,
        488, 434, 450,
    };
    static const char * const irq_names[] = {
        "npu0_irq", "npu1_irq", "npu2_irq",
    };
    static const char * const clock_names[] = {
        "clk_npu", "aclk0", "aclk1", "aclk2",
        "hclk0", "hclk1", "hclk2", "pclk",
    };
    static const char * const reset_names[] = {
        "srst_a0", "srst_a1", "srst_a2",
        "srst_h0", "srst_h1", "srst_h2",
    };
    static const char * const iommu_irq_names[] = {
        "npu0_mmu", "npu1_mmu", "npu2_mmu",
    };
    static const char * const iommu_clock_names[] = {
        "aclk0", "aclk1", "aclk2", "iface0", "iface1", "iface2",
    };
    const char *iommu = "/iommu@fdab9000";
    const char *npu = "/npu@fdab0000";
    uint32_t iommu_phandle = qemu_fdt_alloc_phandle(fdt);

    qemu_fdt_add_subnode(fdt, iommu);
    qemu_fdt_setprop_string(fdt, iommu, "compatible", "rockchip,iommu-v2");
    qemu_fdt_setprop_sized_cells(
        fdt, iommu, "reg",
        2, rk3588_memmap[iommu_memmap[0]].base,
        2, rk3588_memmap[iommu_memmap[0]].size,
        2, rk3588_memmap[iommu_memmap[1]].base,
        2, rk3588_memmap[iommu_memmap[1]].size,
        2, rk3588_memmap[iommu_memmap[2]].base,
        2, rk3588_memmap[iommu_memmap[2]].size,
        2, rk3588_memmap[iommu_memmap[3]].base,
        2, rk3588_memmap[iommu_memmap[3]].size);
    qemu_fdt_setprop_cells(fdt, iommu, "interrupts",
                           FDT_GIC_SPI, irq[0], FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_SPI, irq[1], FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_SPI, irq[2], FDT_IRQ_TYPE_LEVEL_HIGH, 0);
    qemu_fdt_setprop_string_array(fdt, iommu, "interrupt-names",
                                  (char **)&iommu_irq_names,
                                  ARRAY_SIZE(iommu_irq_names));
    qemu_fdt_setprop_cells(fdt, iommu, "clocks",
                           clk_phandle, clk_phandle, clk_phandle,
                           clk_phandle, clk_phandle, clk_phandle);
    qemu_fdt_setprop_string_array(fdt, iommu, "clock-names",
                                  (char **)&iommu_clock_names,
                                  ARRAY_SIZE(iommu_clock_names));
    qemu_fdt_setprop_cell(fdt, iommu, "#iommu-cells", 0);
    qemu_fdt_setprop_cell(fdt, iommu, "phandle", iommu_phandle);
    qemu_fdt_setprop_string(fdt, iommu, "status", "okay");

    qemu_fdt_add_subnode(fdt, npu);
    qemu_fdt_setprop_string(fdt, npu, "compatible",
                            "rockchip,rk3588-rknpu");
    qemu_fdt_setprop_sized_cells(fdt, npu, "reg",
                                 2, rk3588_memmap[core_memmap[0]].base,
                                 2, 0x10000,
                                 2, rk3588_memmap[core_memmap[1]].base,
                                 2, 0x10000,
                                 2, rk3588_memmap[core_memmap[2]].base,
                                 2, 0x10000);
    qemu_fdt_setprop_cells(fdt, npu, "interrupts",
                           FDT_GIC_SPI, irq[0], FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_SPI, irq[1], FDT_IRQ_TYPE_LEVEL_HIGH, 0,
                           FDT_GIC_SPI, irq[2], FDT_IRQ_TYPE_LEVEL_HIGH, 0);
    qemu_fdt_setprop_string_array(fdt, npu, "interrupt-names",
                                  (char **)&irq_names,
                                  ARRAY_SIZE(irq_names));
    qemu_fdt_setprop_cells(fdt, npu, "clocks",
                           clk_phandle, clk_phandle, clk_phandle, clk_phandle,
                           clk_phandle, clk_phandle, clk_phandle, clk_phandle);
    qemu_fdt_setprop_string_array(fdt, npu, "clock-names",
                                  (char **)&clock_names,
                                  ARRAY_SIZE(clock_names));
    qemu_fdt_setprop_cells(fdt, npu, "resets",
                           cru_phandle, reset[0], cru_phandle, reset[1],
                           cru_phandle, reset[2], cru_phandle, reset[3],
                           cru_phandle, reset[4], cru_phandle, reset[5]);
    qemu_fdt_setprop_string_array(fdt, npu, "reset-names",
                                  (char **)&reset_names,
                                  ARRAY_SIZE(reset_names));
    qemu_fdt_setprop_cell(fdt, npu, "iommus", iommu_phandle);
    qemu_fdt_setprop_string(fdt, npu, "status", "okay");
}

static void rk3588_fdt_add_sfc_node(void *fdt, uint32_t clk_phandle)
{
    const char *sfc = "/spi@fe2b0000";
    const char *flash = "/spi@fe2b0000/spi-flash@0";
    static const char * const clock_names[] = {
        "clk_sfc", "hclk_sfc",
    };

    qemu_fdt_add_subnode(fdt, sfc);
    qemu_fdt_setprop_string(fdt, sfc, "compatible", "rockchip,sfc");
    qemu_fdt_setprop_sized_cells(fdt, sfc, "reg",
                                 2, rk3588_memmap[RK3588_SFC].base,
                                 2, rk3588_memmap[RK3588_SFC].size);
    qemu_fdt_setprop_cells(fdt, sfc, "interrupts",
                           FDT_GIC_SPI, RK3588_SFC_SPI,
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0);
    qemu_fdt_setprop_cells(fdt, sfc, "clocks",
                           clk_phandle, clk_phandle);
    qemu_fdt_setprop_string_array(fdt, sfc, "clock-names",
                                  (char **)&clock_names,
                                  ARRAY_SIZE(clock_names));
    qemu_fdt_setprop_cell(fdt, sfc, "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, sfc, "#size-cells", 0);
    qemu_fdt_setprop_string(fdt, sfc, "status", "okay");

    qemu_fdt_add_subnode(fdt, flash);
    qemu_fdt_setprop_string(fdt, flash, "compatible", "jedec,spi-nor");
    qemu_fdt_setprop_cell(fdt, flash, "reg", 0);
    qemu_fdt_setprop_cell(fdt, flash, "spi-max-frequency", 104000000);
    qemu_fdt_setprop_cell(fdt, flash, "spi-tx-bus-width", 1);
    qemu_fdt_setprop_cell(fdt, flash, "spi-rx-bus-width", 4);
    qemu_fdt_setprop_string(fdt, flash, "status", "okay");
}

static void rk3588_fdt_add_i2c6_node(void *fdt, uint32_t clk_phandle)
{
    const char *i2c6 = "/i2c@fec80000";
    const char *rtc = "/i2c@fec80000/rtc@51";
    static const char * const clock_names[] = {
        "i2c", "pclk",
    };

    qemu_fdt_add_subnode(fdt, i2c6);
    qemu_fdt_setprop_string(fdt, i2c6, "compatible",
                            "rockchip,rk3588-i2c");
    qemu_fdt_setprop_sized_cells(fdt, i2c6, "reg",
                                 2, rk3588_memmap[RK3588_I2C6].base,
                                 2, rk3588_memmap[RK3588_I2C6].size);
    qemu_fdt_setprop_cells(fdt, i2c6, "interrupts",
                           FDT_GIC_SPI, RK3588_I2C6_SPI,
                           FDT_IRQ_TYPE_LEVEL_HIGH, 0);
    qemu_fdt_setprop_cells(fdt, i2c6, "clocks",
                           clk_phandle, clk_phandle);
    qemu_fdt_setprop_string_array(fdt, i2c6, "clock-names",
                                  (char **)&clock_names,
                                  ARRAY_SIZE(clock_names));
    qemu_fdt_setprop_cell(fdt, i2c6, "#address-cells", 1);
    qemu_fdt_setprop_cell(fdt, i2c6, "#size-cells", 0);
    qemu_fdt_setprop_string(fdt, i2c6, "status", "okay");

    qemu_fdt_add_subnode(fdt, rtc);
    qemu_fdt_setprop_string(fdt, rtc, "compatible", "haoyu,hym8563");
    qemu_fdt_setprop_cell(fdt, rtc, "reg", 0x51);
    qemu_fdt_setprop_string(fdt, rtc, "status", "okay");
}

static void *rk3588_get_dtb(const struct arm_boot_info *binfo, int *fdt_size)
{
    RK3588MachineState *s = container_of(binfo, RK3588MachineState, bootinfo);
    const RK3588BoardConfig *board = s->board;
    void *fdt = create_device_tree(fdt_size);
    uint32_t clk_phandle;
    uint32_t its0_phandle, its1_phandle;

    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(EXIT_FAILURE);
    }

    qemu_fdt_setprop_string(fdt, "/", "model", board->fdt_model);
    qemu_fdt_setprop_string_array(fdt, "/", "compatible",
                                  (char **)board->fdt_compatible,
                                  board->fdt_compatible_count);
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 2);
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 2);

    clk_phandle = rk3588_fdt_add_fixed_clock_node(fdt);

    uint32_t sys_grf_ph, php_grf_ph;
    rk3588_fdt_add_grf_nodes(fdt, &sys_grf_ph, &php_grf_ph);
    uint32_t cru_phandle = rk3588_fdt_add_cru_node(fdt);
    uint32_t scmi_clk_phandle = rk3588_fdt_add_scmi_nodes(fdt);

    rk3588_fdt_add_cpu_nodes(s, fdt);
    rk3588_fdt_add_gic_node(fdt, &its0_phandle, &its1_phandle);
    rk3588_fdt_add_timer_node(fdt);
    rk3588_fdt_add_uart_nodes(fdt);
    rk3588_fdt_add_storage_nodes(fdt, clk_phandle, scmi_clk_phandle);
    rk3588_fdt_add_sfc_node(fdt, clk_phandle);
    rk3588_fdt_add_i2c6_node(fdt, clk_phandle);
    rk3588_fdt_add_gpio_nodes(fdt, clk_phandle);
    rk3588_fdt_add_gmac_nodes(s, fdt, clk_phandle, sys_grf_ph, php_grf_ph);
    rk3588_fdt_add_pcie_nodes(s, fdt, cru_phandle, clk_phandle,
                              its0_phandle, its1_phandle);
    if (s->rknpu) {
        if (s->board->rknpu_fdt_topology == RK3588_RKNPU_FDT_AGGREGATE) {
            rk3588_fdt_add_rknpu_vendor_node(fdt, cru_phandle, clk_phandle);
        } else {
            rk3588_fdt_add_rknpu_core_nodes(fdt, cru_phandle, clk_phandle);
        }
    }

    return fdt;
}

static void rk3588_create_cpus(RK3588MachineState *s)
{
    MachineState *ms = MACHINE(s);
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    MemoryRegion *sysmem = get_system_memory();
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    for (unsigned int n = 0; n < ms->smp.cpus; n++) {
        g_autofree char *name = g_strdup_printf("cpu%u", n);
        Object *cpuobj = object_new(possible_cpus->cpus[n].type);
        CPUState *cs = CPU(cpuobj);

        object_property_add_child(OBJECT(ms), name, cpuobj);
        cs->cpu_index = n;
        numa_cpu_pre_plug(&possible_cpus->cpus[n], DEVICE(cpuobj),
                          &error_fatal);
        object_property_set_int(cpuobj, "mp-affinity",
                                possible_cpus->cpus[n].arch_id,
                                &error_abort);
        object_property_set_int(cpuobj, "cntfrq", RK3588_GTIMER_HZ,
                                &error_abort);
        object_property_set_link(cpuobj, "memory", OBJECT(sysmem),
                                 &error_abort);

        if (object_property_find(cpuobj, "reset-cbar")) {
            object_property_set_int(cpuobj, "reset-cbar",
                                    rk3588_memmap[RK3588_GIC_DIST].base,
                                    &error_abort);
        }
        if (object_property_find(cpuobj, "has_el2")) {
            object_property_set_bool(cpuobj, "has_el2", !kvm_enabled(),
                                     &error_abort);
        }
        if (object_property_find(cpuobj, "has_el3")) {
            object_property_set_bool(cpuobj, "has_el3", !kvm_enabled(),
                                     &error_abort);
        }
        qdev_realize(DEVICE(cpuobj), NULL, &error_fatal);
        s->cpu[n] = ARM_CPU(cpuobj);
    }
}

static void rk3588_enable_psci_conduit(RK3588MachineState *s)
{
    MachineState *ms = MACHINE(s);

    for (unsigned int n = 0; n < ms->smp.cpus; n++) {
        object_property_set_int(OBJECT(s->cpu[n]), "psci-conduit",
                                QEMU_PSCI_CONDUIT_SMC, &error_abort);
        if (n > 0) {
            object_property_set_bool(OBJECT(s->cpu[n]), "start-powered-off",
                                     true, &error_abort);
        }
    }
}

static void rk3588_create_syscon(RK3588MachineState *s, const char *name,
                                 int memidx)
{
    DeviceState *dev = qdev_new(TYPE_ROCKCHIP_SYSCON);
    SysBusDevice *sbd;

    qdev_prop_set_uint32(dev, "size", rk3588_memmap[memidx].size);
    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[memidx].base);
    object_unref(OBJECT(dev));
}

static void rk3588_write_atags(RK3588MachineState *s)
{
    const RK3588FirmwareProfile *profile = s->board->firmware_profile;
    MachineState *ms = MACHINE(s);
    uint8_t *base = memory_region_get_ram_ptr(&s->atags);
    uint8_t *ddr_tag = base + 8 + 12;
    uint32_t core_size_words = (8 + 12) / sizeof(uint32_t);
    uint32_t ddr_size_words = (8 + 184) / sizeof(uint32_t);
    hwaddr ram_base = rk3588_ram_base(s);
    hwaddr low_ram_size = RK3588_LOW_RAM_END - ram_base;
    uint32_t bank_count = ms->ram_size > low_ram_size ? 2 : 1;
    uint64_t low_size = ram_base + MIN(ms->ram_size, low_ram_size);
    uint64_t high_size = ms->ram_size > low_ram_size ?
                         ms->ram_size - low_ram_size : 0;

    memset(base, 0, RK3588_ATAGS_SIZE);

    /*
     * DDR ATAG banks: the low window runs from 0 to the end of the
     * first 4 GiB window (the base address is folded into the size, as
     * the firmware expects); RAM above it is described as a second bank
     * at 4 GiB.  The firmware (TPL) hands these banks to U-Boot, which
     * adds one bank per entry.
     */
    if (!profile || !profile->atags_core) {
        stl_le_p(base, ddr_size_words);
        stl_le_p(base + 4, 0x54410052);   /* ATAG_DDR_MEM */
        stl_le_p(base + 8, bank_count);
        stl_le_p(base + 12, 0);           /* tag version */
        stq_le_p(base + 16, 0);           /* bank[0] start */
        stq_le_p(base + 24, low_size);     /* bank[0] size */
        if (bank_count == 2) {
            stq_le_p(base + 32, RK3588_HIGH_RAM_BASE);
            stq_le_p(base + 40, high_size);
        }
        stl_le_p(base + ddr_size_words * sizeof(uint32_t), 0);
        return;
    }

    stl_le_p(base, core_size_words);
    stl_le_p(base + 4, 0x54410001);       /* ATAG_CORE */

    stl_le_p(ddr_tag, ddr_size_words);
    stl_le_p(ddr_tag + 4, 0x54410052);     /* ATAG_DDR_MEM */
    stl_le_p(ddr_tag + 8, bank_count);
    stl_le_p(ddr_tag + 12, 0);             /* tag version */
    stq_le_p(ddr_tag + 16, 0);             /* bank[0] start */
    stq_le_p(ddr_tag + 24, low_size);       /* bank[0] size */
    if (bank_count == 2) {
        stq_le_p(ddr_tag + 32, RK3588_HIGH_RAM_BASE);
        stq_le_p(ddr_tag + 40, high_size);
    }
    stl_le_p(ddr_tag + ddr_size_words * sizeof(uint32_t), 0);
}

static void rk3588_seed_iram_firmware_shims(RK3588MachineState *s)
{
    uint8_t *iram = memory_region_get_ram_ptr(&s->iram);
    hwaddr offset = RK3588_PMUSRAM_SKIP_ADDR -
                    rk3588_memmap[RK3588_IRAM].base;

    /*
     * Rockchip BL31 calls into PMUSRAM code that would normally be loaded by
     * earlier firmware stages.  The downstream model does not execute the PMU
     * firmware, so return from this low-power helper and keep the EL3 bring-up
     * path moving when the SRAM slot is otherwise empty.
     */
    stl_le_p(iram + offset, RK3588_AARCH64_RET);
}

static bool rk3588_phys_read32(hwaddr addr, uint32_t *value)
{
    uint32_t data;

    if (address_space_read(&address_space_memory, addr, MEMTXATTRS_UNSPECIFIED,
                           &data, sizeof(data)) != MEMTX_OK) {
        return false;
    }

    *value = le32_to_cpu(data);
    return true;
}

static bool rk3588_phys_write32(hwaddr addr, uint32_t value)
{
    uint32_t data = cpu_to_le32(value);

    return address_space_write(&address_space_memory, addr,
                               MEMTXATTRS_UNSPECIFIED,
                               &data, sizeof(data)) == MEMTX_OK;
}

static bool rk3588_patch_bl31_runtime(RK3588MachineState *s)
{
    uint32_t copy_call;
    uint32_t run_call;
    uint32_t rmr_branch;
    uint32_t mmu_enable;
    uint32_t mmu_body;
    uint32_t exit_lr;
    uint32_t exit_eret;

    if (s->firmware_patch_done) {
        return true;
    }

    if (!rk3588_phys_read32(RK3588_BL31_PMUSRAM_COPY_CALL_ADDR, &copy_call) ||
        !rk3588_phys_read32(RK3588_BL31_PMUSRAM_RUN_CALL_ADDR, &run_call) ||
        !rk3588_phys_read32(RK3588_BL31_RMR_CBZ_ADDR, &rmr_branch) ||
        !rk3588_phys_read32(RK3588_BL31_MMU_ENABLE_ADDR, &mmu_enable) ||
        !rk3588_phys_read32(RK3588_BL31_MMU_ENABLE_BODY_ADDR, &mmu_body) ||
        !rk3588_phys_read32(RK3588_BL31_EXIT_LR_ADDR, &exit_lr) ||
        !rk3588_phys_read32(RK3588_BL31_EXIT_ERET_ADDR, &exit_eret)) {
        return false;
    }

    if (copy_call == RK3588_BL31_PMUSRAM_COPY_CALL &&
        run_call == RK3588_BL31_PMUSRAM_RUN_CALL &&
        rmr_branch == RK3588_BL31_RMR_CBZ &&
        mmu_enable == RK3588_BL31_MMU_ENABLE_BRANCH &&
        mmu_body == RK3588_BL31_MMU_ENABLE_TLBI &&
        exit_lr == RK3588_BL31_EXIT_LR &&
        exit_eret == RK3588_BL31_EXIT_ERET) {
        rk3588_phys_write32(RK3588_BL31_PMUSRAM_COPY_CALL_ADDR,
                            RK3588_AARCH64_NOP);
        rk3588_phys_write32(RK3588_BL31_PMUSRAM_RUN_CALL_ADDR,
                            RK3588_AARCH64_NOP);
        rk3588_phys_write32(RK3588_BL31_RMR_CBZ_ADDR,
                            RK3588_BL31_RMR_SKIP);
        rk3588_phys_write32(RK3588_BL31_MMU_ENABLE_ADDR,
                            RK3588_AARCH64_RET);
        rk3588_phys_write32(RK3588_BL31_MMU_ENABLE_BODY_ADDR,
                            RK3588_AARCH64_RET);
        rk3588_phys_write32(RK3588_BL31_EXIT_LR_ADDR,
                            RK3588_BL31_EXIT_MOVZ);
        rk3588_phys_write32(RK3588_BL31_EXIT_ERET_ADDR,
                            RK3588_AARCH64_SMC);
        s->firmware_patch_done = true;
        return true;
    }

    if (copy_call == RK3588_AARCH64_NOP &&
        run_call == RK3588_AARCH64_NOP &&
        rmr_branch == RK3588_BL31_RMR_SKIP &&
        mmu_enable == RK3588_AARCH64_RET &&
        mmu_body == RK3588_AARCH64_RET &&
        exit_lr == RK3588_BL31_EXIT_MOVZ &&
        exit_eret == RK3588_AARCH64_SMC) {
        s->firmware_patch_done = true;
        return true;
    }

    return false;
}

static void rk3588_patch_spl_atf_handoff(void)
{
    /*
     * SPL is not covered by the FIT hashes it verifies.  Replace the final
     * mov x0, x19; mov x1, #0; blr x20 in spl_invoke_atf() with a
     * QEMU-private SMC after preserving the BL31 params pointer in x2.
     * The handler patches BL31 after SPL has validated and loaded the FIT
     * images, then resumes at the real BL31 entry point.
     */
    rk3588_phys_write32(RK3588_SPL_ATF_CALL_ADDR,
                        RK3588_SPL_ATF_PARAMS_MOV);
    rk3588_phys_write32(RK3588_SPL_ATF_CALL_ADDR + sizeof(uint32_t),
                        RK3588_SPL_ATF_ENTRY_MOVZ);
    rk3588_phys_write32(RK3588_SPL_ATF_CALL_ADDR + 2 * sizeof(uint32_t),
                        RK3588_AARCH64_SMC);
}

static void rk3588_prepare_nonsecure_linux_interrupts(RK3588MachineState *s)
{
    ARMLinuxBootIf *albif = ARM_LINUX_BOOT_IF(s->gic);
    ARMLinuxBootIfClass *albifc = ARM_LINUX_BOOT_IF_GET_CLASS(albif);

    /*
     * The firmware shim bypasses arm_load_kernel(), so run the same GIC
     * Linux-init hook that direct kernel boot uses.  This models secure
     * firmware handing IRQ ownership to the NonSecure kernel before U-Boot
     * eventually jumps to Linux.
     */
    if (albifc->arm_linux_init) {
        albifc->arm_linux_init(albif, false);
    }
    device_cold_reset(s->gic);

    for (unsigned int i = 0; i < ARRAY_SIZE(s->its); i++) {
        if (s->its[i]) {
            device_cold_reset(s->its[i]);
        }
    }
}

static void rk3588_schedule_firmware_patch(RK3588MachineState *s)
{
    if (!s->firmware_boot || s->firmware_handoff_done ||
        !s->firmware_patch_timer) {
        return;
    }

    timer_mod(s->firmware_patch_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              RK3588_FIRMWARE_PATCH_INTERVAL_NS);
}

static void rk3588_set_uboot_cpu_state(RK3588MachineState *s, ARMCPU *cpu)
{
    CPUState *cs = CPU(cpu);
    CPUARMState *env = &cpu->env;
    hwaddr entry = RK3588_UBOOT_LOAD_ADDR;

    if (rk3588_dynamic_fit_handoff(s) &&
        s->bootrom_state.fit_handoff_valid) {
        entry = s->bootrom_state.uboot_entry;
        tb_invalidate_phys_range(cs, s->bootrom_state.uboot_load,
                                 s->bootrom_state.uboot_load +
                                 s->bootrom_state.uboot_size - 1);
    }

    cpu_reset(cs);
    arm_emulate_firmware_reset(cs, 2);
    cpu_set_pc(cs, entry);
    env->xregs[0] = 0;
    env->xregs[1] = 0;
    env->xregs[2] = 0;
    env->xregs[3] = 0;
    cs->halted = false;
    arm_rebuild_hflags(env);
}

static void rk3588_firmware_handoff_to_uboot(RK3588MachineState *s,
                                             ARMCPU *cpu)
{
    rk3588_prepare_nonsecure_linux_interrupts(s);
    rk3588_set_uboot_cpu_state(s, cpu);
    s->firmware_handoff_done = true;
    rk3588_usb2_host_set_active(s->usb2_host, true);
    rk3588_arm_bootargs_patch(s);
}

static void rk3588_firmware_handoff_work(CPUState *cs,
                                         run_on_cpu_data data)
{
    RK3588MachineState *s = data.host_ptr;

    rk3588_set_uboot_cpu_state(s, ARM_CPU(cs));
}

static void rk3588_schedule_firmware_handoff(RK3588MachineState *s,
                                              ARMCPU *cpu)
{
    rk3588_prepare_nonsecure_linux_interrupts(s);
    s->firmware_handoff_done = true;
    rk3588_usb2_host_set_active(s->usb2_host, true);
    rk3588_arm_bootargs_patch(s);
    async_run_on_cpu(CPU(cpu), rk3588_firmware_handoff_work,
                     RUN_ON_CPU_HOST_PTR(s));
}

static void rk3588_arm_bootargs_patch(RK3588MachineState *s)
{
    if (s->bootargs_timer && s->firmware_bootargs &&
        s->firmware_bootargs[0]) {
        s->bootargs_patch_start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_mod(s->bootargs_timer,
                  s->bootargs_patch_start_ns +
                  RK3588_FIRMWARE_BOOTARGS_INTERVAL_NS);
    }
}

/*
 * Patch /chosen/bootargs in the kernel DTB that U-Boot loaded into RAM.
 * Runs against a host-side copy with libfdt slack so the property can
 * grow, then writes the repacked tree back in place (the kernel only
 * ever reads the DTB through the address U-Boot passed in x0).
 *
 * The patch is idempotent: once the requested parameters are present in
 * /chosen/bootargs the tree is left alone, so repeated polling never
 * grows the command line.  booti() rewrites bootargs from its own
 * environment right before entering the kernel, so the poller keeps
 * re-applying the patch until the kernel entry PC is seen (see
 * rk3588_firmware_bootargs_tick()).
 */
/*
 * Patch /chosen/bootargs in place.  The new value is built to fit the
 * existing property slot (the trailing Rockchip "androidboot.fwver"
 * marker is dropped to make room), so the DTB layout never changes:
 * U-Boot's libfdt fixups and the kernel's unflatten keep their cached
 * offsets valid, and a plain 400-byte store cannot race either of them.
 * Idempotent: once the requested parameters are present the tree is
 * left alone.
 */
/*
 * True when the command line already contains the requested text as a
 * whole argument (bounded by whitespace), so a substring inside another
 * value cannot mark the patch as done.
 */
static bool rk3588_bootargs_has_arg(const char *cmdline, const char *arg)
{
    size_t len = strlen(arg);
    const char *p = cmdline;

    while ((p = strstr(p, arg))) {
        if ((p == cmdline || p[-1] == ' ') &&
            (p[len] == '\0' || p[len] == ' ')) {
            return true;
        }
        p += len;
    }
    return false;
}

static void rk3588_patch_firmware_bootargs(RK3588MachineState *s)
{
    hwaddr dtb_addr = RK3588_UBOOT_DTB_ADDR;
    uint32_t magic, totalsize;
    g_autofree uint8_t *src = NULL;
    const char *extra = s->firmware_bootargs;
    static const char fwver_tail[] =
        " androidboot.fwver=uboot-17.09-64-3-07/24/202626";
    int chosen = -1;

    if (!extra || !extra[0]) {
        return;
    }

    /*
     * The DTB is stored big-endian in memory (FDT format), so the raw
     * 32-bit reads need a byte swap before comparing with the magic.
     */
    if (!rk3588_phys_read32(dtb_addr, &magic)) {
        return;
    }
    magic = bswap32(magic);
    if (magic != 0xd00dfeed) {
        return;
    }
    if (!rk3588_phys_read32(dtb_addr + 4, &totalsize)) {
        return;
    }
    totalsize = bswap32(totalsize);
    if (totalsize < 8 || totalsize > 0x100000) {
        return;
    }

    src = g_malloc(0x100000);
    address_space_read(&address_space_memory, dtb_addr,
                       MEMTXATTRS_UNSPECIFIED, src, totalsize);

    chosen = fdt_path_offset(src, "/chosen");
    if (chosen < 0) {
        return;
    }

    {
        int plen;
        const char *old = fdt_getprop(src, chosen, "bootargs", &plen);
        g_autofree char *newargs = NULL;
        size_t oldlen, newlen;

        if (!old || plen <= 0) {
            return;
        }
        /*
         * The property may not carry a terminator within plen; only use
         * C-string operations once a NUL is verified inside the copy.
         */
        if (!memchr(old, '\0', plen)) {
            return;
        }
        if (rk3588_bootargs_has_arg(old, extra)) {
            return; /* already patched */
        }
        oldlen = strlen(old);
        if (oldlen > sizeof(fwver_tail) - 1 &&
            strcmp(old + oldlen - (sizeof(fwver_tail) - 1),
                   fwver_tail) == 0) {
            /*
             * Drop the trailing Rockchip "androidboot.fwver" marker to
             * make room for the extra parameters inside the existing
             * property slot (the value must not grow).  Only shorten
             * when the value really ends with that marker; a length-only
             * test would discard arbitrary kernel parameters.
             */
            newargs = g_strdup_printf("%.*s %s",
                                      (int)(oldlen - (sizeof(fwver_tail) - 1)),
                                      old, extra);
        } else {
            /*
             * No marker: append if it fits; otherwise trim whole
             * trailing tokens (never mid-token) until the patched value
             * fits, preserving the leading root=/console= parameters.
             */
            size_t keep = oldlen;
            size_t need = strlen(extra) + 2; /* space + value + NUL */

            while (keep + need > (size_t)plen && keep > 0) {
                while (keep > 0 && old[keep - 1] == ' ') {
                    keep--;
                }
                while (keep > 0 && old[keep - 1] != ' ') {
                    keep--;
                }
            }
            newargs = g_strdup_printf("%.*s %s", (int)keep, old, extra);
        }
        newlen = strlen(newargs) + 1;
        if (newlen > (size_t)plen) {
            return; /* does not fit the existing slot in place */
        }

        {
            g_autofree uint8_t *slot = g_malloc0(plen);

            memcpy(slot, newargs, newlen);
            address_space_write(&address_space_memory,
                                dtb_addr + (hwaddr)(old - (const char *)src),
                                MEMTXATTRS_UNSPECIFIED, slot, plen);
        }
    }
}

/*
 * Polling tick for the firmware-bootargs DTB patch.
 *
 * booti() rewrites /chosen/bootargs from U-Boot's environment (from the
 * extlinux append line) immediately before jumping to the kernel, so
 * patching earlier would be overwritten.  The tick therefore does
 * nothing but wait until the CPU reaches the kernel entry in low RAM:
 * by then U-Boot's fixups are finished, the DTB is stable, and the
 * kernel will not unflatten it for a long time (its early phase runs
 * head.S and the page-table setup first).  One patch at that point
 * sticks, and the timer is stopped so the kernel never sees a rewrite
 * while walking the tree.  The kernel Image header is checked so the
 * PC range cannot latch on stray U-Boot code in low RAM.  The first
 * PSCI probe and a deadline are safety nets.
 */
static void rk3588_firmware_bootargs_tick(void *opaque)
{
    RK3588MachineState *s = opaque;
    CPUARMState *env = &s->cpu[0]->env;

    if (!s->firmware_boot || s->kernel_started || s->kernel_entered) {
        return;
    }

    /*
     * The kernel's early code runs either in low RAM (0x00400000,
     * before __primary_switch) or at high virtual addresses afterwards.
     * Either way the DTB has not been unflattened yet (that happens in
     * setup_machine_fdt(), long after the switch), so patching here is
     * safe, and the in-place value store cannot race the kernel's later
     * read.  U-Boot never executes at these addresses.
     */
    if ((env->pc >= RK3588_KERNEL_LOAD_ADDR &&
         env->pc < RK3588_KERNEL_LOAD_ADDR + RK3588_KERNEL_ENTRY_RANGE) ||
        env->pc >= RK3588_KERNEL_VA_BASE) {
        s->kernel_entered = true;
        rk3588_patch_firmware_bootargs(s);
        return;
    }

    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->bootargs_patch_start_ns <
        RK3588_FIRMWARE_BOOTARGS_DEADLINE_NS) {
        timer_mod(s->bootargs_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  RK3588_FIRMWARE_BOOTARGS_INTERVAL_NS);
    }
}

static void rk3588_firmware_patch_tick(void *opaque)
{
    RK3588MachineState *s = opaque;
    ARMCPU *cpu = s->cpu[0];
    CPUARMState *env = &cpu->env;
    uint32_t uboot_entry;
    uint64_t pc;
    bool pc_in_bl31;

    if (!s->firmware_boot || s->firmware_handoff_done) {
        return;
    }

    pc = env->pc;
    if (rk3588_dynamic_fit_handoff(s)) {
        RK3588BootROM *bootrom = &s->bootrom_state;

        if (bootrom->fit_handoff_valid &&
            arm_current_el(env) == 3 &&
            pc >= bootrom->atf_load &&
            pc < bootrom->atf_load + bootrom->atf_size &&
            rk3588_phys_read32(bootrom->uboot_entry, &uboot_entry) &&
            uboot_entry == bootrom->uboot_entry_word) {
            rk3588_schedule_firmware_handoff(s, cpu);
            return;
        }

        rk3588_schedule_firmware_patch(s);
        return;
    }

    /*
     * Keep BL31 writes after SPL hash verification by patching only once the
     * CPU has entered BL31.  The U-Boot proper load may become visible on a
     * later tick, so handoff below is not gated by the current PC.
     */
    pc_in_bl31 = pc >= RK3588_BL31_BASE && pc < RK3588_BL31_LIMIT;
    if (pc_in_bl31) {
        rk3588_patch_bl31_runtime(s);
    }

    if (!s->firmware_atf_entered && s->firmware_patch_done &&
        rk3588_phys_read32(RK3588_UBOOT_LOAD_ADDR, &uboot_entry) &&
        uboot_entry == RK3588_UBOOT_ENTRY_BRANCH) {
        rk3588_schedule_firmware_handoff(s, cpu);
        return;
    }

    rk3588_schedule_firmware_patch(s);
}

static void rk3588_boot_state_reset(void *opaque)
{
    RK3588MachineState *s = opaque;

    if (s->firmware_boot) {
        /*
         * The firmware path needs every PCIe link down: U-Boot's
         * dw-pcie scan skips hosts whose LTSSM is not up (it does not
         * program the outbound iATU before touching the config window).
         * The kernel flips the links up on its first PSCI call, so
         * restore the firmware-path state here on every reset.  Direct
         * -kernel boots and qtests keep the links up.
         */
        s->pcie_links_up = false;
        if (s->pcie3x4) {
            rockchip_pcie_host_set_link_up(s->pcie3x4, false);
        }
        if (s->pcie3x2) {
            rockchip_pcie_host_set_link_up(s->pcie3x2, false);
        }
        if (s->pcie2x1l0) {
            rockchip_pcie_host_set_link_up(s->pcie2x1l0, false);
        }
        if (s->pcie2x1l2) {
            rockchip_pcie_host_set_link_up(s->pcie2x1l2, false);
        }
    }

    rk3588_usb2_host_set_active(s->usb2_host, !s->firmware_boot);
    rk3588_write_atags(s);
    rk3588_seed_iram_firmware_shims(s);
    s->firmware_patch_done = false;
    s->firmware_handoff_done = false;
    s->firmware_atf_entered = false;
    s->kernel_started = false;
    s->kernel_entered = false;
    s->bootrom_state.spl_loaded = false;
    rk3588_schedule_firmware_patch(s);
}

static void rk3588_create_low_memory(RK3588MachineState *s)
{
    MemoryRegion *sysmem = get_system_memory();
    static const uint32_t trampoline[] = {
        0xd286b100, /* movz x0, #0x3588 */
        0xf2b84000, /* movk x0, #0xc200, lsl #16 */
        0xd4000003, /* smc #0 */
        0x14000000, /* b . */
    };

    memory_region_init_ram(&s->sram, NULL, "rk3588.sram",
                           rk3588_memmap[RK3588_SRAM].size, &error_fatal);
    memory_region_add_subregion(sysmem, rk3588_memmap[RK3588_SRAM].base,
                                &s->sram);

    memory_region_init_ram(&s->firmware_scratch, NULL, "rk3588.fw-scratch",
                           rk3588_memmap[RK3588_FIRMWARE_SCRATCH].size,
                           &error_fatal);
    memory_region_add_subregion(sysmem,
                                rk3588_memmap[RK3588_FIRMWARE_SCRATCH].base,
                                &s->firmware_scratch);

    memory_region_init_ram(&s->atags, NULL, "rk3588.atags",
                           rk3588_memmap[RK3588_ATAGS].size, &error_fatal);
    memory_region_add_subregion(sysmem, rk3588_memmap[RK3588_ATAGS].base,
                                &s->atags);
    rk3588_write_atags(s);

    /*
     * Reserved DRAM backing the vendor DTB's ramoops@110000 pstore
     * window (0x110000, 0xe0000 bytes).  The real board's boot firmware
     * keeps the first MiBs of DRAM reserved below the 0x200000 memory
     * start; the pstore driver reads this window at probe and must not
     * take a synchronous external abort on an unmapped address.
     */
    memory_region_init_ram(&s->ramoops, NULL, "rk3588.ramoops",
                           0x000e0000, &error_fatal);
    memory_region_add_subregion(sysmem, 0x00110000, &s->ramoops);

    memory_region_init_ram(&s->iram, NULL, "rk3588.iram",
                           rk3588_memmap[RK3588_IRAM].size, &error_fatal);
    memory_region_add_subregion(sysmem, rk3588_memmap[RK3588_IRAM].base,
                                &s->iram);
    rk3588_seed_iram_firmware_shims(s);

    memory_region_init_ram(&s->bootrom, NULL, "rk3588.bootrom",
                           rk3588_memmap[RK3588_BROM].size, &error_fatal);
    memcpy(memory_region_get_ram_ptr(&s->bootrom), trampoline,
           sizeof(trampoline));
    memory_region_set_readonly(&s->bootrom, true);
    memory_region_add_subregion(sysmem, rk3588_memmap[RK3588_BROM].base,
                                &s->bootrom);

    s->firmware_patch_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           rk3588_firmware_patch_tick, s);
}

static void rk3588_create_atf_ddr(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_ATF_DDR);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), "atf-ddr", OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map_overlap(sbd, 0, RK3588_ATF_DDR_RUNTIME_BASE, 10);
    s->atf_ddr = dev;
    object_unref(OBJECT(dev));
}

static void rk3588_create_zvm_ram(RK3588MachineState *s)
{
    MachineState *ms = MACHINE(s);
    MemoryRegion *sysmem = get_system_memory();
    hwaddr main_ram_limit = rk3588_ram_base(s) + ms->ram_size;
    hwaddr low_base;

    if (!s->zvm_ram) {
        return;
    }

    /*
     * The downstream RK3588 ZVM binary has fixed guest/shared-memory pools:
     *
     *   0x68000000..0xe7efffff: guest RAM allocator
     *   0xe7f00000..0xefffffff: zshm/notify slots
     *   0x100000000..0x1ffffffff: high guest RAM allocator
     *
     * These are intentionally not described to a normal Linux FDT.  Map only
     * the low-window portion not already covered by -m RAM, keeping ordinary
     * board RAM sizing intact while still giving ZVM's fixed pools real RAM.
     */
    low_base = MAX(main_ram_limit, RK3588_ZVM_LOW_RAM_BASE);
    if (low_base < RK3588_ZVM_LOW_RAM_LIMIT) {
        memory_region_init_ram(&s->zvm_low_ram, NULL,
                               "rk3588.zvm-low-ram",
                               RK3588_ZVM_LOW_RAM_LIMIT - low_base,
                               &error_fatal);
        memory_region_add_subregion(sysmem, low_base, &s->zvm_low_ram);
    }

    memory_region_init_ram(&s->zvm_high_ram, NULL, "rk3588.zvm-high-ram",
                           RK3588_ZVM_HIGH_RAM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, RK3588_ZVM_HIGH_RAM_BASE,
                                &s->zvm_high_ram);
}

static void rk3588_create_firmware_mmio(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_FIRMWARE_MMIO);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), "firmware-mmio", OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map_overlap(sbd, 0, RK3588_FIRMWARE_MMIO_BASE, -1000);
    object_unref(OBJECT(dev));
}

static bool rk3588_blk_read(BlockBackend *blk, int64_t offset,
                            void *buf, size_t size, Error **errp)
{
    int ret = blk_pread(blk, offset, size, buf, 0);

    if (ret < 0) {
        error_setg_errno(errp, -ret, "failed to read RK3588 firmware image");
        return false;
    }

    return true;
}

static bool rk3588_dynamic_fit_handoff(RK3588MachineState *s)
{
    const RK3588FirmwareProfile *profile = s->board->firmware_profile;

    return profile && profile->dynamic_fit_handoff;
}

static const char *rk3588_fit_single_string(const void *fit, int node,
                                             const char *property,
                                             Error **errp)
{
    const char *value;
    const char *node_name = fdt_get_name(fit, node, NULL);
    int count = fdt_stringlist_count(fit, node, property);
    int len;

    if (count != 1) {
        if (count < 0) {
            error_setg(errp, "invalid FIT %s/%s property: %s",
                       node_name, property, fdt_strerror(count));
        } else {
            error_setg(errp, "FIT %s/%s contains %d strings, expected 1",
                       node_name, property, count);
        }
        return NULL;
    }

    value = fdt_stringlist_get(fit, node, property, 0, &len);
    if (!value || !len) {
        error_setg(errp, "FIT %s/%s is empty", node_name, property);
        return NULL;
    }

    return value;
}

static bool rk3588_fit_check_string(const void *fit, int node,
                                     const char *property,
                                     const char *expected, Error **errp)
{
    const char *value = rk3588_fit_single_string(fit, node, property, errp);

    if (!value) {
        return false;
    }
    if (strcmp(value, expected)) {
        error_setg(errp, "FIT image %s has %s '%s', expected '%s'",
                   fdt_get_name(fit, node, NULL), property, value, expected);
        return false;
    }

    return true;
}

static bool rk3588_fit_is_uboot_os(const char *value)
{
    return value && (!strcmp(value, "U-Boot") || !strcmp(value, "u-boot"));
}

static bool rk3588_fit_check_uboot_os(const void *fit, int node,
                                      Error **errp)
{
    const char *value = rk3588_fit_single_string(fit, node, "os", errp);

    if (!value) {
        return false;
    }
    if (!rk3588_fit_is_uboot_os(value)) {
        error_setg(errp, "FIT image %s has os '%s', expected U-Boot",
                   fdt_get_name(fit, node, NULL), value);
        return false;
    }

    return true;
}

static bool rk3588_fit_get_address(const void *fit, int node,
                                    const char *property, bool optional,
                                    hwaddr *value, Error **errp)
{
    const void *data;
    int len;

    data = fdt_getprop(fit, node, property, &len);
    if (!data) {
        if (optional && len == -FDT_ERR_NOTFOUND) {
            return true;
        }
        error_setg(errp, "cannot read FIT image %s/%s: %s",
                   fdt_get_name(fit, node, NULL), property,
                   fdt_strerror(len));
        return false;
    }

    switch (len) {
    case sizeof(fdt32_t):
        *value = fdt32_ld(data);
        return true;
    case sizeof(fdt64_t):
        *value = fdt64_ld(data);
        return true;
    default:
        error_setg(errp, "FIT image %s/%s has invalid length %d",
                   fdt_get_name(fit, node, NULL), property, len);
        return false;
    }
}

static bool rk3588_fit_get_u32(const void *fit, int node,
                                const char *property, uint32_t *value,
                                Error **errp)
{
    const fdt32_t *data;
    int len;

    data = fdt_getprop(fit, node, property, &len);
    if (!data) {
        error_setg(errp, "cannot read FIT image %s/%s: %s",
                   fdt_get_name(fit, node, NULL), property,
                   fdt_strerror(len));
        return false;
    }
    if (len != sizeof(*data)) {
        error_setg(errp, "FIT image %s/%s has invalid length %d",
                   fdt_get_name(fit, node, NULL), property, len);
        return false;
    }

    *value = fdt32_ld(data);
    return true;
}

static const char *rk3588_fit_find_uboot(const void *fit, int config,
                                         int images, Error **errp)
{
    const char *candidate = NULL;
    int count = fdt_stringlist_count(fit, config, "loadables");

    if (count <= 0) {
        error_setg(errp, "FIT configuration has no valid loadables list");
        return NULL;
    }

    for (int i = 0; i < count; i++) {
        const char *name = fdt_stringlist_get(fit, config, "loadables", i,
                                               NULL);

        if (!name) {
            error_setg(errp, "cannot read FIT loadables[%d]", i);
            return NULL;
        }
        if (!strcmp(name, "uboot")) {
            return name;
        }
    }

    for (int i = 0; i < count; i++) {
        const char *name = fdt_stringlist_get(fit, config, "loadables", i,
                                               NULL);
        int image = fdt_subnode_offset(fit, images, name);
        const char *os;

        if (image < 0) {
            error_setg(errp, "FIT loadable '%s' has no image node", name);
            return NULL;
        }
        os = fdt_getprop(fit, image, "os", NULL);
        if (fdt_stringlist_search(fit, image, "type", "standalone") >= 0 &&
            rk3588_fit_is_uboot_os(os)) {
            if (candidate) {
                error_setg(errp, "FIT configuration has multiple U-Boot "
                           "loadables");
                return NULL;
            }
            candidate = name;
        }
    }

    if (!candidate) {
        error_setg(errp, "FIT configuration has no U-Boot loadable");
    }
    return candidate;
}

static bool rk3588_fit_read_image(const void *fit, int images,
                                   const char *name,
                                   const char *expected_type,
                                   const char *expected_os,
                                   uint64_t payload_base,
                                   uint64_t media_size,
                                   RK3588FITImage *image, Error **errp)
{
    uint32_t data_offset;
    int node = fdt_subnode_offset(fit, images, name);

    if (node < 0) {
        error_setg(errp, "FIT configuration references missing image '%s'",
                   name);
        return false;
    }
    if (!rk3588_fit_check_string(fit, node, "type", expected_type, errp) ||
        (strcmp(expected_os, "U-Boot") &&
         !rk3588_fit_check_string(fit, node, "os", expected_os, errp)) ||
        (!strcmp(expected_os, "U-Boot") &&
         !rk3588_fit_check_uboot_os(fit, node, errp)) ||
        !rk3588_fit_check_string(fit, node, "compression", "none", errp) ||
        !rk3588_fit_get_address(fit, node, "load", false,
                                 &image->load, errp) ||
        !rk3588_fit_get_u32(fit, node, "data-size", &image->size, errp) ||
        !rk3588_fit_get_u32(fit, node, "data-offset", &data_offset, errp)) {
        return false;
    }

    image->entry = 0;
    if (!rk3588_fit_get_address(fit, node, "entry", true,
                                 &image->entry, errp)) {
        return false;
    }
    if (!image->entry) {
        image->entry = image->load;
    }

    if (!image->size || image->load > HWADDR_MAX - image->size ||
        image->entry < image->load ||
        image->entry >= image->load + image->size) {
        error_setg(errp, "FIT image '%s' has an invalid load range", name);
        return false;
    }
    if (payload_base > media_size ||
        data_offset > media_size - payload_base ||
        image->size > media_size - payload_base - data_offset) {
        error_setg(errp, "FIT image '%s' external data exceeds boot media",
                   name);
        return false;
    }

    image->media_offset = payload_base + data_offset;
    return true;
}

static bool rk3588_bootrom_prepare_fit_handoff(RK3588MachineState *s,
                                                BlockBackend *blk,
                                                Error **errp)
{
    const RK3588FirmwareProfile *profile = s->board->firmware_profile;
    MachineState *ms = MACHINE(s);
    struct fdt_header header;
    g_autofree uint8_t *fit = NULL;
    RK3588FITImage atf = { 0 };
    RK3588FITImage uboot = { 0 };
    const char *default_name;
    const char *atf_name;
    const char *uboot_name;
    int64_t media_len;
    uint64_t media_size;
    uint64_t fit_offset;
    uint64_t payload_base;
    uint64_t entry_delta;
    uint32_t metadata_size;
    uint32_t entry_word;
    int configs;
    int config;
    int images;
    int ret;

    if (!rk3588_dynamic_fit_handoff(s)) {
        return true;
    }

    s->bootrom_state.fit_handoff_valid = false;
    if (!profile->fit_alignment ||
        (profile->fit_alignment & (profile->fit_alignment - 1)) ||
        profile->fit_alignment > RK3588_FIT_METADATA_MAX_SIZE) {
        error_setg(errp, "%s has invalid FIT alignment %u",
                   s->board->machine_name, profile->fit_alignment);
        return false;
    }

    media_len = blk_getlength(blk);
    if (media_len < 0) {
        error_setg_errno(errp, -media_len,
                         "cannot determine RK3588 boot media size");
        return false;
    }
    media_size = media_len;
    fit_offset = s->bootrom_state.brom_bootsource ==
                 RK3588_BROM_BOOTSOURCE_SPINOR && profile->spi_fit_offset ?
                 profile->spi_fit_offset : profile->fit_offset;
    if (fit_offset > media_size ||
        sizeof(header) > media_size - fit_offset ||
        !rk3588_blk_read(blk, fit_offset, &header,
                         sizeof(header), errp)) {
        if (!*errp) {
            error_setg(errp, "%s FIT header exceeds boot media",
                       s->board->machine_name);
        }
        return false;
    }

    ret = fdt_check_header(&header);
    if (ret < 0) {
        error_setg(errp, "%s boot media has an invalid FIT header: %s",
                   s->board->machine_name, fdt_strerror(ret));
        return false;
    }
    metadata_size = fdt_totalsize(&header);
    if (metadata_size < sizeof(header) ||
        metadata_size > RK3588_FIT_METADATA_MAX_SIZE ||
        metadata_size > media_size - fit_offset) {
        error_setg(errp, "%s FIT metadata size 0x%x is invalid",
                   s->board->machine_name, metadata_size);
        return false;
    }

    fit = g_malloc(metadata_size);
    if (!rk3588_blk_read(blk, fit_offset, fit, metadata_size,
                         errp)) {
        return false;
    }
    ret = fdt_check_full(fit, metadata_size);
    if (ret < 0) {
        error_setg(errp, "%s FIT metadata is invalid: %s",
                   s->board->machine_name, fdt_strerror(ret));
        return false;
    }

    payload_base = ROUND_UP((uint64_t)metadata_size,
                            profile->fit_alignment);
    if (payload_base > media_size - fit_offset) {
        error_setg(errp, "%s FIT payload exceeds boot media",
                   s->board->machine_name);
        return false;
    }
    payload_base += fit_offset;

    configs = fdt_path_offset(fit, "/configurations");
    images = fdt_path_offset(fit, "/images");
    if (configs < 0 || images < 0) {
        error_setg(errp, "%s FIT lacks configurations or images",
                   s->board->machine_name);
        return false;
    }
    default_name = rk3588_fit_single_string(fit, configs, "default", errp);
    if (!default_name) {
        return false;
    }
    config = fdt_subnode_offset(fit, configs, default_name);
    if (config < 0) {
        error_setg(errp, "FIT default configuration '%s' is missing",
                   default_name);
        return false;
    }
    atf_name = rk3588_fit_single_string(fit, config, "firmware", errp);
    if (!atf_name) {
        return false;
    }
    uboot_name = rk3588_fit_find_uboot(fit, config, images, errp);
    if (!uboot_name ||
        !rk3588_fit_read_image(fit, images, atf_name, "firmware",
                                "arm-trusted-firmware", payload_base,
                                media_size, &atf, errp) ||
        !rk3588_fit_read_image(fit, images, uboot_name, "standalone",
                                "U-Boot", payload_base, media_size,
                                &uboot, errp)) {
        return false;
    }

    if (atf.load >= rk3588_memmap[RK3588_SRAM].size ||
        atf.size > rk3588_memmap[RK3588_SRAM].size - atf.load) {
        error_setg(errp, "FIT ATF image lies outside RK3588 SRAM");
        return false;
    }
    if (uboot.load < rk3588_ram_base(s) ||
        uboot.load - rk3588_ram_base(s) >= ms->ram_size ||
        uboot.size > ms->ram_size -
                     (uboot.load - rk3588_ram_base(s))) {
        error_setg(errp, "FIT U-Boot image lies outside guest RAM");
        return false;
    }

    entry_delta = uboot.entry - uboot.load;
    if (uboot.size < sizeof(entry_word) ||
        entry_delta > uboot.size - sizeof(entry_word) ||
        !rk3588_blk_read(blk, uboot.media_offset + entry_delta,
                         &entry_word, sizeof(entry_word), errp)) {
        if (!*errp) {
            error_setg(errp, "FIT U-Boot entry does not contain an "
                       "instruction");
        }
        return false;
    }

    s->bootrom_state.atf_load = atf.load;
    s->bootrom_state.atf_size = atf.size;
    s->bootrom_state.uboot_load = uboot.load;
    s->bootrom_state.uboot_entry = uboot.entry;
    s->bootrom_state.uboot_size = uboot.size;
    s->bootrom_state.uboot_entry_word = le32_to_cpu(entry_word);
    s->bootrom_state.fit_handoff_valid = true;
    return true;
}

static bool rk3588_load_rkns_image(BlockBackend *blk,
                                   const RK3588HeaderV2 *hdr,
                                   uint64_t rkns_base, unsigned int index,
                                   uint8_t **data,
                                   size_t *size, Error **errp)
{
    uint32_t size_and_off = le32_to_cpu(hdr->images[index].size_and_off);
    uint32_t sectors = size_and_off >> 16;
    uint32_t offset_sectors = size_and_off & 0xffff;
    size_t bytes;

    if (!sectors || !offset_sectors) {
        error_setg(errp, "invalid RK3588 RKNS image%u descriptor", index);
        return false;
    }

    bytes = sectors * RK3588_RKNS_SECTOR_SIZE;
    if (bytes > RK3588_SRAM_SIZE) {
        error_setg(errp, "RK3588 RKNS image%u is too large: %zu bytes",
                   index, bytes);
        return false;
    }

    *data = g_malloc0(bytes);
    *size = bytes;
    return rk3588_blk_read(blk,
                           rkns_base + offset_sectors * RK3588_RKNS_SECTOR_SIZE,
                           *data, bytes, errp);
}

static bool rk3588_bootrom_prepare(RK3588MachineState *s, Error **errp)
{
    const RK3588BoardConfig *board = s->board;
    DriveInfo *di = NULL;
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;
    RK3588HeaderV2 hdr;
    uint8_t *tpl = NULL;
    size_t tpl_size = 0;
    uint8_t *spl = NULL;
    size_t spl_size = 0;
    uint64_t rkns_base = RK3588_RKNS_LBA * RK3588_RKNS_SECTOR_SIZE;
    uint32_t nimage;

    if (board->firmware_spi) {
        di = drive_get(IF_MTD, 0, 0);
        blk = di ? blk_by_legacy_dinfo(di) : NULL;
        if (blk) {
            s->bootrom_state.brom_bootsource =
                RK3588_BROM_BOOTSOURCE_SPINOR;
        }
    }
    if (!blk) {
        di = drive_get(IF_SD, 0, board->firmware_sd_unit);
        blk = di ? blk_by_legacy_dinfo(di) : NULL;
        s->bootrom_state.brom_bootsource = board->brom_bootsource;
    }
    if (!blk) {
        if (board->firmware_spi) {
            error_setg(errp, "%s firmware boot requires "
                       "-drive if=mtd,index=0,file=<spi-image>,format=raw "
                       "and SD payload media at if=sd,index=%u",
                       board->machine_name, board->firmware_sd_unit + 2);
        } else {
            error_setg(errp, "%s firmware boot requires "
                       "-drive if=sd,index=%u,file=<rockchip-image>,format=raw",
                       board->machine_name, board->firmware_sd_unit);
        }
        return false;
    }

    if (!rk3588_blk_read(blk, rkns_base, &hdr, sizeof(hdr), errp)) {
        return false;
    }

    if (le32_to_cpu(hdr.magic) != RK3588_RKNS_MAGIC) {
        error_setg(errp, "%s boot media does not contain an RKNS v2 header",
                   board->machine_name);
        return false;
    }

    nimage = le32_to_cpu(hdr.size_and_nimage) >> 16;
    if (nimage < 2) {
        error_setg(errp, "RK3588 RKNS header contains %u image(s), need 2",
                   nimage);
        return false;
    }

    if (!rk3588_load_rkns_image(blk, &hdr, rkns_base, 0, &tpl, &tpl_size,
                                errp)) {
        return false;
    }
    if (!rk3588_load_rkns_image(blk, &hdr, rkns_base, 1, &spl, &spl_size,
                                errp)) {
        g_free(tpl);
        return false;
    }
    if (!rk3588_bootrom_prepare_fit_handoff(s, blk, errp)) {
        g_free(tpl);
        g_free(spl);
        return false;
    }

    if (address_space_write(&address_space_memory, RK3588_TPL_LOAD_ADDR,
                            MEMTXATTRS_UNSPECIFIED, tpl, tpl_size) !=
        MEMTX_OK) {
        error_setg(errp, "failed to load RK3588 TPL into IRAM");
        g_free(tpl);
        g_free(spl);
        return false;
    }
    s->bootrom_state.spl = spl;
    s->bootrom_state.spl_size = spl_size;
    s->bootrom_state.tpl_entry = RK3588_TPL_LOAD_ADDR;
    s->bootrom_state.spl_loaded = false;
    g_free(tpl);

    return true;
}

static void rk3588_bootrom_load_spl(RK3588MachineState *s, ARMCPU *cpu)
{
    CPUARMState *env = &cpu->env;

    if (!s->bootrom_state.spl || s->bootrom_state.spl_loaded) {
        return;
    }

    address_space_write(&address_space_memory, rk3588_memmap[RK3588_SRAM].base,
                        MEMTXATTRS_UNSPECIFIED, s->bootrom_state.spl,
                        s->bootrom_state.spl_size);
    if (!rk3588_dynamic_fit_handoff(s)) {
        rk3588_patch_spl_atf_handoff();
    }
    stl_le_p(memory_region_get_ram_ptr(&s->iram) + 0x10,
             s->bootrom_state.brom_bootsource);
    s->bootrom_state.spl_loaded = true;

    cpu_set_pc(CPU(cpu), rk3588_memmap[RK3588_SRAM].base);
    env->xregs[30] = RK3588_BROM_TRAMPOLINE;
    env->xregs[31] = rk3588_memmap[RK3588_SRAM].base +
                     rk3588_memmap[RK3588_SRAM].size - 0x100;
    env->sp_el[3] = env->xregs[31];
    arm_rebuild_hflags(env);
}

typedef struct RK3588FirmwareReset {
    RK3588MachineState *machine;
    ARMCPU *cpu;
    bool primary;
} RK3588FirmwareReset;

static void rk3588_firmware_cpu_reset(void *opaque)
{
    RK3588FirmwareReset *rst = opaque;
    RK3588MachineState *s = rst->machine;
    ARMCPU *cpu = rst->cpu;
    CPUState *cs = CPU(cpu);
    CPUARMState *env = &cpu->env;

    cpu_reset(cs);

    if (!rst->primary) {
        cs->halted = true;
        return;
    }

    arm_emulate_firmware_reset(cs, 3);
    cpu_set_pc(cs, s->bootrom_state.tpl_entry);
    env->xregs[30] = RK3588_BROM_TRAMPOLINE;
    env->xregs[31] = rk3588_memmap[RK3588_IRAM].base +
                     RK3588_SRAM_SIZE - 0x100;
    env->sp_el[3] = env->xregs[31];
    arm_rebuild_hflags(env);
}

static void rk3588_register_firmware_reset(RK3588MachineState *s)
{
    MachineState *ms = MACHINE(s);

    for (unsigned int n = 0; n < ms->smp.cpus; n++) {
        RK3588FirmwareReset *rst = g_new0(RK3588FirmwareReset, 1);

        rst->machine = s;
        rst->cpu = s->cpu[n];
        rst->primary = n == 0;
        qemu_register_reset(rk3588_firmware_cpu_reset, rst);
    }
}

static void rk3588_create_gic(RK3588MachineState *s)
{
    MachineState *ms = MACHINE(s);
    SysBusDevice *gicbusdev;
    QList *redist_region_count;
    uint32_t redist_capacity;

    s->gic = qdev_new(gicv3_class_name());
    object_property_add_child(OBJECT(s), "gic", OBJECT(s->gic));
    qdev_prop_set_uint32(s->gic, "revision", 3);
    qdev_prop_set_uint32(s->gic, "num-cpu", ms->smp.cpus);
    qdev_prop_set_uint32(s->gic, "num-irq",
                         RK3588_NUM_SPI_IRQS + GIC_INTERNAL);
    qdev_prop_set_bit(s->gic, "has-security-extensions", true);
    qdev_prop_set_bit(s->gic, "has-lpi", true);

    redist_capacity = rk3588_memmap[RK3588_GIC_REDIST].size / GICV3_REDIST_SIZE;
    redist_region_count = qlist_new();
    qlist_append_int(redist_region_count, MIN(ms->smp.cpus, redist_capacity));
    qdev_prop_set_array(s->gic, "redist-region-count", redist_region_count);
    object_property_set_link(OBJECT(s->gic), "sysmem", OBJECT(get_system_memory()),
                             &error_fatal);

    gicbusdev = SYS_BUS_DEVICE(s->gic);
    sysbus_realize_and_unref(gicbusdev, &error_fatal);
    sysbus_mmio_map(gicbusdev, 0, rk3588_memmap[RK3588_GIC_DIST].base);
    sysbus_mmio_map(gicbusdev, 1, rk3588_memmap[RK3588_GIC_REDIST].base);

    for (unsigned int n = 0; n < ms->smp.cpus; n++) {
        DeviceState *cpudev = DEVICE(s->cpu[n]);
        int intidbase = RK3588_NUM_SPI_IRQS + n * GIC_INTERNAL;
        static const int timer_irqs[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
            [GTIMER_HYP] = ARCH_TIMER_NS_EL2_IRQ,
            [GTIMER_SEC] = ARCH_TIMER_S_EL1_IRQ,
            [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ,
            [GTIMER_S_EL2_PHYS] = ARCH_TIMER_S_EL2_IRQ,
            [GTIMER_S_EL2_VIRT] = ARCH_TIMER_S_EL2_VIRT_IRQ,
        };

        for (int irq = 0; irq < ARRAY_SIZE(timer_irqs); irq++) {
            qdev_connect_gpio_out(cpudev, irq,
                                  qdev_get_gpio_in(s->gic,
                                                   intidbase + timer_irqs[irq]));
        }

        qdev_connect_gpio_out_named(cpudev, "gicv3-maintenance-interrupt", 0,
                                    qdev_get_gpio_in(s->gic,
                                                     intidbase +
                                                     ARCH_GIC_MAINT_IRQ));
        qdev_connect_gpio_out_named(cpudev, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(s->gic,
                                                     intidbase +
                                                     VIRTUAL_PMU_IRQ));

        sysbus_connect_irq(gicbusdev, n, qdev_get_gpio_in(cpudev, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbusdev, n + ms->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbusdev, n + 2 * ms->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbusdev, n + 3 * ms->smp.cpus,
                           qdev_get_gpio_in(cpudev, ARM_CPU_VFIQ));
    }
}

static void rk3588_create_its(RK3588MachineState *s)
{
    static const int its_memmap[] = {
        RK3588_GIC_ITS0,
        RK3588_GIC_ITS1,
    };

    for (unsigned int i = 0; i < ARRAY_SIZE(its_memmap); i++) {
        g_autofree char *name = g_strdup_printf("its%u", i);
        DeviceState *dev = qdev_new(its_class_name());
        SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

        object_property_add_child(OBJECT(s), name, OBJECT(dev));
        object_property_set_link(OBJECT(dev), "parent-gicv3",
                                 OBJECT(s->gic), &error_abort);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, rk3588_memmap[its_memmap[i]].base);
        s->its[i] = dev;
    }
}

static void rk3588_create_uarts(RK3588MachineState *s)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(rk3588_uarts); i++) {
        const RK3588UARTInfo *u = &rk3588_uarts[i];
        DeviceState *vendor;
        SysBusDevice *vendor_sbd;
        g_autofree char *name = g_strdup_printf("%s-vendor", u->name);
        int chr_index = u->chr_index;

        if (u->memidx == RK3588_UART2) {
            /* The console UART keeps the legacy chardev slot. */
            chr_index = s->zephyr_ram ? 1 : 0;
        } else if (s->zephyr_ram && chr_index >= 1) {
            /* UART2 claims slot 1 in zephyr mode; shift the others. */
            chr_index++;
        }

        /*
         * Each UART is a Synopsys dw-apb-uart (16550-compatible).
         * serial_mm models the standard 16550 range (8 registers,
         * regshift 2 -> a 0x20-byte window).  The DesignWare extension
         * registers (USR @0x7c, ...) sit above that window; cover only
         * that range so it does not overlap serial_mm.  Ports without
         * routed I/O pass no chardev, so they stay usable in the guest
         * but have no external backend.
         */
        serial_mm_init(get_system_memory(), rk3588_memmap[u->memidx].base, 2,
                       qdev_get_gpio_in(s->gic, u->spi),
                       RK3588_UART_BAUDBASE,
                       chr_index >= 0 ? serial_hd(chr_index) : NULL,
                       DEVICE_LITTLE_ENDIAN);

        vendor = qdev_new(TYPE_DW_APB_UART_VENDOR);
        vendor_sbd = SYS_BUS_DEVICE(vendor);
        object_property_add_child(OBJECT(s), name, OBJECT(vendor));
        sysbus_realize(vendor_sbd, &error_fatal);
        sysbus_mmio_map(vendor_sbd, 0, rk3588_memmap[u->memidx].base +
                        DW_APB_UART_VENDOR_BASE);
    }
}

static void rk3588_attach_emmc_card(RK3588MachineState *s)
{
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;
    DeviceState *sdhci = s->sdhci;
    BusState *bus = qdev_get_child_bus(sdhci, "sd-bus");
    DeviceState *card;

    if (!di) {
        return;
    }
    if (!bus) {
        error_report("%s: eMMC controller has no sd-bus",
                     s->board->machine_name);
        exit(EXIT_FAILURE);
    }

    card = qdev_new(TYPE_EMMC);
    qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
    qdev_realize_and_unref(card, bus, &error_fatal);
}

/*
 * Attach an SD card to the dw_mmc SD-card controller. The board maps
 * `-drive if=sd,index=2` to this slot: QEMU's block layer maps
 * `if=sd,index=2` to drive_get(IF_SD, bus=0, unit=2) (since
 * if_max_devs[IF_SD] = 0, index N maps to bus=0, unit=N). The eMMC
 * at the SDHCI takes unit=0; unit=1 is left unused to mirror the
 * common sd-host numbering (the secondary slot on most boards), so
 * the SD card is at unit=2.
 */
static void rk3588_attach_sd_card(RK3588MachineState *s, DeviceState *sdmmc)
{
    DriveInfo *di = drive_get(IF_SD, 0, 2);
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;
    BusState *bus = qdev_get_child_bus(sdmmc, "sd-bus");
    DeviceState *card;

    if (!di) {
        return;
    }
    if (!bus) {
        error_report("%s: dw_mmc has no sd-bus", s->board->machine_name);
        exit(EXIT_FAILURE);
    }

    card = qdev_new(TYPE_SD_CARD);
    qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
    qdev_realize_and_unref(card, bus, &error_fatal);
}

static void rk3588_create_sdhci(RK3588MachineState *s)
{
    SysBusDevice *sbd;

    s->sdhci = qdev_new(TYPE_SYSBUS_SDHCI);
    object_property_add_child(OBJECT(s), "sdhci", OBJECT(s->sdhci));
    object_property_set_uint(OBJECT(s->sdhci), "sd-spec-version", 3,
                             &error_abort);
    object_property_set_uint(OBJECT(s->sdhci), "capareg", 0x280737ec6481,
                             &error_abort);
    object_property_set_uint(OBJECT(s->sdhci), "uhs", UHS_I, &error_abort);
    object_property_set_uint(OBJECT(s->sdhci), "vendor-area1", 0x500,
                             &error_abort);
    object_property_set_uint(OBJECT(s->sdhci), "vendor-area2", 0x800,
                             &error_abort);
    sbd = SYS_BUS_DEVICE(s->sdhci);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_SDHCI].base);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, RK3588_SDHCI_SPI));

    DeviceState *vendor = qdev_new(TYPE_ROCKCHIP_DWCMSHC_VENDOR);
    SysBusDevice *vendor_sbd = SYS_BUS_DEVICE(vendor);

    object_property_add_child(OBJECT(s), "sdhci-vendor", OBJECT(vendor));
    sysbus_realize(vendor_sbd, &error_fatal);
    sysbus_mmio_map(vendor_sbd, 0,
                    rk3588_memmap[RK3588_SDHCI].base +
                    ROCKCHIP_DWCMSHC_VENDOR_BASE);

    rk3588_attach_emmc_card(s);
}

static void rk3588_create_gpio(RK3588MachineState *s)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(s->gpio); i++) {
        int idx = RK3588_GPIO0 + i;
        g_autofree char *name = g_strdup_printf("gpio%u", i);
        SysBusDevice *sbd;

        s->gpio[i] = qdev_new(TYPE_ROCKCHIP_GPIO);
        object_property_add_child(OBJECT(s), name, OBJECT(s->gpio[i]));
        sbd = SYS_BUS_DEVICE(s->gpio[i]);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, rk3588_memmap[idx].base);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(s->gic, RK3588_GPIO0_SPI + i));
    }
}

static void rk3588_create_gmac(RK3588MachineState *s)
{
    SysBusDevice *sbd;

    /*
     * Synopsys dwmac-4.20a (GMAC4). Boards select the RK3588 GMAC instances
     * that their FDT advertises as "rockchip,rk3588-gmac",
     * "snps,dwmac-4.20a". Linux stmmac reads MAC_VERSION @0x110 expecting
     * SNPSVER 0x51 (GMAC4); TYPE_DWMAC4 returns exactly that, letting
     * stmmac_bind complete and the interface enumerate. Replaces the older
     * TYPE_NPCM_GMAC v3.50a model which returned the wrong synth-id and never
     * bound.
     */
    if (s->board->gmac_mask & BIT(0)) {
        s->gmac0 = qdev_new(TYPE_DWMAC4);
        object_property_add_child(OBJECT(s), "gmac0", OBJECT(s->gmac0));
        qemu_configure_nic_device(s->gmac0, false, "gmac0");
        sbd = SYS_BUS_DEVICE(s->gmac0);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_GMAC0].base);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(s->gic, RK3588_GMAC0_SPI));
    }

    if (s->board->gmac_mask & BIT(1)) {
        s->gmac1 = qdev_new(TYPE_DWMAC4);
        object_property_add_child(OBJECT(s), "gmac1", OBJECT(s->gmac1));
        qemu_configure_nic_device(s->gmac1, true, "gmac1");
        sbd = SYS_BUS_DEVICE(s->gmac1);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_GMAC1].base);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(s->gic, RK3588_GMAC1_SPI));
    }
}

enum {
    RK3588_PCIE_IRQ_ERR,
    RK3588_PCIE_IRQ_LEGACY,
    RK3588_PCIE_IRQ_MSG,
    RK3588_PCIE_IRQ_PMC,
    RK3588_PCIE_IRQ_SYS,
};

static DeviceState *rk3588_create_pcie_host(RK3588MachineState *s,
                                             const char *name,
                                             const char *vmstate_id,
                                             hwaddr dbi_base,
                                             hwaddr apb_base,
                                             uint32_t domain,
                                             uint8_t bus_nr,
                                             bool link_down,
                                             const int *spis)
{
    DeviceState *dev = qdev_new(TYPE_ROCKCHIP_PCIE_HOST);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (vmstate_id) {
        dev->id = g_strdup(vmstate_id);
    }
    qdev_prop_set_bit(dev, "link-up", !link_down);
    qdev_prop_set_uint32(dev, "domain", domain);
    qdev_prop_set_uint8(dev, "bus-nr", bus_nr);
    object_property_add_child(OBJECT(s), name, OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);

    sysbus_mmio_map(sbd, 0, dbi_base);
    sysbus_mmio_map(sbd, 1, apb_base);
    sysbus_mmio_map(sbd, 2, dbi_base + ROCKCHIP_PCIE_DBI_CORE_SIZE);

    for (unsigned int i = 0; i < 4; i++) {
        sysbus_connect_irq(sbd, i,
                           qdev_get_gpio_in(s->gic,
                                           spis[RK3588_PCIE_IRQ_LEGACY]));
    }
    sysbus_connect_irq(sbd, ROCKCHIP_PCIE_MSG_IRQ,
                       qdev_get_gpio_in(s->gic,
                                       spis[RK3588_PCIE_IRQ_MSG]));
    sysbus_connect_irq(sbd, ROCKCHIP_PCIE_ERR_IRQ,
                       qdev_get_gpio_in(s->gic,
                                       spis[RK3588_PCIE_IRQ_ERR]));
    sysbus_connect_irq(sbd, ROCKCHIP_PCIE_PMC_IRQ,
                       qdev_get_gpio_in(s->gic,
                                       spis[RK3588_PCIE_IRQ_PMC]));
    sysbus_connect_irq(sbd, ROCKCHIP_PCIE_SYS_IRQ,
                       qdev_get_gpio_in(s->gic,
                                       spis[RK3588_PCIE_IRQ_SYS]));

    return dev;
}

static void rk3588_create_rknpu(RK3588MachineState *s)
{
    static const int pc_memmap[] = {
        RK3588_RKNN0_PC,
        RK3588_RKNN1_PC,
        RK3588_RKNN2_PC,
    };
    static const int cna_memmap[] = {
        RK3588_RKNN0_CNA,
        RK3588_RKNN1_CNA,
        RK3588_RKNN2_CNA,
    };
    static const int core_memmap[] = {
        RK3588_RKNN0_CORE,
        RK3588_RKNN1_CORE,
        RK3588_RKNN2_CORE,
    };
    static const int iommu_memmap0[] = {
        RK3588_RKNN0_MMU0,
        RK3588_RKNN1_MMU,
        RK3588_RKNN2_MMU,
    };
    static const int iommu_memmap1[] = {
        RK3588_RKNN0_MMU1,
        -1,
        -1,
    };
    static const int irq[] = {
        RK3588_RKNN0_SPI,
        RK3588_RKNN1_SPI,
        RK3588_RKNN2_SPI,
    };

    if (!s->rknpu) {
        return;
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(s->rknn_mmu); i++) {
        g_autofree char *name = g_strdup_printf("rknn-mmu%u", i);
        g_autofree char *irq_name = g_strdup_printf("rknn-irq-or%u", i);
        SysBusDevice *sbd;

        s->rknn_mmu[i] = qdev_new(TYPE_ROCKCHIP_IOMMU);
        qdev_prop_set_uint32(s->rknn_mmu[i], "num-mmu",
                             iommu_memmap1[i] >= 0 ? 2 : 1);
        qdev_prop_set_uint32(s->rknn_mmu[i], "core-index", i);
        object_property_add_child(OBJECT(s), name, OBJECT(s->rknn_mmu[i]));
        sbd = SYS_BUS_DEVICE(s->rknn_mmu[i]);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, rk3588_memmap[iommu_memmap0[i]].base);
        if (iommu_memmap1[i] >= 0) {
            sysbus_mmio_map(sbd, 1, rk3588_memmap[iommu_memmap1[i]].base);
        }

        s->rknn_irq_or[i] = qdev_new(TYPE_OR_IRQ);
        qdev_prop_set_uint16(s->rknn_irq_or[i], "num-lines", 2);
        object_property_add_child(OBJECT(s), irq_name,
                                  OBJECT(s->rknn_irq_or[i]));
        qdev_realize(s->rknn_irq_or[i], NULL, &error_fatal);
        qdev_connect_gpio_out(s->rknn_irq_or[i], 0,
                              qdev_get_gpio_in(s->gic, irq[i]));
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(s->rknn_irq_or[i], 0));
    }

    for (unsigned int i = 0; i < ARRAY_SIZE(s->rknn); i++) {
        g_autofree char *name = g_strdup_printf("rknn%u", i);
        MemoryRegion *dma_mr = rockchip_iommu_get_memory_region(
            ROCKCHIP_IOMMU(s->rknn_mmu[i]));
        SysBusDevice *sbd;

        s->rknn[i] = qdev_new(TYPE_ROCKCHIP_RKNN_CORE);
        qdev_prop_set_uint32(s->rknn[i], "core-index", i);
        object_property_set_link(OBJECT(s->rknn[i]), "dma",
                                 OBJECT(dma_mr), &error_fatal);
        object_property_add_child(OBJECT(s), name, OBJECT(s->rknn[i]));
        sbd = SYS_BUS_DEVICE(s->rknn[i]);
        sysbus_realize(sbd, &error_fatal);
        sysbus_mmio_map(sbd, 0, rk3588_memmap[pc_memmap[i]].base);
        sysbus_mmio_map(sbd, 1, rk3588_memmap[cna_memmap[i]].base);
        sysbus_mmio_map(sbd, 2, rk3588_memmap[core_memmap[i]].base);
        sysbus_mmio_map(sbd, 3,
                        rk3588_memmap[pc_memmap[i]].base +
                        ROCKCHIP_RKNN_DPU_OFFSET);
        sysbus_mmio_map(sbd, 4,
                        rk3588_memmap[pc_memmap[i]].base +
                        ROCKCHIP_RKNN_GLOBAL_OFFSET);
        sysbus_mmio_map(sbd, 5,
                        rk3588_memmap[pc_memmap[i]].base +
                        ROCKCHIP_RKNN_PPU_OFFSET);
        sysbus_mmio_map(sbd, 6,
                        rk3588_memmap[pc_memmap[i]].base +
                        ROCKCHIP_RKNN_PPU_RDMA_OFFSET);
        sysbus_connect_irq(sbd, 0,
                           qdev_get_gpio_in(s->rknn_irq_or[i], 1));
        qdev_connect_gpio_out_named(
            s->cru, "rknpu-reset", i,
            qdev_get_gpio_in_named(s->rknn[i], "reset", 0));
    }
}

static void rk3588_create_pcie2x1(RK3588MachineState *s)
{
    static const int pcie2x1l0_spis[] = {
        [RK3588_PCIE_IRQ_ERR] = RK3588_PCIE2X1L0_ERR_SPI,
        [RK3588_PCIE_IRQ_LEGACY] = RK3588_PCIE2X1L0_LEGACY_SPI,
        [RK3588_PCIE_IRQ_MSG] = RK3588_PCIE2X1L0_MSG_SPI,
        [RK3588_PCIE_IRQ_PMC] = RK3588_PCIE2X1L0_PMC_SPI,
        [RK3588_PCIE_IRQ_SYS] = RK3588_PCIE2X1L0_SYS_SPI,
    };
    static const int pcie2x1l2_spis[] = {
        [RK3588_PCIE_IRQ_ERR] = RK3588_PCIE2X1L2_ERR_SPI,
        [RK3588_PCIE_IRQ_LEGACY] = RK3588_PCIE2X1L2_LEGACY_SPI,
        [RK3588_PCIE_IRQ_MSG] = RK3588_PCIE2X1L2_MSG_SPI,
        [RK3588_PCIE_IRQ_PMC] = RK3588_PCIE2X1L2_PMC_SPI,
        [RK3588_PCIE_IRQ_SYS] = RK3588_PCIE2X1L2_SYS_SPI,
    };

    bool firmware_path = !MACHINE(s)->kernel_filename && !qtest_enabled();

    if (s->board->pcie2x1_mask & BIT(0)) {
        s->pcie2x1l0 = rk3588_create_pcie_host(
            s, "pcie2x1l0", "pcie2x1l0",
            rk3588_memmap[RK3588_PCIE2X1L0_DBI].base,
            rk3588_memmap[RK3588_PCIE2X1L0_APB].base,
            2, 0x20, firmware_path, pcie2x1l0_spis);
    }
    if (s->board->pcie2x1_mask & BIT(2)) {
        s->pcie2x1l2 = rk3588_create_pcie_host(
            s, "pcie2x1l2", "pcie2x1l2",
            rk3588_memmap[RK3588_PCIE2X1L2_DBI].base,
            rk3588_memmap[RK3588_PCIE2X1L2_APB].base,
            4, 0x40, firmware_path, pcie2x1l2_spis);
    }
}

/*
 * Bring all modeled PCIe links up.  During the firmware path the hosts
 * start with the links down so U-Boot's dw-pcie scan (which does not
 * program the outbound iATU before touching the config window) skips
 * them cleanly; the first kernel PSCI call flips the links up before the
 * kernel's dw-rockchip probe runs.
 */
static void rk3588_pcie_set_links_up(RK3588MachineState *s)
{
    if (s->pcie_links_up) {
        return;
    }
    if (s->pcie3x4) {
        rockchip_pcie_host_set_link_up(s->pcie3x4, true);
    }
    if (s->pcie3x2) {
        rockchip_pcie_host_set_link_up(s->pcie3x2, true);
    }
    if (s->pcie2x1l0) {
        rockchip_pcie_host_set_link_up(s->pcie2x1l0, true);
    }
    if (s->pcie2x1l2) {
        rockchip_pcie_host_set_link_up(s->pcie2x1l2, true);
    }
    s->pcie_links_up = true;
}

static void rk3588_create_pcie(RK3588MachineState *s)
{
    static const int pcie3x4_spis[] = {
        [RK3588_PCIE_IRQ_ERR] = RK3588_PCIE3X4_ERR_SPI,
        [RK3588_PCIE_IRQ_LEGACY] = RK3588_PCIE3X4_LEGACY_SPI,
        [RK3588_PCIE_IRQ_MSG] = RK3588_PCIE3X4_MSG_SPI,
        [RK3588_PCIE_IRQ_PMC] = RK3588_PCIE3X4_PMC_SPI,
        [RK3588_PCIE_IRQ_SYS] = RK3588_PCIE3X4_SYS_SPI,
    };
    static const int pcie3x2_spis[] = {
        [RK3588_PCIE_IRQ_ERR] = RK3588_PCIE3X2_ERR_SPI,
        [RK3588_PCIE_IRQ_LEGACY] = RK3588_PCIE3X2_LEGACY_SPI,
        [RK3588_PCIE_IRQ_MSG] = RK3588_PCIE3X2_MSG_SPI,
        [RK3588_PCIE_IRQ_PMC] = RK3588_PCIE3X2_PMC_SPI,
        [RK3588_PCIE_IRQ_SYS] = RK3588_PCIE3X2_SYS_SPI,
    };

    /*
     * During firmware boot the links start down so U-Boot skips the
     * hosts without touching the unprogrammed config windows; the kernel
     * sees the links up (see rk3588_pcie_set_links_up).  qtests never
     * run the firmware path, so they keep the default link-up state.
     */
    bool firmware_path = !MACHINE(s)->kernel_filename && !qtest_enabled();

    s->pcie3x4 = rk3588_create_pcie_host(
        s, "pcie3x4", "pcie3x4",
        rk3588_memmap[RK3588_PCIE3X4_DBI].base,
        rk3588_memmap[RK3588_PCIE3X4_APB].base,
        0, 0, s->board->pcie3x4_link_down || firmware_path, pcie3x4_spis);

    if (s->board->pcie3x2_num_lanes) {
        s->pcie3x2 = rk3588_create_pcie_host(
            s, "pcie3x2", "pcie3x2",
            rk3588_memmap[RK3588_PCIE3X2_DBI].base,
            rk3588_memmap[RK3588_PCIE3X2_APB].base,
            1, 0x10, s->board->pcie3x2_link_down || firmware_path,
            pcie3x2_spis);
    }
}

static void rk3588_create_cru(RK3588MachineState *s)
{
    SysBusDevice *sbd;

    s->cru = qdev_new(TYPE_RK3588_CRU);
    object_property_add_child(OBJECT(s), "cru", OBJECT(s->cru));
    sbd = SYS_BUS_DEVICE(s->cru);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_CRU_MEM].base);
}

static void rk3588_create_stimer(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_ROCKCHIP_STIMER);
    SysBusDevice *sbd;

    object_property_add_child(OBJECT(s), "stimer", OBJECT(dev));
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_STIMER].base);
}

static void rk3588_create_ddr(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_DDR);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), "ddr", OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);
    for (unsigned int i = 0; i < RK3588_DDR_LEGACY_CHANNEL_COUNT; i++) {
        hwaddr base = RK3588_DDR_LEGACY_BASE +
                      i * RK3588_DDR_LEGACY_CHANNEL_STRIDE;

        for (unsigned int j = 0;
             j < RK3588_DDR_LEGACY_CTRL_WINDOWS_PER_CHANNEL; j++) {
            sysbus_mmio_map(sbd, RK3588_DDR_MMIO_LEGACY(i, j),
                            base + j * RK3588_DDR_LEGACY_WINDOW_STRIDE);
        }
        sysbus_mmio_map(sbd, RK3588_DDR_MMIO_LEGACY_GATE(i),
                        RK3588_DDR_PHY_GATE_BASE +
                        i * RK3588_DDR_LEGACY_WINDOW_STRIDE);
    }
    sysbus_mmio_map(sbd, RK3588_DDR_MMIO_LEGACY_PHY,
                    RK3588_DDR_LEGACY_PHY_BASE);
    sysbus_mmio_map(sbd, RK3588_DDR_MMIO_LEGACY_PHY_AUX,
                    RK3588_DDR_LEGACY_PHY_AUX_BASE);
    sysbus_mmio_map(sbd, RK3588_DDR_MMIO_GLOBAL,
                    RK3588_DDR_GLOBAL_BASE);
    for (unsigned int i = 0; i < RK3588_DDR_CHANNEL_COUNT; i++) {
        sysbus_mmio_map(sbd, RK3588_DDR_MMIO_CHANNEL(i),
                        RK3588_DDR_CHANNEL_BASE +
                        i * RK3588_DDR_CHANNEL_MMIO_STRIDE);
    }
    sysbus_mmio_map(sbd, RK3588_DDR_MMIO_DDRPHY, RK3588_DDRPHY_BASE);
    s->ddr = RK3588_DDR(dev);
    object_unref(OBJECT(dev));
}

static void rk3588_create_usb2_host(RK3588MachineState *s)
{
    static const hwaddr bases[RK3588_USB2_HOST_MMIO_COUNT] = {
        [RK3588_USB2_HOST_EHCI0] = RK3588_USB2_HOST0_EHCI_BASE,
        [RK3588_USB2_HOST_OHCI0] = RK3588_USB2_HOST0_OHCI_BASE,
        [RK3588_USB2_HOST_EHCI1] = RK3588_USB2_HOST1_EHCI_BASE,
        [RK3588_USB2_HOST_OHCI1] = RK3588_USB2_HOST1_OHCI_BASE,
    };
    MachineState *machine = MACHINE(s);
    DeviceState *dev = qdev_new(TYPE_RK3588_USB2_HOST);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), "usb2-host", OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);
    for (unsigned int i = 0; i < ARRAY_SIZE(bases); i++) {
        sysbus_mmio_map(sbd, i, bases[i]);
    }

    s->usb2_host = RK3588_USB2_HOST(dev);
    rk3588_usb2_host_set_active(s->usb2_host,
                                qtest_enabled() || machine->kernel_filename);
    object_unref(OBJECT(dev));
}

static void rk3588_create_usb3_device(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_DWC3_UDC);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    if (s->zephyr_ram) {
        qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    }
    object_property_add_child(OBJECT(s), "usb3otg0", OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_USB3OTG0].base);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(s->gic, RK3588_USB3OTG0_SPI));

    s->dwc3_udc = RK3588_DWC3_UDC(dev);
    object_unref(OBJECT(dev));
}

/*
 * Per-machine SMC handler entry. Registered with
 * arm_register_psci_smc_handler() so accelerator SMC exception paths can run it
 * before the generic PSCI switch or architectural SMC exception entry.
 * Consumes only the SCMI SMC (function-id 0x82000010); all other SMCs fall
 * through to standard PSCI handling (CPU_ON/OFF/SYSTEM_RESET/...).
 *
 * The shmem-backed responder lives in the rk3588-scmi device realized by
 * rk3588_create_scmi().  Resolve the active machine through QOM so multiple
 * machine instances do not share file-scope device state.
 */
static bool rk3588_smc_handler(ARMCPU *cpu)
{
    RK3588MachineState *s = RK3588_MACHINE(qdev_get_machine());
    CPUARMState *env = &cpu->env;
    uint64_t fn = is_a64(env) ? env->xregs[0] : env->regs[0];

    if ((uint32_t)fn == RK3588_BROM_SMC_NEXT_STAGE) {
        rk3588_bootrom_load_spl(s, cpu);
        if (is_a64(env)) {
            env->xregs[0] = 0;
        } else {
            env->regs[0] = 0;
        }
        return true;
    }

    if ((uint32_t)fn == RK3588_QEMU_SMC_UBOOT_HANDOFF) {
        rk3588_firmware_handoff_to_uboot(s, cpu);
        return true;
    }

    if ((uint32_t)fn == RK3588_QEMU_SMC_ATF_ENTRY) {
        uint64_t bl31_params = is_a64(env) ? env->xregs[2] : env->regs[2];

        rk3588_patch_bl31_runtime(s);
        s->firmware_atf_entered = true;
        cpu_set_pc(CPU(cpu), RK3588_BL31_BASE);
        if (is_a64(env)) {
            env->xregs[0] = bl31_params;
            env->xregs[1] = 0;
            env->xregs[2] = 0;
            env->xregs[3] = 0;
        } else {
            env->regs[0] = bl31_params;
            env->regs[1] = 0;
            env->regs[2] = 0;
            env->regs[3] = 0;
        }
        arm_rebuild_hflags(env);
        return true;
    }

    if ((uint32_t)fn == RK3588_QEMU_SMC_BL31_EXIT) {
        rk3588_firmware_handoff_to_uboot(s, cpu);
        return true;
    }

    /*
     * First kernel PSCI probe: bring the PCIe links up now that U-Boot
     * has handed off (U-Boot itself never issues PSCI_VERSION).
     */
    if ((uint32_t)fn == 0x84000000) { /* PSCI_VERSION */
        rk3588_pcie_set_links_up(s);
        s->kernel_started = true;
        return false;
    }

    /*
     * Rockchip SIP services for the vendor FIQ debugger.  The guest
     * console (ttyFIQ0) drives UART2 directly once probed; these SMCs
     * only need to report success and hand out a shared page.  The
     * debugger's FIQ itself is never raised in the model, so the shared
     * page stays untouched by firmware.
     */
    if ((uint32_t)fn == RK3588_SIP_SHARE_MEM) {
        qemu_log_mask(LOG_GUEST_ERROR, "rk3588: SIP_SHARE_MEM called\n");
        env->xregs[0] = RK3588_SIP_RET_SUCCESS;
        env->xregs[1] = RK3588_FIQ_SHARE_MEM_ADDR;
        return true;
    }
    if ((uint32_t)fn == RK3588_SIP_UARTDBG_CFG64) {
        uint64_t sub = is_a64(env) ? env->xregs[3] : env->regs[3];

        qemu_log_mask(LOG_GUEST_ERROR, "rk3588: SIP_UARTDBG sub=%" PRIu64 "\n",
                      sub);

        switch (sub) {
        case RK3588_UARTDBG_CFG_INIT:
            /* a0 = status, a1 = shared page for the CPU context. */
            env->xregs[0] = RK3588_SIP_RET_SUCCESS;
            env->xregs[1] = RK3588_FIQ_SHARE_MEM_ADDR;
            break;
        case RK3588_UARTDBG_CFG_PRINT_PORT:
        case RK3588_UARTDBG_CFG_OSHDL_TO_OS:
        case RK3588_UARTDBG_CFG_CPUSW:
        case RK3588_UARTDBG_CFG_DEBUG_ENABLE:
        case RK3588_UARTDBG_CFG_DEBUG_DISABLE:
        case RK3588_UARTDBG_CFG_FIQ_ENABLE:
        case RK3588_UARTDBG_CFG_FIQ_DISABLE:
            env->xregs[0] = RK3588_SIP_RET_SUCCESS;
            break;
        default:
            env->xregs[0] = -2; /* SIP_RET_NOT_SUPPORTED */
            break;
        }
        return true;
    }

    /*
     * First kernel PSCI probe: bring the PCIe links up now that U-Boot
     * has handed off (U-Boot itself never issues PSCI_VERSION).
     */
    if ((uint32_t)fn == 0x84000000) { /* PSCI_VERSION */
        rk3588_pcie_set_links_up(s);
        s->kernel_started = true;
        return false;
    }

    if ((uint32_t)fn != RK3588_SCMI_SMC_ID) {
        return false;
    }
    if (!s->scmi) {
        /* Responder not yet realized - return NOT_SUPPORTED. */
        if (is_a64(env)) {
            env->xregs[0] = (uint64_t)(int64_t)-1;
        } else {
            env->regs[0] = (uint32_t)(int64_t)-1;
        }
        return true;
    }

    rk3588_scmi_handle_smc(RK3588_SCMI(s->scmi));
    /* a0 = 0 means "response is in shmem, fetch it". */
    if (is_a64(env)) {
        env->xregs[0] = 0;
    } else {
        env->regs[0] = 0;
    }
    return true;
}

static void rk3588_create_scmi(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_SCMI);
    SysBusDevice *sbd;

    object_property_add_child(OBJECT(s), "scmi", OBJECT(dev));
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_SCMI_SHMEM].base);

    s->scmi = dev;
    arm_register_psci_smc_handler(rk3588_smc_handler);
}

static void rk3588_create_sdmmc(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_DW_MMC);
    SysBusDevice *sbd;

    object_property_add_child(OBJECT(s), "sdmmc", OBJECT(dev));
    sbd = SYS_BUS_DEVICE(dev);
    sysbus_realize(sbd, &error_fatal);
    /*
     * sysbus mmio[0] is the core bank through the legacy FIFO data port at
     * +0x200; mmio[1] is the remaining RAZ/WI tail (RK vendor + the rest of
     * the window). Both are placed at the same base - they're contiguous
     * sub-windows of the 0x4000 controller MMIO.
     */
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_SDMMC].base);
    sysbus_mmio_map(sbd, 1, rk3588_memmap[RK3588_SDMMC].base + 0x204);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, RK3588_SDMMC_SPI));

    rk3588_attach_sd_card(s, dev);
}

/*
 * Rockchip SFC with the board SPI NOR flash.  The flash is the 16 MiB
 * XTX XT25F128 part used by the Radxa ROCK 5B+ (jedec,spi-nor class); a
 * backing store can be supplied with -drive if=mtd,index=0,file=...,format=raw.
 */
static void rk3588_create_sfc(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_ROCKCHIP_SFC);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DeviceState *flash;
    DriveInfo *di = drive_get(IF_MTD, 0, 0);
    qemu_irq flash_cs;

    object_property_add_child(OBJECT(s), "sfc", OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_SFC].base);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, RK3588_SFC_SPI));

    flash = qdev_new("xt25f128");
    if (di) {
        qdev_prop_set_drive(flash, "drive", blk_by_legacy_dinfo(di));
    }
    qdev_realize_and_unref(flash, BUS(ROCKCHIP_SFC(dev)->spi), &error_fatal);
    flash_cs = qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0);
    qdev_connect_gpio_out_named(dev, "cs", 0, flash_cs);

    s->sfc = dev;
    object_unref(OBJECT(dev));
}

static void rk3588_create_syscon_devices(RK3588MachineState *s)
{
    DeviceState *grf = qdev_new(TYPE_RK3588_GRF);
    SysBusDevice *grf_sbd = SYS_BUS_DEVICE(grf);

    qdev_prop_set_uint64(grf, "ram-size", MACHINE(s)->ram_size);
    qdev_prop_set_uint32(grf, "dram-type", s->board->dram_type);
    object_property_add_child(OBJECT(s), "pmu-grf", OBJECT(grf));
    sysbus_realize(grf_sbd, &error_fatal);
    sysbus_mmio_map(grf_sbd, RK3588_GRF_MMIO_PMU0,
                    rk3588_memmap[RK3588_PMU0_GRF].base);
    sysbus_mmio_map(grf_sbd, RK3588_GRF_MMIO_PMU1,
                    rk3588_memmap[RK3588_PMU1_GRF].base);
    sysbus_mmio_map(grf_sbd, RK3588_GRF_MMIO_SYS,
                    rk3588_memmap[RK3588_SYS_GRF].base);
    object_unref(OBJECT(grf));

    rk3588_create_syscon(s, "php-grf", RK3588_PHP_GRF);
    rk3588_create_syscon(s, "usb-grf", RK3588_USB_GRF);
    rk3588_create_syscon(s, "pmu1-ioc", RK3588_PMU1_IOC);
    rk3588_create_syscon(s, "pmu2-ioc", RK3588_PMU2_IOC);
    rk3588_create_syscon(s, "bus-ioc", RK3588_BUS_IOC);
    rk3588_create_syscon(s, "firewall-ddr", RK3588_FIREWALL_DDR);
    rk3588_create_syscon(s, "firewall-sysmem", RK3588_FIREWALL_SYSMEM);

    rk3588_create_syscon(s, "combphy0", RK3588_COMBPHY0);
    rk3588_create_syscon(s, "combphy1", RK3588_COMBPHY1);
    rk3588_create_syscon(s, "combphy2", RK3588_COMBPHY2);
    rk3588_create_syscon(s, "pcie30phy", RK3588_PCIE30PHY);
    rk3588_create_syscon(s, "pipe-phy-grf0", RK3588_PIPE_PHY_GRF0);
    rk3588_create_syscon(s, "pipe-phy-grf1", RK3588_PIPE_PHY_GRF1);
    rk3588_create_syscon(s, "pipe-phy-grf2", RK3588_PIPE_PHY_GRF2);
}

static void rk3588_create_crypto(RK3588MachineState *s)
{
    const RK3588FirmwareProfile *profile = s->board->firmware_profile;
    SysBusDevice *sbd;

    if (!profile || !profile->crypto_v2_sha256) {
        return;
    }

    s->crypto = qdev_new(TYPE_ROCKCHIP_CRYPTO_V2);
    object_property_add_child(OBJECT(s), "crypto", OBJECT(s->crypto));
    sbd = SYS_BUS_DEVICE(s->crypto);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_CRYPTO].base);
}

/*
 * TSADC thermal sensor: fixed room-temperature ADC codes so the Linux
 * thermal framework boots normally (the critical-trip shutdown path is
 * only reachable when the sensor reports an out-of-range code).
 */
static void rk3588_create_tsadc(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_TSADC);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), "tsadc", OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_TSADC_MMIO].base);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, RK3588_TSADC_SPI));
    s->tsadc = dev;
    object_unref(OBJECT(dev));
}

/*
 * SPI2 with the RK806 PMIC on chip-select 0.  The PMIC supplies the
 * board regulators (vcc_3v3_s3 for the SD controller, etc.); the model
 * exposes them enabled at their nominal voltages so the MMC and PCIe
 * driver probe chains do not defer forever.
 */
static void rk3588_create_spi2(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_ROCKCHIP_SPI);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DeviceState *pmic;

    object_property_add_child(OBJECT(s), "spi2", OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_SPI2].base);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, RK3588_SPI2_SPI));

    pmic = qdev_new(TYPE_RK806);
    qdev_realize_and_unref(pmic, BUS(ROCKCHIP_SPI(dev)->spi), &error_fatal);

    s->spi2 = dev;
    object_unref(OBJECT(dev));
}

static void rk3588_create_i2c6(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_RK3X_I2C);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DeviceState *rtc;
    I2CBus *bus;

    object_property_add_child(OBJECT(s), "i2c6", OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_I2C6].base);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, RK3588_I2C6_SPI));

    bus = I2C_BUS(qdev_get_child_bus(dev, "i2c-bus"));
    rtc = DEVICE(i2c_slave_create_simple(bus, TYPE_HYM8563, 0x51));
    /*
     * The RTC INT output is open-drain active-low, wired to GPIO0
     * bank B pin 0 (RK_PB0).  The driver keeps the alarm and timer
     * interrupts disabled, so the line idles high.
     */
    qdev_connect_gpio_out_named(rtc, "irq", 0,
                                qdev_get_gpio_in(s->gpio[0], 8));

    s->i2c6 = dev;
    object_unref(OBJECT(dev));
}

/* TRNGv1: seeds the kernel CRNG so getrandom() does not block. */
static void rk3588_create_rng(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_RNG);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), "rng", OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_RNG_MMIO].base);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, RK3588_RNG_SPI));
    object_unref(OBJECT(dev));
}

/*
 * DMAC1 stub: lets the kernel's pl330 driver bind and register the DT
 * DMA provider so spi-rockchip's dma_request_chan() does not defer.
 */
static void rk3588_create_dmac1(RK3588MachineState *s)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_PL330);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    object_property_add_child(OBJECT(s), "dmac1", OBJECT(dev));
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, rk3588_memmap[RK3588_DMAC1].base);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(s->gic, RK3588_DMAC1_SPI));
    sysbus_connect_irq(sbd, 1,
                       qdev_get_gpio_in(s->gic, RK3588_DMAC1_SPI + 1));
    s->dmac1 = dev;
    object_unref(OBJECT(dev));
}

static void rk3588_create_secure_otp(RK3588MachineState *s)
{
    const RK3588FirmwareProfile *profile = s->board->firmware_profile;
    SysBusDevice *sbd;

    if (!profile || !profile->unfused_secure_otp) {
        return;
    }

    s->secure_otp = qdev_new(TYPE_RK3588_SECURE_OTP);
    object_property_add_child(OBJECT(s), "secure-otp",
                              OBJECT(s->secure_otp));
    sbd = SYS_BUS_DEVICE(s->secure_otp);
    sysbus_realize(sbd, &error_fatal);
    sysbus_mmio_map(sbd, 0, RK3588_SECURE_OTP_BASE);
    object_unref(OBJECT(s->secure_otp));
}

static void rk3588_init(MachineState *machine)
{
    RK3588MachineState *s = RK3588_MACHINE(machine);
    const RK3588BoardConfig *board = s->board;
    MemoryRegion *sysmem = get_system_memory();
    hwaddr ram_base = rk3588_ram_base(s);
    hwaddr low_ram_size = RK3588_LOW_RAM_END - ram_base;

    if (machine->smp.cpus > RK3588_MAX_CPUS ||
        machine->smp.max_cpus > RK3588_MAX_CPUS) {
        error_report("%s: at most %u CPUs are supported",
                     board->machine_name, RK3588_MAX_CPUS);
        exit(EXIT_FAILURE);
    }

    if (machine->ram_size > RK3588_MAX_RAM_SIZE) {
        g_autofree char *sz = size_to_str(RK3588_MAX_RAM_SIZE);
        error_report("%s: RAM size must not exceed %s",
                     board->machine_name, sz);
        exit(EXIT_FAILURE);
    }
    /*
     * The first 4 GiB window is shared with the peripheral MMIO, so
     * RAM above the low window is mapped in the high window starting
     * at 4 GiB (as on real RK3588).  The generic -kernel loader can
     * only describe a single memory bank, so direct kernel boots stay
     * within the low window; the firmware path describes both banks
     * through the DDR ATAGS.
     */
    if (machine->kernel_filename && machine->ram_size > low_ram_size) {
        g_autofree char *sz = size_to_str(low_ram_size);
        error_report("%s: -kernel boot supports at most %s of RAM",
                     board->machine_name, sz);
        exit(EXIT_FAILURE);
    }

    rk3588_create_cpus(s);
    rk3588_create_low_memory(s);
    rk3588_create_atf_ddr(s);
    rk3588_create_firmware_mmio(s);
    memory_region_init_alias(&s->ram_low, OBJECT(s), "rk3588.ram-low",
                             machine->ram, 0, low_ram_size);
    memory_region_add_subregion(sysmem, ram_base, &s->ram_low);
    if (machine->ram_size > low_ram_size) {
        memory_region_init_alias(&s->ram_high, OBJECT(s), "rk3588.ram-high",
                                 machine->ram, low_ram_size,
                                 machine->ram_size - low_ram_size);
        memory_region_add_subregion(sysmem, RK3588_HIGH_RAM_BASE,
                                    &s->ram_high);
    }
    rk3588_create_zvm_ram(s);

    rk3588_create_gic(s);
    rk3588_create_its(s);
    rk3588_create_syscon_devices(s);
    rk3588_create_cru(s);
    rk3588_create_stimer(s);
    rk3588_create_ddr(s);
    rk3588_create_usb3_device(s);
    rk3588_create_usb2_host(s);
    rk3588_create_scmi(s);
    rk3588_create_secure_otp(s);
    rk3588_create_crypto(s);
    rk3588_create_tsadc(s);
    rk3588_create_uarts(s);
    rk3588_create_sdhci(s);
    rk3588_create_sdmmc(s);
    rk3588_create_sfc(s);
    rk3588_create_gpio(s);
    rk3588_create_dmac1(s);
    rk3588_create_spi2(s);
    rk3588_create_i2c6(s);
    rk3588_create_rng(s);
    rk3588_create_gmac(s);
    rk3588_create_rknpu(s);
    rk3588_create_pcie(s);
    rk3588_create_pcie2x1(s);

    s->bootinfo = (struct arm_boot_info) {
        .loader_start = ram_base,
        .board_id = -1,
        .ram_size = machine->ram_size,
        .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
        .get_dtb = rk3588_get_dtb,
    };

    rk3588_enable_psci_conduit(s);

    if (qtest_enabled() && !machine->kernel_filename) {
        return;
    }

    if (machine->kernel_filename) {
        /*
         * arm/boot.c only invokes the ARMLinuxBootIf handoff for raw
         * Linux images; ELF kernels skip it, leaving a TZ-aware GIC with
         * all interrupts in Group 0 that NS guests can neither reassign
         * nor enable. Zephyr-ram direct boot is known to run NonSecure,
         * so run the same handoff for it: the GIC resets with all
         * interrupts in NonSecure Group 1, as secure firmware would have
         * configured it on real hardware.
         *
         * Other ELF payloads (e.g. U-Boot or BL31) boot at secure EL3
         * and may rely on or test the hardware Group 0 reset state, so
         * leave the GIC untouched for them.
         */
        if (s->zephyr_ram) {
            ARMLinuxBootIf *albif = ARM_LINUX_BOOT_IF(s->gic);
            ARMLinuxBootIfClass *albifc = ARM_LINUX_BOOT_IF_GET_CLASS(albif);

            if (albifc->arm_linux_init) {
                albifc->arm_linux_init(albif, false);
            }
        }
        arm_load_kernel(s->cpu[0], machine, &s->bootinfo);
    } else {
        Error *local_err = NULL;

        if (!rk3588_bootrom_prepare(s, &local_err)) {
            error_report_err(local_err);
            exit(EXIT_FAILURE);
        }
        s->firmware_boot = true;
        rk3588_schedule_firmware_patch(s);
        rk3588_register_firmware_reset(s);
        if (s->firmware_bootargs && s->firmware_bootargs[0]) {
            s->bootargs_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                             rk3588_firmware_bootargs_tick,
                                             s);
        }
    }
}

static void rk3588_machine_reset(MachineState *machine, ResetType type)
{
    qemu_devices_reset(type);
    rk3588_boot_state_reset(RK3588_MACHINE(machine));
}

static const CPUArchIdList *rk3588_possible_cpu_arch_ids(MachineState *ms)
{
    unsigned int max_cpus = ms->smp.max_cpus;

    if (ms->possible_cpus) {
        assert(ms->possible_cpus->len == max_cpus);
        return ms->possible_cpus;
    }

    ms->possible_cpus = g_malloc0(sizeof(CPUArchIdList) +
                                  sizeof(CPUArchId) * max_cpus);
    ms->possible_cpus->len = max_cpus;

    for (unsigned int n = 0; n < max_cpus; n++) {
        ms->possible_cpus->cpus[n].type = rk3588_cpu_type(n);
        ms->possible_cpus->cpus[n].arch_id = rk3588_cpu_mpidr[n];
        ms->possible_cpus->cpus[n].props.has_core_id = true;
        ms->possible_cpus->cpus[n].props.core_id = n;
    }

    return ms->possible_cpus;
}

static CpuInstanceProperties
rk3588_cpu_index_to_props(MachineState *ms, unsigned cpu_index)
{
    MachineClass *mc = MACHINE_GET_CLASS(ms);
    const CPUArchIdList *possible_cpus = mc->possible_cpu_arch_ids(ms);

    assert(cpu_index < possible_cpus->len);
    return possible_cpus->cpus[cpu_index].props;
}

static void rk3588_set_firmware_bootargs(Object *obj, const char *value,
                                        Error **errp)
{
    RK3588MachineState *s = RK3588_MACHINE(obj);

    g_free(s->firmware_bootargs);
    s->firmware_bootargs = g_strdup(value);
}

static bool rk3588_get_zvm_ram(Object *obj, Error **errp)
{
    RK3588MachineState *s = RK3588_MACHINE(obj);

    return s->zvm_ram;
}

static void rk3588_set_zvm_ram(Object *obj, bool value, Error **errp)
{
    RK3588MachineState *s = RK3588_MACHINE(obj);

    s->zvm_ram = value;
}

static bool rk3588_get_rknpu(Object *obj, Error **errp)
{
    RK3588MachineState *s = RK3588_MACHINE(obj);

    return s->rknpu;
}

static void rk3588_set_rknpu(Object *obj, bool value, Error **errp)
{
    RK3588MachineState *s = RK3588_MACHINE(obj);

    s->rknpu = value;
}

static bool rk3588_get_zephyr_ram(Object *obj, Error **errp)
{
    RK3588MachineState *s = RK3588_MACHINE(obj);

    return s->zephyr_ram;
}

static void rk3588_set_zephyr_ram(Object *obj, bool value, Error **errp)
{
    RK3588MachineState *s = RK3588_MACHINE(obj);

    s->zephyr_ram = value;
}

void rk3588_machine_instance_configure(Object *obj,
                                       const RK3588BoardConfig *board)
{
    RK3588MachineState *s = RK3588_MACHINE(obj);

    assert(board);
    assert(board->dram_type && board->dram_type <= 0xf);
    assert(!(board->gmac_mask & ~(BIT(0) | BIT(1))));
    assert(board->pcie3x4_num_lanes == 1 ||
           board->pcie3x4_num_lanes == 2 ||
           board->pcie3x4_num_lanes == 4);
    assert(board->pcie3x2_num_lanes == 0 ||
           board->pcie3x2_num_lanes == 1 ||
           board->pcie3x2_num_lanes == 2);
    assert(!(board->pcie2x1_mask & ~(BIT(0) | BIT(2))));
    s->board = board;
    s->zvm_ram = board->default_zvm_ram;
    s->rknpu = false;
}

void rk3588_machine_class_configure(ObjectClass *oc,
                                    const RK3588BoardConfig *board)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = board->desc;
    mc->init = rk3588_init;
    mc->reset = rk3588_machine_reset;
    mc->max_cpus = RK3588_MAX_CPUS;
    mc->default_cpus = RK3588_MAX_CPUS;
    mc->default_ram_size = board->default_ram_size ?
                        board->default_ram_size : 2 * GiB;
    mc->default_ram_id = board->ram_id;
    mc->possible_cpu_arch_ids = rk3588_possible_cpu_arch_ids;
    mc->cpu_index_to_instance_props = rk3588_cpu_index_to_props;

    object_class_property_add_bool(oc, "zvm-ram", rk3588_get_zvm_ram,
                                   rk3588_set_zvm_ram);
    object_class_property_set_description(oc, "zvm-ram",
                                          "Map RK3588 ZVM fixed guest and "
                                          "shared RAM windows");
    object_class_property_add_bool(oc, "rknpu", rk3588_get_rknpu,
                                   rk3588_set_rknpu);
    object_class_property_set_description(oc, "rknpu",
                                          "Enable RK3588 RKNN/RKNPU "
                                          "accelerator cores");

    object_class_property_add_bool(oc, "zephyr-ram", rk3588_get_zephyr_ram,
                                   rk3588_set_zephyr_ram);
    object_class_property_set_description(oc, "zephyr-ram",
                                          "Place direct-kernel RAM at "
                                          "0x10000000 for Zephyr board images");

    object_class_property_add_str(oc, "firmware-bootargs",
                                  NULL, rk3588_set_firmware_bootargs);
    object_class_property_set_description(oc, "firmware-bootargs",
                                          "Extra kernel arguments appended "
                                          "to /chosen/bootargs in the DTB "
                                          "loaded by the firmware path");
}

static void rk3588_machine_finalize(Object *obj)
{
    RK3588MachineState *s = RK3588_MACHINE(obj);

    timer_free(s->bootargs_timer);
    g_free(s->firmware_bootargs);
}

static const TypeInfo rk3588_machine_typeinfo = {
    .name = TYPE_RK3588_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(RK3588MachineState),
    .instance_finalize = rk3588_machine_finalize,
    .abstract = true,
    .interfaces = aarch64_machine_interfaces,
};

static void rk3588_machine_init_register_types(void)
{
    type_register_static(&rk3588_machine_typeinfo);
}

type_init(rk3588_machine_init_register_types)
