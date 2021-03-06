Building and Configuring Panda Server
=====================================

Setting up the build for the Panda Socket Server requires configuring a number
of dependencies and creating a suitable ``CONFIG`` file in the root directory so
that they can be found.

Dependencies
------------

The following dependencies must be configured before any part of the server can
be built.

Zynq cross-compiler toolchain
    This can be downloaded as part of the Xilinx Vivado build environment for
    working with Zynq, or probably any ARMv7-A cross-compiler can be used.  This
    is needed to build all applications running on Panda.

PandABlocks-FPGA
    This part of the Panda project must be available before the server can be
    built, as it contains a configuration file defining the low level register
    interface to the Panda firmware.

PandABlocks-rootfs
    This part of the Panda project is required in order to provide a working
    kernel build tree, and to provide the zpkg build tool.


Setting up the ``CONFIG`` file
------------------------------

Start by copying the file ``CONFIG.example`` to ``CONFIG`` in the base
directory, and edit the file by commenting out lines as appropriate and editing
them.

The following symbols must be set to point to the appropriate dependencies:

``BINUTILS_DIR``
    If the Zynq cross-compiler toolchain is not on the path, this must be set in
    order to build the kernel module and the target build.  This symbol is not
    required for building the simulation server or the documentation.

``KERNEL_DIR``
    In order to build the kernel module, this symbol must be pointed to the
    kernel build tree generated by the PandABlocks-roots build.

``PANDA_ROOTFS``
    The zpkg build tool is found here.

The following symbols can all be left at their default values:

``BUILD_DIR``
    This configures where the built files will be placed.

``PYTHON``
    This configures which Python interpreter will be used for building.

``SPHINX_BUILD``
    The sphinx-build Python script used for building the documentation.

``DEFAULT_TARGETS``
    This determines which makefile targets are generated when ``make`` is run
    without specifying a particular target, or when ``make default`` is run.


Build Targets
-------------

The following build targets for the top level makefile are useful:

``default``
    Builds all the targets specified by ``$(DEFAULT_TARGETS)``, by default
    this list is: ``driver``, ``server``, ``sim_server``, ``docs``, ``zpkg``.

``driver``
    Builds the kernel driver module.

``server``
    Builds the server version to run on Panda.

``sim_server``
    Builds a simulation version of the server to run on the local PC.

``docs``
    Builds the documentation.

``zpkg``
    Builds the final ``panda-server`` zpgk file.

``clean``
    Removes the entire ``$(BUILD_DIR)`` directory.


Generated Files
---------------

In the ``$(BUILD_DIR)`` directory the following subdirectories and files will be
found.  In practice the ``.zpg`` file and ``html/`` directory will be wanted.

``driver/``
    The kernel module required for hardware access is built here.

| ``server/``
| ``sim_server/``

    These two directories are used to build the server to run on Panda, and
    a simulation server to run on the local PC.

``html/``
    The documentation is built in html format in this directory.

| ``panda-server@``\ version\ ``.zpg``
| ``zpkg-panda-server/``

    A zpkg for the server is built here.
