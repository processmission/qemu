# Process Mission QEMU

Process Mission maintains this downstream QEMU branch for board and SoC model
development that is not yet carried by upstream QEMU. It tracks new machine
models, boot flows, and test coverage as they are added.

This branch is regularly rebased on top of upstream QEMU. Please file an issue
for new machine requests or bug reports.

## Machine Models

| arch | machine | direct booting | firmware booting | documentation |
| --- | --- | --- | --- | --- |
| RISC-V | `k230-canmv` | ✅ | ✅ | [docs/system/riscv/k230-canmv.rst](docs/system/riscv/k230-canmv.rst) |
| ARM | `phytium-pi` | ✅ | ✅ | [docs/system/arm/phytium-pi.rst](docs/system/arm/phytium-pi.rst) |
| ARM | `rk3588-evb` | ✅ | ✅ | [docs/system/arm/rk3588.rst](docs/system/arm/rk3588.rst) |
| ARM | `rk3588s-roc-pc` | ✅ | ✅ | [docs/system/arm/rk3588.rst](docs/system/arm/rk3588.rst) |

## Development Workflow

The machine models in this branch are developed with the `oh-my-qemu` workflow.
It provides agent skills for planning, register extraction, peripheral modeling,
board modeling, qtest, build, debugging, and verification.

- Skill repository: <https://github.com/processmission/oh-my-qemu>
