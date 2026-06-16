/*
 * QEMU RISC-V Virt Board Compatible with kendryte K230 SDK
 *
 * Copyright (c) 2026 Process Mission
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Provides a board compatible with the kendryte K230 SDK
 *
 * K230 Technical Reference Manual V0.3.1 (2024-11-18):
 * https://github.com/revyos/external-docs/blob/master/K230/en-us/K230_Technical_Reference_Manual_V0.3.1_20241118.pdf
 *
 * For more information, see <https://www.kendryte.com/en/proDetail/230>
 */
#ifndef HW_K230_H
#define HW_K230_H

#include "hw/core/boards.h"
#include "hw/core/split-irq.h"
#include "hw/display/k230_display.h"
#include "hw/dma/k230_gsdma.h"
#include "hw/dma/k230_pdma.h"
#include "hw/i2c/k230_i2c.h"
#include "hw/misc/k230_adc.h"
#include "hw/misc/k230_dewarp.h"
#include "hw/misc/k230_gpio.h"
#include "hw/misc/k230_hardlock.h"
#include "hw/misc/k230_hi_sys_cfg.h"
#include "hw/misc/k230_iomux.h"
#include "hw/misc/k230_i2s.h"
#include "hw/misc/k230_isp.h"
#include "hw/misc/k230_kpu.h"
#include "hw/misc/k230_nonai_2d.h"
#include "hw/misc/k230_pmu.h"
#include "hw/misc/k230_pwm.h"
#include "hw/misc/k230_regs.h"
#include "hw/misc/k230_rx_csi.h"
#include "hw/misc/k230_security.h"
#include "hw/misc/k230_sysctl.h"
#include "hw/misc/k230_timer.h"
#include "hw/misc/k230_tsensor.h"
#include "hw/misc/k230_ugzip.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/rtc/k230_rtc.h"
#include "hw/sd/k230_sdhci.h"
#include "hw/ssi/k230_spi.h"
#include "hw/usb/hcd-dwc2.h"
#include "hw/watchdog/k230_wdt.h"

#define C908_CPU_HARTID   (0)
#define C908V_CPU_HARTID  (0)
#define C908V_CPU_INDEX   (1)
#define K230_UART_COUNT   5
#define K230_REGS_COUNT   12
#define K230_PLIC_NUM_SOURCES 257

#define TYPE_RISCV_K230_SOC "riscv.k230.soc"
#define RISCV_K230_SOC(obj) \
    OBJECT_CHECK(K230SoCState, (obj), TYPE_RISCV_K230_SOC)

typedef struct K230SoCState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    RISCVHartArrayState c908_cpu; /* Small core */
    RISCVHartArrayState c908v_cpu; /* Big core */

    K230WdtState wdt[2];
    K230SdhciState sdhci[K230_SDHCI_COUNT];
    K230GsdmaState gsdma;
    K230PdmaState pdma;
    K230UgzipState ugzip;
    K230HiSysCfgState hi_sys_cfg;
    K230HardlockState hardlock;
    K230TSensorState tsensor;
    K230GpioState gpio[2];
    K230IomuxState iomux;
    K230RegsState uart_ext[K230_UART_COUNT];
    K230I2CState i2c[5];
    K230AdcState adc;
    K230PwmState pwm;
    K230TimerState timer;
    K230SysctlBootState sysctl_boot;
    K230SysctlPowerState sysctl_power;
    K230SysctlResetState sysctl_reset;
    K230PmuState pmu;
    K230RtcState rtc;
    K230SecurityState security;
    K230VoState vo;
    K230DsiState dsi;
    K230SpiState spi[3];
    K230I2SState i2s;
    K230RegsState regs[K230_REGS_COUNT];
    K230KpuState kpu;
    K230NonAI2DState nonai_2d;
    K230IspState isp;
    K230DewarpState dewarp;
    K230RxCsiState rx_csi;
    DWC2State usb[2];
    MemoryRegion sram;
    MemoryRegion kpu_l2_cache;
    MemoryRegion bootrom;
    MemoryRegion flash_xip;
    MemoryRegion c908v_mem;
    MemoryRegion c908v_sysmem;

    DeviceState *c908_plic;
    DeviceState *c908v_plic;
    SplitIRQ plic_irq_splitter[K230_PLIC_NUM_SOURCES];
    bool c908v_enabled;
} K230SoCState;

#define TYPE_RISCV_K230_MACHINE MACHINE_TYPE_NAME("k230-canmv")
#define RISCV_K230_MACHINE(obj) \
    OBJECT_CHECK(K230MachineState, (obj), TYPE_RISCV_K230_MACHINE)

typedef struct K230MachineState {
    /*< private >*/
    MachineState parent_obj;

    /*< public >*/
    K230SoCState soc;
    Notifier machine_done;
    bool boot_both_cores;
} K230MachineState;

