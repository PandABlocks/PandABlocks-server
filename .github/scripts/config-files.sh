#!/bin/bash
# Generates config files in PandABlocks-rootfs and rootfs repositories and populates them with information.

GITHUB_WORKSPACE='/home/runner/work/PandABlocks-rootfs/PandABlocks-rootfs'
# PLATFORM=$1

# PandABlocks-rootfs:
# Create the CONFIG file
cd $GITHUB_WORKSPACE/pandABlocks-rootfs
touch CONFIG
# Populate the CONFIG file
cat >> CONFIG <<EOL
# Default build location.  Default is to build in build subdirectory.
BUILD_DIR = /home/runner/work/PandABlocks-rootfs/PandABlocks-rootfs/build

# Python interpreter.  Default interpreter is python2, needs to be 2.7
PYTHON = python2

# Sphinx build for documentation.
SPHINX_BUILD = sphinx-build

# Compiler settings.  Default cross compile prefix given here.  If BINUTILS_DIR
# is specified it will be prepended to the path for building the driver and
# target server.
#
TOOLCHAIN_ROOT = \
     /home/runner/work/PandABlocks-server/PandABlocks-server/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf

# Some tools from the rootfs are used.
#
PANDA_ROOTFS = /home/runner/work/PandABlocks-rootfs/PandABlocks-rootfs/rootfs

# List of default targets build when running make
#
# DEFAULT_TARGETS = driver server sim_server docs
#
# Whether it will run in platform zynq or zynqmp
#
PLATFORM = zynq
EOL

# rootfs:
# Create the CONFIG file
cd $GITHUB_WORKSPACE/rootfs
touch CONFIG.local
# Populate the CONFIG file
cat >> CONFIG.local <<EOL
TARGET = minimal

# This is the location where source and build files will be placed.
ROOTFS_ROOT = /home/runner/work/PandABlocks-server/PandABlocks-server/build
EOL
