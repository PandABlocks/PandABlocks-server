# Makefile for running tests

TOP = ..


# ------------------------------------------------------------------------------
# Logic LUT (FPGA LookUp Table) parser test.  This involves generating and
# running an intermediate executable file.

test_lut: parse_lut_test
	./$^

.PHONY: test_lut
TESTS += test_lut

parse_lut_test: parse_lut_test.c $(TOP)/server/parse_lut.c
	gcc -std=gnu99 -I$(TOP)/server -o $@ $^
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
	$(TOP)/python/extension_server -d $(TOP)/python/test_extension
	./test_extension.py extension_script

.PHONY: test_extension
TESTS += test_extension


# ------------------------------------------------------------------------------

tests: $(TESTS)
.DEFAULT_GOAL = tests
