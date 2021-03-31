# Support for xadc readout.  Dummy example code.

# This file is loaded by the extension server

import logging

import os.path

XADC_PATH = '/sys/devices/soc0/amba/f8007100.adc/iio:device0'

class XADC:
    def __init__(self, node):
        logging.info('XADC %s', repr(node))
        self.node = node
        self.offset = 0
        self.scale = 1

    def read_node(self, part):
        filename = os.path.join(XADC_PATH, '%s_%s' % (self.node, part))
        return float(open(filename).read())

    def read(self, number):
        return self.scale * (self.offset + self.read_node('raw'))


class Extension:
    def __init__(self, count):
        assert count == 1, 'Only one system block expected'

    def parse_read(self, node):
        return XADC(node)
