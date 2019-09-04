# Dummy extension service

class Dummy:
    def __init__(self):
        pass

    def read(self, number):
        return 0

    def write(self, number, value):
        pass

class Extension:
    def __init__(self, count):
        pass

    def parse_read(self, node):
        return Dummy()

    def parse_write(self, node):
        return Dummy()
