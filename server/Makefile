# This Makefile is invoked by makefile after setting the working directory to
# the $(SERVER_BUILD_DIR) directory.
ifndef TOP
$(error Do not call this make file directly)
endif


# Discover name of compiler: we're going to want different flags in some cases
CC_NAME := $(shell $(CC) --version | sed 's/ .*//;q')


default:    # Default target comes first!
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
CFLAGS += -Wconversion

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


# Compiling the server with clang has flagged up some points of interest, but
# alas most of its messages need to be disabled.
CFLAGS_clang += -Wmost
CFLAGS_clang += -Weverything

CFLAGS_clang += -Wno-language-extension-token
CFLAGS_clang += -Wno-disabled-macro-expansion
CFLAGS_clang += -Wno-gnu-conditional-omitted-operand
CFLAGS_clang += -Wno-gnu-statement-expression
CFLAGS_clang += -Wno-gnu-empty-struct
CFLAGS_clang += -Wno-gnu-zero-variadic-macro-arguments
CFLAGS_clang += -Wno-gnu-empty-initializer

CFLAGS_clang += -Wno-initializer-overrides
CFLAGS_clang += -Wno-pedantic
CFLAGS_clang += -Wno-assign-enum
CFLAGS_clang += -Wno-conditional-uninitialized
CFLAGS_clang += -Wno-covered-switch-default


# Pick up kernel driver header file
CPPFLAGS += -I$(TOP)/driver

LDLIBS += -lpthread
LDLIBS += -lrt
LDLIBS += -lm

CFLAGS += $(CFLAGS_$(CC_NAME))


SRCS += server.c                # Entry point to server, command line parsing
SRCS += persistence.c           # Support for persistence state over restarts
SRCS += error.c                 # Common error handling framework
SRCS += socket_server.c         # Common socket server handling
SRCS += config_server.c         # Configuration command server
SRCS += data_server.c           # Data socket server for streamed data capture
SRCS += buffered_file.c         # Buffered file IO for socket interface
SRCS += parse.c                 # Common string parsing support
SRCS += parse_lut.c             # 5 input lookup table expression parsing
SRCS += hashtable.c             # Simple hash table
SRCS += database.c              # Reading configuration and register files
SRCS += config_command.c        # Block and field command parsing and dispatch
SRCS += system_command.c        # System command parse and dispatch
SRCS += fields.c                # Top level block and field support
SRCS += classes.c               # Hardware class support
SRCS += types.c                 # Type parsing and formatting support
SRCS += enums.c                 # Enumeration handling
SRCS += attributes.c            # Class and type attribute support
SRCS += mux_lookup.c            # Mapping between field name and capture index
SRCS += output.c                # {bit,pos}_out class support
SRCS += time_position.c         # time and position support
SRCS += table.c                 # table classes support
SRCS += register.c              # param, read, write class support
SRCS += base64.c                # base64 conversion support

HW_SRCS  += hardware.c          # Hardware interface
SIM_SRCS += sim_hardware.c      # Simulated hardware interface


default: server sim_server

server: $(SRCS:.c=.o) $(HW_SRCS:.c=.o)

sim_server: $(SRCS:.c=.o) $(SIM_SRCS:.c=.o)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.d: %.c
	set -o pipefail && $(CC) -M $(CPPFLAGS) $(CFLAGS) $< | \
            sed '1s/:/ $@:/' >$@
include $(SRCS:.c=.d)

# Target for assembler build for code generation inspection.
%.s: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -S $<