enum {
    K230_DEV_DDRC,
    K230_DEV_KPU_L2_CACHE,
    K230_DEV_SRAM,
    K230_DEV_KPU_CFG,
    K230_DEV_FFT,
    K230_DEV_AI_2D_ENGINE,
    K230_DEV_GSDMA,
    K230_DEV_DMA,
    K230_DEV_DECOMP_GZIP,
    K230_DEV_NON_AI_2D,
    K230_DEV_ISP,
    K230_DEV_DEWARP,
    K230_DEV_RX_CSI,
    K230_DEV_H264,
    K230_DEV_2P5D,
    K230_DEV_VO,
    K230_DEV_VO_CFG,
    K230_DEV_3D_ENGINE,
    K230_DEV_PMU,
    K230_DEV_RTC,
    K230_DEV_CMU,
    K230_DEV_RMU,
    K230_DEV_BOOT,
    K230_DEV_PWR,
    K230_DEV_MAILBOX,
    K230_DEV_IOMUX,
    K230_DEV_TIMER,
    K230_DEV_WDT0,
    K230_DEV_WDT1,
    K230_DEV_TS,
    K230_DEV_HDI,
    K230_DEV_STC,
    K230_DEV_BOOTROM,
    K230_DEV_SECURITY,
    K230_DEV_UART0,
    K230_DEV_UART1,
    K230_DEV_UART2,
    K230_DEV_UART3,
    K230_DEV_UART4,
    K230_DEV_I2C0,
    K230_DEV_I2C1,
    K230_DEV_I2C2,
    K230_DEV_I2C3,
    K230_DEV_I2C4,
    K230_DEV_PWM,
    K230_DEV_GPIO0,
    K230_DEV_GPIO1,
    K230_DEV_ADC,
    K230_DEV_CODEC,
    K230_DEV_I2S,
    K230_DEV_USB0,
    K230_DEV_USB1,
    K230_DEV_SD0,
    K230_DEV_SD1,
    K230_DEV_QSPI0,
    K230_DEV_QSPI1,
    K230_DEV_SPI,
    K230_DEV_HI_SYS_CFG,
    K230_DEV_DDRC_CFG,
    K230_DEV_FLASH,
    K230_DEV_PLIC,
    K230_DEV_CLINT,
};

enum {
    /*
     * K230 TRM v0.3.1 section 2.4 lists peripheral interrupt bits; SDK
     * DTBs expose the corresponding PLIC IDs as bit + 16.
     */
    K230_UART0_IRQ  = 16,
    K230_UART1_IRQ  = 17,
    K230_UART2_IRQ  = 18,
    K230_UART3_IRQ  = 19,
    K230_UART4_IRQ  = 20,
    K230_I2C0_IRQ   = 21,
    K230_GPIO0_IRQ  = 32,
    K230_WDT0_IRQ   = 107,
    K230_WDT1_IRQ   = 108,
    K230_IPCM_IRQ_BASE = 109,
    K230_IPCM_IRQ_COUNT = 4,
    K230_ISP_MI_IRQ = 127,
    K230_ISP_FE_IRQ = 128,
    K230_ISP_IRQ    = 129,
    K230_DWE_IRQ    = 130,
    K230_FE_IRQ     = 131,
    K230_VO_IRQ     = 133,
    K230_DMA_IRQ    = 140,
    K230_PDMA_IRQ   = 203,
    K230_NON_AI_2D_IRQ = 141,
    K230_SD0_IRQ    = 142,
    K230_SD1_IRQ    = 144,
    K230_SPI_IRQ    = 146,
    K230_QSPI0_IRQ  = 155,
    K230_QSPI1_IRQ  = 164,
    K230_USB0_IRQ   = 173,
    K230_USB1_IRQ   = 174,
    K230_PMU_IRQ    = 175,
    K230_DPU_IRQ    = 186,
    K230_GNNE_IRQ   = 189,
    K230_FFT_IRQ    = 190,
    K230_AI2D_IRQ   = 191,
    K230_VSE_IRQ    = 204,
};

#define K230_I2C_COUNT 5
#define K230_SPI_COUNT 3

enum {
    K230_SPI_QSPI0,
    K230_SPI_QSPI1,
    K230_SPI_SPI0,
};

enum {
    K230_REGS_CMU,
    K230_REGS_KPU_CFG,
    K230_REGS_HDI,
    K230_REGS_STC,
    K230_REGS_NOC_QOS,
    K230_REGS_CODEC,
    K230_REGS_I2S,
    K230_REGS_DDRC_CFG,
    K230_REGS_FFT,
    K230_REGS_AI_2D_ENGINE,
    K230_REGS_NON_AI_2D,
    K230_REGS_DPU,
};

/*
 * The TRM lists fewer implemented peripheral interrupt lines, but the
 * RT-Smart maix3/c908 BSP defines IRQ_MAX_NR as 256 and initializes
 * PLIC source IDs 1..256.  Keep the PLIC source space wide enough for
 * those SDK accesses while individual devices still only drive the lines
 * they implement.
 */
#define K230_PLIC_NUM_PRIORITIES 7
#define K230_PLIC_PRIORITY_BASE 0x00
#define K230_PLIC_PENDING_BASE 0x1000
#define K230_PLIC_ENABLE_BASE 0x2000
#define K230_PLIC_ENABLE_STRIDE 0x80
#define K230_PLIC_CONTEXT_BASE 0x200000
#define K230_PLIC_CONTEXT_STRIDE 0x1000

#endif
