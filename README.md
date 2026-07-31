# Process Mission QEMU

Process Mission maintains this downstream QEMU branch for board and SoC model
development that is not yet carried by upstream QEMU. It tracks new machine
models, boot flows, and test coverage as they are added.

This branch is regularly rebased on top of upstream QEMU. Please file an issue
for new machine requests or bug reports.

## Machine Models

| arch | machine | direct | firmware | src |
| --- | --- | --- | --- | --- |
| RISC-V | [`k230-canmv`](docs/system/riscv/k230-canmv.rst) | ✅ | ✅ | PM |
| RISC-V | [`k3-pico-itx`](docs/system/riscv/spacemit-k3.rst) | ✅ | ✅ | PM |
| RISC-V | [`milkv-duo`](docs/system/riscv/milkv-duo.rst) | ✅ | ✅ | UP |
| RISC-V | [`riscv-server-ref`](docs/system/riscv/riscv-server-ref.rst) | ✅ | ✅ | UP |
| ARM | [`ardep-v2`](docs/system/arm/stm32g474.rst) | ✅ | ✅ | PM |
| ARM | [`ax650x-pyramid`](docs/system/arm/ax650x-pyramid.rst) | ✅ | - | PM |
| ARM | [`phytium-pi`](docs/system/arm/phytium-pi.rst) | ✅ | ✅ | PM |
| ARM | [`rk3588-evb`](docs/system/arm/rk3588.rst) | ✅ | ✅ | PM |
| ARM | [`rk3588s-roc-pc`](docs/system/arm/rk3588.rst) | ✅ | ✅ | PM |
| ARM | [`rock-5b-plus`](docs/system/arm/rk3588.rst) | ✅ | ✅ | PM |
| ARM | [`s32k566-cvb-r52`](docs/system/arm/s32k5.rst) | ✅ | - | PM |
| ARM | [`stm32g474`](docs/system/arm/stm32g474.rst) | ✅ | - | PM |
| MCS-51 | [`stc8g1k08a`](docs/system/target-mcs51.rst) | ✅ | ✅ | PM |
| MCS-251 | [`stc32g144k246`](docs/system/target-mcs51.rst) | ✅ | ✅ | PM |

Source legend:

- `PM`: Process Mission downstream-maintained model.
- `UP`: imported from upstream QEMU/qemu-devel.
- `OSS`: imported from other open-source repositories.
- `VND`: imported from vendor sources.

## Development Workflow

The machine models in this branch are developed with the `oh-my-qemu` workflow.
It provides agent skills for planning, register extraction, peripheral modeling,
board modeling, qtest, build, debugging, and verification.

- Skill repository: <https://github.com/processmission/oh-my-qemu>
