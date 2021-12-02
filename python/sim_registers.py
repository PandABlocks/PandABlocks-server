# Simple register support for sim_server

from __future__ import print_function

verbose = False

def set_verbose(_verbose):
    global verbose
    verbose = _verbose


class Registers:
    def read(self, num, reg):
        return 0

    def write(self, num, reg, value):
        pass


class REG(Registers):
    def read(self, num, reg):
        if reg == 15:
            # Return 1 for the CAPABILITY register to for Std Dev support
            return 1
        else:
            return 0


default_registers = Registers()

# Instances we need for our implementation
register_blocks = {
    0  : REG(),
}

def lookup_block(block):
    return register_blocks.get(block, default_registers)


def read_reg(block, num, reg):
    if verbose:
        print('R', block, num, reg, end = '')
    block = lookup_block(block)
    result = block.read(num, reg)
    if verbose:
        print('=>', result, hex(result))
    return result

def write_reg(block, num, reg, value):
    if verbose:
        print('W', block, num, reg, '<=', value, hex(value))
    block = lookup_block(block)
    block.write(num, reg, value)

def write_table(block, num, reg, data):
    if verbose:
        print('T', block, num, reg, repr(data))

def read_data(length):
    result = b''
    if verbose:
        print('D', length, '=>', repr(result))
    return result