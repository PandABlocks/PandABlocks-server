# Dummy extension service

import logging


class DummyField:
    def __init__(self, parent):
        logging.info('dummy field')
        self.values = [0] * parent.count

    def read(self, number):
        return self.values[number]

    def write(self, number, value):
        self.values[number] = value


class Extension:
    def __init__(self, count):
        logging.info('dummy %d', count)
        self.count = count
        self.fields = {}

    def make_field(self, name):
        logging.info('make_field %s', name)
        if name not in self.fields:
            self.fields[name] = DummyField(self)
        return self.fields[name]

    def parse_read(self, node):
        return self.make_field(node)

    def parse_write(self, node):
        return self.make_field(node)
