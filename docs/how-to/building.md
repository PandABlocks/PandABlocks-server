# Build and test the server

:::{tip}
For local development the [PandABlocks-devcontainer](https://github.com/PandABlocks/PandABlocks-devcontainer)
provides a pre-configured environment and is the recommended starting point.
The instructions below cover a native build for those who need it.
:::

## Dependencies

The following must be available before building the server.

**Zynq cross-compiler toolchain**
: Available as part of the Xilinx Vivado build environment, or any ARMv7-A
  cross-compiler. Required for all applications running on PandA.

**PandABlocks-FPGA**
: Must be available before building the server; it contains the configuration
  file defining the low-level register interface to the PandA firmware.

**PandABlocks-rootfs**
: Provides the kernel build tree and the `zpkg` build tool.

## Setting up the `CONFIG` file

Copy `CONFIG.example` to `CONFIG` in the base directory and edit as needed,
commenting out lines that are not required.

:::{note}
The exact syntax accepted by the server Makefile for `CONFIG` values is pending
verification (tracked in the issue backlog). Use the form shown in
`CONFIG.example`.
:::

The following symbols must point to the appropriate dependencies:

`BINUTILS_DIR`
: Path to the Zynq cross-compiler toolchain if it is not on `PATH`. Not required
  for the simulation server or documentation builds.

`KERNEL_DIR`
: Path to the kernel build tree from the PandABlocks-rootfs build. Required to
  build the kernel module.

`PANDA_ROOTFS`
: Path to the PandABlocks-rootfs checkout where the `zpkg` build tool is found.

The following symbols can be left at their defaults:

`BUILD_DIR`
: Destination for built files.

`PYTHON`
: Python interpreter used during the build.

`SPHINX_BUILD`
: `sphinx-build` script for documentation builds.

`DEFAULT_TARGETS`
: Makefile targets built by `make` or `make default`. Default list: `driver`,
  `server`, `sim_server`, `docs`, `zpkg`.

## Build targets

| Target       | Description                                              |
|--------------|----------------------------------------------------------|
| `default`    | Builds all targets listed in `$(DEFAULT_TARGETS)`        |
| `driver`     | Kernel driver module                                     |
| `server`     | Server binary to run on PandA                            |
| `sim_server` | Simulation server to run on the local PC                 |
| `docs`       | HTML documentation                                       |
| `zpkg`       | `panda-server` zpkg package                              |
| `clean`      | Removes the entire `$(BUILD_DIR)` directory              |

## Generated files

After a successful build, `$(BUILD_DIR)` contains:

`driver/`
: Kernel module for hardware access.

`server/` and `sim_server/`
: The on-PandA server and the local simulation server respectively.

`html/`
: HTML documentation.

`panda-server@`*version*`.zpg` and `zpkg-panda-server/`
: The zpkg package for installation onto PandA.
