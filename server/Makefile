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

CFLAGS_clang += -Wmost
CFLAGS_clang += -Weverything

CFLAGS += -Wno-switch-enum
CFLAGS += -Wno-variadic-macros
CFLAGS += -Wno-padded
CFLAGS += -Wno-format-nonliteral
CFLAGS += -Wno-vla
CFLAGS += -Wno-c++-compat
CFLAGS += -Wno-pointer-arith

CFLAGS_clang += -Wno-language-extension-token
CFLAGS_clang += -Wno-disabled-macro-expansion
CFLAGS_clang += -Wno-gnu-conditional-omitted-operand
CFLAGS_clang += -Wno-gnu-statement-expression
CFLAGS_clang += -Wno-gnu-empty-struct
CFLAGS_clang += -Wno-gnu-zero-variadic-macro-arguments
CFLAGS_clang += -Wno-gnu-empty-initializer

CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-missing-field-initializers
CFLAGS_cc    += -Wno-override-init
CFLAGS_clang += -Wno-initializer-overrides


# Pick up kernel driver header file
CPPFLAGS += -I$(TOP)/driver

LDLIBS += -lpthread
LDLIBS += -lrt
LDLIBS += -lm

CFLAGS += $(CFLAGS_$(CC_NAME))


SRCS += server.c
SRCS += error.c
SRCS += socket_server.c
SRCS += config_server.c
SRCS += buffered_file.c
SRCS += data_server.c
SRCS += parse.c
SRCS += hashtable.c
SRCS += database.c
SRCS += config_command.c
SRCS += system_command.c
SRCS += fields.c
SRCS += classes.c
SRCS += parse_lut.c
SRCS += types.c
SRCS += enums.c
SRCS += attributes.c
SRCS += capture.c

ifdef SIMSERVER
CFLAGS += -DSIMSERVER
SRCS += sim_hardware.c
else
SRCS += hardware.c
endif


default: server

server: $(SRCS:.c=.o)

%.d: %.c
	set -o pipefail && $(CC) -M $(CPPFLAGS) $(CFLAGS) $< | \
            sed '1s/:/ $@:/' >$@
include $(SRCS:.c=.d)

# Target for assembler build for code generation inspection.
%.s: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -S $<
