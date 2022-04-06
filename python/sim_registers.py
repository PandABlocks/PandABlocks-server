# Simple register support for sim_server

from __future__ import print_function

import time
import numpy


# A bit of backwards compatibility sadness.  Numpy on RHEL7 is only version 1.7,
# too old for tobytes, so we roll our own if necessary.
def tobytes(x):
    if hasattr(x, 'tobytes'):
        return x.tobytes()
    else:
        # Fall back to the old way of doing this
        import ctypes
        return ctypes.string_at(x.ctypes.data, x.nbytes)


verbose = False

def set_verbose(_verbose):
    global verbose
    verbose = _verbose


class Registers:
    def read(self, num, reg):
        # By default all registers return 0
        return 0

    def write(self, num, reg, value):
        # Register writes are normally ignored
        pass


class REG(Registers):
    # Register names (from ../config_d/registers, as needed)
    PCAP_REGISTERS = range(8, 15)   # 8 to 14 inclusive
    FPGA_CAPABILITIES = 15

    def __init__(self, pcap):
        self.pcap = pcap

    def read(self, num, reg):
        if reg == self.FPGA_CAPABILITIES:
            # Return 1 for the CAPABILITY register for Std Dev support
            return 1
        else:
            return 0

    def write(self, num, reg, value):
        print('*REG[%d] <= %08x' % (reg, value))
        if reg in self.PCAP_REGISTERS:
            pcap.write_pcap(reg, value)


class PCAP:
    # Register names from *REG
    PCAP_START_WRITE  = 8
    PCAP_WRITE        = 9
    PCAP_ARM          = 13
    PCAP_DISARM       = 14

    def __init__(self):
        self.active = False
        self.capture_count = 0
        self.sample_count = 0
        self.last_sent = time.time()

    def write_pcap(self, reg, value):
        if reg == self.PCAP_START_WRITE:
            self.capture_count = 0
            self.sample_count = 0
        elif reg == self.PCAP_WRITE:
            self.capture_count += 1
        elif reg == self.PCAP_ARM:
            self.active = True
        elif reg == self.PCAP_DISARM:
            self.active = False

    def read_data(self, length):
        if self.active:
            # Pacing: only send an update every 100ms
            now = time.time()
            if now - self.last_sent > 0.1:
                self.last_sent = now
                count = self.capture_count
                samples = self.sample_count
                self.sample_count += 1

                data = numpy.arange(count, dtype = numpy.uint32) + 256 * samples
                return tobytes(data)
            else:
                return b''
        else:
            return None



# Instances we need for our implementation
default_registers = Registers()
pcap = PCAP()
register_blocks = {
    0  : REG(pcap),
}


def lookup_block(block):
    return register_blocks.get(block, default_registers)


def read_reg(block, num, reg):
    if verbose:
        print('R', block, num, reg, end = '')
    result = lookup_block(block).read(num, reg)
    if verbose:
        print(' =>', result, hex(result))
    return result

def write_reg(block, num, reg, value):
    if verbose:
        print('W', block, num, reg, '<=', value, hex(value))
    lookup_block(block).write(num, reg, value)

def write_table(block, num, reg, data):
    if verbose:
        print('T', block, num, reg, repr(data))

read_data = pcap.read_data
