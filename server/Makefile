# This Makefile is invoked by makefile after setting the working directory to
# the $(SERVER_BUILD_DIR) directory.
ifndef TOP
$(error Do not call this make file directly)
endif


default:    # Default target comes first!
.PHONY: default

CFLAGS += -std=gnu99
CFLAGS += -O3
CFLAGS += -g
CPPFLAGS += -D_GNU_SOURCE

CFLAGS += -Werror
CFLAGS += -Wall -Wextra
CFLAGS += -Wno-unused-parameter
CFLAGS += -Wno-missing-field-initializers
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

# Pick up kernel driver header file
CPPFLAGS += -I$(TOP)/driver

LDLIBS += -lpthread


SRCS += server.c
SRCS += error.c
SRCS += hardware.c
SRCS += socket_server.c
SRCS += config_server.c
SRCS += data_server.c
SRCS += parse.c
SRCS += hashtable.c
SRCS += database.c
SRCS += config_command.c
SRCS += system_command.c
SRCS += fields.c
SRCS += mux_lookup.c
SRCS += types.c
SRCS += classes.c


default: server

server: $(SRCS:.c=.o)

%.d: %.c
	set -o pipefail && $(CC) -M $(CPPFLAGS) $(CFLAGS) $< | \
            sed '1s/:/ $@:/' >$@
include $(SRCS:.c=.d)

# Target for assembler build for code generation inspection.
%.s: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -S $<
