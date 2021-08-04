#!/bin/bash
# Generates config files in PandABlocks-server and server repositories and populates them with information.

PLATFORM=$1

# PandABlocks-server:
# Create the CONFIG file
cd PandABlocks-server
touch CONFIG
# Populate the CONFIG file
if [ "$PLATFORM" == "zynq" ]; 
then
cat >> CONFIG <<EOL
# Default build location. Default is to build in build subdirectory.
BUILD_DIR = \$GITHUB_WORKSPACE/build-server

# Python interpreter. Default interpreter is python3
PYTHON = python3

# Sphinx build for documentation.
SPHINX_BUILD = sphinx-build

# Compiler settings. Default cross compile prefix given here.  If BINUTILS_DIR
# is specified it will be prepended to the path for building the driver and
# target server.
#
TOOLCHAIN_ROOT = \$GITHUB_WORKSPACE/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf

# Where the kernel was compiled.  Use PandABlocks-rootfs to build this.  This is
# only required if building the driver target.
#
KERNEL_DIR = \$GITHUB_WORKSPACE/build/build/linux

# Some tools from the PandABlocks-rootfs are used.
#
PANDA_ROOTFS = \$GITHUB_WORKSPACE/PandABlocks-rootfs

# List of default targets build when running make
#
DEFAULT_TARGETS =  server sim_server docs driver
#
# Whether it will run in platform zynq or zynqmp
#
PLATFORM = zynq
EOL
elif [ "$PLATFORM" == "zynqmp" ]
then
cat >> CONFIG <<EOL
# Default build location.  Default is to build in build subdirectory.
BUILD_DIR = \$GITHUB_WORKSPACE/build-server

# Python interpreter.  Default interpreter is python3
PYTHON = python3

# Sphinx build for documentation.
SPHINX_BUILD = sphinx-build

# Compiler settings.  Default cross compile prefix given here.  If BINUTILS_DIR
# is specified it will be prepended to the path for building the driver and
# target server.
#
TOOLCHAIN_ROOT = \$GITHUB_WORKSPACE/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu

# Where the kernel was compiled.  Use PandABlocks-rootfs to build this.  This is
# only required if building the driver target.
#
KERNEL_DIR = \$GITHUB_WORKSPACE/build/build/linux

# Some tools from the PandABlocks-rootfs are used.
#
PANDA_ROOTFS = \$GITHUB_WORKSPACE/PandABlocks-rootfs

# List of default targets build when running make
#
DEFAULT_TARGETS =  server sim_server docs driver
#
# Whether it will run in platform zynq or zynqmp
#
PLATFORM = zynqmp
EOL
fi
