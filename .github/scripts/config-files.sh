#!/bin/bash
# Generates config files in PandABlocks-server and server repositories and populates them with information.

GITHUB_WORKSPACE='/home/runner/work/PandABlocks-server/PandABlocks-server'
PLATFORM=$1

# PandABlocks-server:
# Create the CONFIG file
cd $GITHUB_WORKSPACE/pandABlocks-server
touch CONFIG
# Populate the CONFIG file
if [ "$PLATFORM" == "zynq" ]; 
then
cat >> CONFIG <<EOL
# Default build location. Default is to build in build subdirectory.
BUILD_DIR = /home/runner/work/PandABlocks-server/PandABlocks-server/build-server

# Python interpreter. Default interpreter is python2, needs to be 2.7
PYTHON = python2

# Sphinx build for documentation.
SPHINX_BUILD = sphinx-build

# Compiler settings. Default cross compile prefix given here.  If BINUTILS_DIR
# is specified it will be prepended to the path for building the driver and
# target server.
#
TOOLCHAIN_ROOT = \
     /home/runner/work/PandABlocks-server/PandABlocks-server/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf

# Where the kernel was compiled.  Use PandABlocks-rootfs to build this.  This is
# only required if building the driver target.
#
KERNEL_DIR = /home/runner/work/PandABlocks-server/PandABlocks-server/build/linux

# Some tools from the rootfs are used.
#
PANDA_server = /home/runner/work/PandABlocks-server/PandABlocks-server/rootfs

# List of default targets build when running make
#
DEFAULT_TARGETS =  server # sim_server docs driver
#
# Whether it will run in platform zynq or zynqmp
#
PLATFORM = zynq
EOL
elif [ "$PLATFORM" == "zynqmp" ]
then
cat >> CONFIG <<EOL
# Default build location.  Default is to build in build subdirectory.
BUILD_DIR = /home/runner/work/PandABlocks-server/PandABlocks-server/build-server

# Python interpreter.  Default interpreter is python2, needs to be 2.7
PYTHON = python2

# Sphinx build for documentation.
SPHINX_BUILD = sphinx-build

# Compiler settings.  Default cross compile prefix given here.  If BINUTILS_DIR
# is specified it will be prepended to the path for building the driver and
# target server.
#
TOOLCHAIN_ROOT = \
     /home/runner/work/PandABlocks-server/PandABlocks-server/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu

# Where the kernel was compiled.  Use PandABlocks-rootfs to build this.  This is
# only required if building the driver target.
#
KERNEL_DIR = /home/runner/work/PandABlocks-server/PandABlocks-server/build/linux

# Some tools from the rootfs are used.
#
PANDA_server = /home/runner/work/PandABlocks-server/PandABlocks-server/rootfs

# List of default targets build when running make
#
DEFAULT_TARGETS =  server # sim_server docs driver
#
# Whether it will run in platform zynq or zynqmp
#
PLATFORM = zynqmp
EOL
fi

# PandABlocks-rootfs:
# Create the CONFIG file
cd $GITHUB_WORKSPACE/pandABlocks-rootfs
touch CONFIG
# Populate the CONFIG file
if [ "$PLATFORM" == "zynq" ]; 
then
cat >> CONFIG <<EOL
# Location of rootfs builder
ROOTFS_TOP = /home/runner/work/PandABlocks-server/PandABlocks-server/rootfs

# Toolchain used to build the target
TOOLCHAIN_ROOT = /home/runner/work/PandABlocks-server/PandABlocks-server/gcc-arm-10.2-2020.11-x86_64-arm-none-linux-gnueabihf

# Where to find source files
TAR_FILES = /home/runner/work/PandABlocks-server/PandABlocks-server/tar-files

# Target location for build
PANDA_ROOT = /home/runner/work/PandABlocks-server/PandABlocks-server/build

# Whether the platform is zynq or zyqnmp
PLATFORM = zynq
EOL
elif [ "$PLATFORM" == "zynqmp" ]
then
cat >> CONFIG <<EOL
# Location of rootfs builder
ROOTFS_TOP = /home/runner/work/PandABlocks-server/PandABlocks-server/rootfs

# Toolchain used to build the target
TOOLCHAIN_ROOT = /home/runner/work/PandABlocks-server/PandABlocks-server/gcc-arm-10.2-2020.11-x86_64-aarch64-none-linux-gnu

# Where to find source files
TAR_FILES = /home/runner/work/PandABlocks-server/PandABlocks-server/tar-files

# Target location for build
PANDA_ROOT = /home/runner/work/PandABlocks-server/PandABlocks-server/build

# Whether the platform is zynq or zyqnmp
PLATFORM = zynqmp
EOL
fi

# server:
# Create the CONFIG file
cd $GITHUB_WORKSPACE/rootfs
touch CONFIG.local
# Populate the CONFIG file
cat >> CONFIG.local <<EOL
TARGET = minimal

# This is the location where source and build files will be placed.
server_ROOT = /home/runner/work/PandABlocks-server/PandABlocks-server/build

# This is where all of the source tar files will be found.
TAR_DIRS = /home/runner/work/PandABlocks-server/PandABlocks-server/tar-files
EOL
