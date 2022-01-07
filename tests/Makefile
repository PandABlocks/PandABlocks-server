# Makefile for running tests

TOP = ..

include $(TOP)/server/Makefile.flags


# ------------------------------------------------------------------------------
# Logic LUT (FPGA LookUp Table) parser test.  This involves generating and
# running an intermediate executable file.

test_lut: parse_lut_test
	./$^

.PHONY: test_lut
TESTS += test_lut

parse_lut_test: parse_lut_test.c $(TOP)/server/parse_lut.c
.INTERMEDIATE: parse_lut_test


# ------------------------------------------------------------------------------
# Exchange tests.

test_exchange:
	./run_with_server ./test_exchange.py -q transcript

.PHONY: test_exchange
TESTS += test_exchange


# ------------------------------------------------------------------------------
# Test handling of configuration file parsing.

test_configs:
	./test_configs

.PHONY: test_configs
TESTS += test_configs

# ------------------------------------------------------------------------------
# Test interface to extension server

test_extension:
	-nc localhost 9999 </dev/null
	$(TOP)/python/extension_server -d -qn $(TOP)/python/test_extension
	./test_extension.py extension_script

.PHONY: test_extension
TESTS += test_extension


# ------------------------------------------------------------------------------
# Tests for std_dev

test_std_sim: test_std_dev.c $(TOP)/server/std_dev.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test_std_cross: CC = $(CROSS_COMPILE)cc
test_std_cross: test_std_dev.c $(TOP)/server/std_dev.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)


# ------------------------------------------------------------------------------

tests: $(TESTS)
.DEFAULT_GOAL = tests
