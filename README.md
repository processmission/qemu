# Process Mission QEMU

<p align="center">
  <strong>Board and SoC model development on top of QEMU</strong><br>
  A downstream tree for new machine models, boot flows, and test coverage.
</p>

<p align="center">
  <a href="https://github.com/processmission/qemu/actions/workflows/build.yml?query=branch%3Adevel">
    <img src="https://github.com/processmission/qemu/actions/workflows/build.yml/badge.svg?branch=devel" alt="Upstream QEMU CI">
  </a>
  <a href="https://github.com/processmission/qemu/tree/devel">
    <img src="https://img.shields.io/badge/branch-devel-4f46e5?logo=git&logoColor=white" alt="devel branch">
  </a>
</p>

Process Mission maintains this downstream QEMU branch for board and SoC models
that are not yet carried by upstream QEMU. The branch is regularly rebased on
top of upstream QEMU as new machine models, boot flows, and tests are added.

> **Looking for something specific?**
> Start with the [machine model list](#machine-models), browse the
> [system emulation documentation](docs/system/index.rst), or open an
> [issue](https://github.com/processmission/qemu/issues) for a new machine
> request or a bug report.

## Quick navigation

[Machine models](#machine-models) ·
[Build and documentation](#build-and-documentation) ·
[Development workflow](#development-workflow)

## Machine models

The table below is the current map of machine models covered by this tree.

| Architecture | Machine | Direct boot | Firmware | Origin |
| --- | --- | :---: | :---: | :---: |
| RISC-V | [`k230-canmv`](docs/system/riscv/k230-canmv.rst) | ✅ | ✅ | PM |
| RISC-V | [`k3-pico-itx`](docs/system/riscv/spacemit-k3.rst) | ✅ | ✅ | PM |
| RISC-V | [`milkv-duo`](docs/system/riscv/milkv-duo.rst) | ✅ | ✅ | UP |
| RISC-V | [`riscv-server-ref`](docs/system/riscv/riscv-server-ref.rst) | ✅ | ✅ | UP |
| ARM | [`ardep-v2`](docs/system/arm/stm32g474.rst) | ✅ | ✅ | PM |
| ARM | [`ax650x-pyramid`](docs/system/arm/ax650x-pyramid.rst) | ✅ | — | PM |
| ARM | [`phytium-pi`](docs/system/arm/phytium-pi.rst) | ✅ | ✅ | PM |
| ARM | [`rk3588-evb`](docs/system/arm/rk3588.rst) | ✅ | ✅ | PM |
| ARM | [`rk3588s-roc-pc`](docs/system/arm/rk3588.rst) | ✅ | ✅ | PM |
| ARM | [`rock-5b-plus`](docs/system/arm/rk3588.rst) | ✅ | ✅ | PM |
| ARM | [`s32k566-cvb-r52`](docs/system/arm/s32k5.rst) | ✅ | — | PM |
| ARM | [`stm32g474`](docs/system/arm/stm32g474.rst) | ✅ | — | PM |
| MCS-51 | [`stc8g1k08a`](docs/system/target-mcs51.rst) | ✅ | ✅ | PM |
| MCS-251 | [`stc32g144k246`](docs/system/target-mcs51.rst) | ✅ | ✅ | PM |

<details>
<summary>Origin legend</summary>

| Mark | Meaning |
| --- | --- |
| `PM` | Process Mission downstream-maintained model |
| `UP` | Imported from upstream QEMU/qemu-devel |
| `OSS` | Imported from another open-source repository |
| `VND` | Imported from vendor sources |

</details>

## Build and documentation

This tree follows the upstream QEMU build and documentation layout. Use the
upstream README for the general build path, then explore the local system and
developer documentation for model-specific details.

- [QEMU build and contribution guide](README.rst)
- [System emulation documentation](docs/system/index.rst)
- [Developer documentation](docs/devel/index.rst)
- [Online QEMU documentation](https://www.qemu.org/docs/master/)

## Development workflow

Machine models in this branch are developed with the
[`oh-my-qemu`](https://github.com/processmission/oh-my-qemu) workflow. It
provides agent skills for planning, register extraction, peripheral modeling,
board modeling, qtest, build, debugging, and verification.
