# Top level make file for building PandA socket server and associated device
# drivers for interfacing to the FPGA resources.

TOP := $(CURDIR)

# Build defaults that can be overwritten by the CONFIG file if present

BUILD_DIR = $(TOP)/build
PYTHON = python2
SPHINX_BUILD = sphinx-build
CROSS_COMPILE = arm-xilinx-linux-gnueabi-
BINUTILS_DIR =
KERNEL_DIR = $(error Define KERNEL_DIR in CONFIG file)
PANDA_ROOTFS = $(error Define PANDA_ROOTFS in CONFIG file)
MAKE_ZPKG = $(PANDA_ROOTFS)/make-zpkg

DEFAULT_TARGETS = driver server sim_server docs zpkg


# The CONFIG file is required.  If not present, create by copying CONFIG.example
# and editing as appropriate.
include CONFIG


ARCH = arm
CC = $(CROSS_COMPILE)gcc

DRIVER_BUILD_DIR = $(BUILD_DIR)/driver
SERVER_BUILD_DIR = $(BUILD_DIR)/server
SIM_SERVER_BUILD_DIR = $(BUILD_DIR)/sim_server
DOCS_BUILD_DIR = $(BUILD_DIR)/html

DRIVER_FILES := $(wildcard driver/*)
SERVER_FILES := $(wildcard server/*)

ifdef BINUTILS_DIR
PATH := $(BINUTILS_DIR):$(PATH)
endif

default: $(DEFAULT_TARGETS)
.PHONY: default


export GIT_VERSION := $(shell git describe --abbrev=7 --dirty --always --tags)

export PYTHONPATH = $(TOP)/python

# ------------------------------------------------------------------------------
# Kernel driver building

PANDA_KO = $(DRIVER_BUILD_DIR)/panda.ko

# Building kernel modules out of tree is a headache.  The best workaround is to
# link all the source files into the build directory.
DRIVER_BUILD_FILES := $(DRIVER_FILES:driver/%=$(DRIVER_BUILD_DIR)/%)
$(DRIVER_BUILD_FILES): $(DRIVER_BUILD_DIR)/%: driver/%
	ln -s $$(readlink -e $<) $@

# The driver register header file needs to be built.
DRIVER_HEADER = $(DRIVER_BUILD_DIR)/panda_drv.h
$(DRIVER_HEADER): driver/panda_drv.py $(TOP)/config_d/registers
	$(PYTHON) $^ >$@

$(PANDA_KO): $(DRIVER_BUILD_DIR) $(DRIVER_BUILD_FILES) $(DRIVER_HEADER)
	$(MAKE) -C $(KERNEL_DIR) M=$< modules \
            ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE)
	touch $@


driver: $(PANDA_KO)
.PHONY: driver


# ------------------------------------------------------------------------------
# Socket server

SERVER = $(SERVER_BUILD_DIR)/server
SIM_SERVER = $(SIM_SERVER_BUILD_DIR)/sim_server
SLOW_LOAD = $(SERVER_BUILD_DIR)/slow_load
SERVER_FILES := $(wildcard server/*)

SERVER_BUILD_ENV += VPATH=$(TOP)/server
SERVER_BUILD_ENV += TOP=$(TOP)
SERVER_BUILD_ENV += PYTHON=$(PYTHON)

$(SERVER): $(SERVER_BUILD_DIR) $(SERVER_FILES)
	$(MAKE) -C $< -f $(TOP)/server/Makefile $(SERVER_BUILD_ENV) CC=$(CC)

# Two differences with building sim_server: we use the native compiler, not the
# cross-compiler, and we only build the sim_server target.
$(SIM_SERVER): $(SIM_SERVER_BUILD_DIR) $(SERVER_FILES)
	$(MAKE) -C $< -f $(TOP)/server/Makefile $(SERVER_BUILD_ENV) sim_server

$(SLOW_LOAD): $(SERVER_BUILD_DIR) server/slow_load.c
	$(MAKE) -C $< -f $(TOP)/server/Makefile $(SERVER_BUILD_ENV) CC=$(CC) \
            slow_load


# Construction of simserver launch script.
SIMSERVER_SUBSTS += s:@@PYTHON@@:$(PYTHON):;
SIMSERVER_SUBSTS += s:@@BUILD_DIR@@:$(BUILD_DIR):;

simserver: simserver.in
	sed '$(SIMSERVER_SUBSTS)' $< >$@
	chmod +x $@

$(BUILD_DIR)/config_d: config_d/registers $(wildcard python/sim_config/*)
	mkdir -p $@
	rm -f $@/*
	cp python/sim_config/* $@
	cat $< >>$@/registers
	touch $@

server: $(SERVER)
sim_server: $(SIM_SERVER) simserver $(BUILD_DIR)/config_d
slow_load: $(SLOW_LOAD)

.PHONY: server sim_server slow_load


# ------------------------------------------------------------------------------
# Documentation

$(DOCS_BUILD_DIR)/index.html: $(wildcard docs/*.rst docs/*/*.rst docs/conf.py)
	$(SPHINX_BUILD) -b html docs $(DOCS_BUILD_DIR)

docs: $(DOCS_BUILD_DIR)/index.html

clean-docs:
	rm -rf $(DOCS_BUILD_DIR)

.PHONY: docs clean-docs


# ------------------------------------------------------------------------------
# Build installation package

zpkg: etc/panda-server.list $(PANDA_KO) $(SERVER) $(SLOW_LOAD)
	rm -f $(BUILD_DIR)/*.zpg
	$(MAKE_ZPKG) -t $(TOP) -b $(BUILD_DIR) -d $(BUILD_DIR) \
            $< $(GIT_VERSION)

.PHONY: zpkg


# ------------------------------------------------------------------------------
# Run automatic tests

tests: sim_server
	make -C tests

.PHONY: tests


# ------------------------------------------------------------------------------

# This has global effect, and is mostly desirable behaviour.
.DELETE_ON_ERROR:


# This needs to go more or less last to avoid conflict with other targets.
$(BUILD_DIR)/%:
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f simserver
	find -name '*.pyc' -delete

.PHONY: clean


DEPLOY += $(PANDA_KO)
DEPLOY += $(SERVER)

deploy: $(DEPLOY)
	scp $^ root@172.23.252.202:/opt

.PHONY: deploy
