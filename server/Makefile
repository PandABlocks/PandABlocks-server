# This Makefile is invoked by makefile after setting the working directory to
# the $(SERVER_BUILD_DIR) directory.
ifndef TOP
$(error Do not call this make file directly)
endif

# The default shell used by make is sh, but in this makefile we use the bash
# pipefail option.  On most systems this still works because sh is now usually
# linked to bash, but on Ubuntu sh is linked to dash which doesn't support
# pipefail.  Setting this option is always safe, just makes bash a dependency.
SHELL = bash


# Git version
BUILD_DATE_TIME = $(shell date +'%Y-%m-%d %H:%M:%S')


default: server sim_server
.PHONY: default


CFLAGS += -std=gnu99
CFLAGS += -O3
CFLAGS += -g
CPPFLAGS += -D_GNU_SOURCE

CFLAGS += -Werror
CFLAGS += -Wall
CFLAGS += -Wextra

CFLAGS += -Wundef
CFLAGS += -Wshadow
CFLAGS += -Wcast-align
CFLAGS += -Wwrite-strings
CFLAGS += -Wredundant-decls
CFLAGS += -Wmissing-prototypes
CFLAGS += -Wmissing-declarations
CFLAGS += -Wstrict-prototypes
CFLAGS += -Wcast-qual
CFLAGS += -Woverflow
#CFLAGS += -Wconversion

CFLAGS += -fstrict-overflow
CFLAGS += -Wsign-compare

CFLAGS += -Wstrict-overflow=5

CFLAGS += -Wno-switch-enum
CFLAGS += -Wno-variadic-macros
CFLAGS += -Wno-padded
CFLAGS += -Wno-format-nonliteral
CFLAGS += -Wno-vla
CFLAGS += -Wno-c++-compat
CFLAGS += -Wno-pointer-arith
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-missing-field-initializers


# Pick up kernel driver header file
CPPFLAGS += -I$(TOP)/driver

LDLIBS += -lpthread
LDLIBS += -lrt
LDLIBS += -lm


# Allow access to function names in assert fail tracebacks
sim_server: LDFLAGS += -rdynamic

# Debug override option
ifdef DEBUG
CFLAGS += -O0
endif


SRCS += server.c                # Entry point to server, command line parsing
SRCS += version.c               # Server version string
SRCS += locking.c               # Pthread locking support etc
SRCS += persistence.c           # Support for persistence state over restarts
SRCS += error.c                 # Common error handling framework
SRCS += socket_server.c         # Common socket server handling
SRCS += config_server.c         # Configuration command server
SRCS += data_server.c           # Data socket server for streamed data capture
SRCS += buffer.c                # Circular buffer for captured data stream
SRCS += buffered_file.c         # Buffered file IO for socket interface
SRCS += parse.c                 # Common string parsing support
SRCS += utf8_check.c            # External UTF-8 format checker
SRCS += parse_lut.c             # 5 input lookup table expression parsing
SRCS += hashtable.c             # Simple hash table
SRCS += database.c              # Reading configuration and register files
SRCS += config_command.c        # Block and field command parsing and dispatch
SRCS += system_command.c        # System command parse and dispatch
SRCS += fields.c                # Top level block and field support
SRCS += types.c                 # Type parsing and formatting support
SRCS += enums.c                 # Enumeration handling
SRCS += attributes.c            # Class and type attribute support
SRCS += pos_mux.c               # pos_mux field class
SRCS += ext_out.c               # ext_out field class
SRCS += bit_out.c               # bit_out class and bit_mux type support
SRCS += pos_out.c               # pos_out field class
SRCS += output.c                # Top level data capture
SRCS += prepare.c               # Data capture preparation
SRCS += capture.c               # Data capture control
SRCS += time.c                  # time class and type support
SRCS += table.c                 # table classes support
SRCS += register.c              # param, read, write class support
SRCS += base64.c                # base64 conversion support
SRCS += metadata.c              # Support for *METADATA command
SRCS += mac_address.c           # Support for FPGA MAC address loading
SRCS += extension.c             # Support for extension server registers


# Program for loading slow FPGA on carrier card.
slow_load: slow_load.c


# For the true hardware target and simulation targets we need to take a bit of
# care.  The source file hardware.c needs to be compiled twice with two
# differing targets.
hw_hardware.o: hardware.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<

sw_hardware.o: CPPFLAGS += -DSIM_HARDWARE
sw_hardware.o: hardware.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c -o $@ $<


# Special rules for named_registers.h: this is automatically generated from the
# *REG entry in the register definitions file.
hw_hardware.o: named_registers.h
sw_hardware.o: named_registers.h

named_registers.h: named_registers.py $(TOP)/config_d/registers
	$(PYTHON) $^ >$@

CPPFLAGS += -I.


# Pass the git version through to version
version.o: CPPFLAGS += -DGIT_VERSION='"$(GIT_VERSION)"'
version.o: CPPFLAGS += -DBUILD_DATE_TIME='"$(BUILD_DATE_TIME)"'

# Forces version.o to be rebuild every time so that it's accurate
version.o: FORCE
FORCE:
.phony: FORCE


server: $(SRCS:.c=.o) hw_hardware.o
	! nm $^ | grep ' [CD] '
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

sim_server: $(SRCS:.c=.o) sw_hardware.o sim_hardware.o
	! nm $^ | grep ' [CD] '
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.d: %.c
	set -o pipefail && $(CC) -M $(CPPFLAGS) $(CFLAGS) $< | \
            sed '1s/:/ $@:/' >$@
include $(SRCS:.c=.d)

# Target for assembler build for code generation inspection.
%.s: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -S $<


# This has global effect, and is mostly desirable behaviour.
.DELETE_ON_ERROR:
