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

class PolyField:
    def __init__(self, parent):
        logging.info('poly field')

    def read(self, number, *values):
        return sum(values)

    def write(self, number, *values):
        return reversed(values)


class Extension:
    constructors = {
        'poly' : PolyField,
    }

    def __init__(self, count):
        logging.info('dummy %d', count)
        self.count = count
        self.fields = {}

    def make_field(self, name):
        logging.info('make_field %s', name)
        if name not in self.fields:
            constructor = self.constructors.get(name, DummyField)
            self.fields[name] = constructor(self)
        return self.fields[name]

    def parse_read(self, node):
        return self.make_field(node).read

    def parse_write(self, node):
        return self.make_field(node).write
