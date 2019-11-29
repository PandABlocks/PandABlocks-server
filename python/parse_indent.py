# Helper functions for parsing register and configuration files.
#
# This file exports two functions of interest:
#   parse_indented_file(file_name) -> list of parsed indented sections
#   parse_register_file(file_name) -> dictionary of register definitions
#
# The more abstract function is parse_indented_file(): in this case the data
# structure returned can be described as a list of Parse values, where a Parse
# is a pair consisting of a parsed line and a list of sub-parses:
#
#   type FileParse = [Parse]
#   type Parse = (Line, [Parse])
#
# For example, a file containing the following lines:
#
#   a
#       b
#           c
#       d
#
# will return the following parse: [['a', [['b', [['c', []]]], ['d', []]]]] .
#
# The function parse_register_file(file_name) digests such a parse with
# assumptions appropriate to a register file to produce a rather simpler parse,
# consisting of a dictionary mapping block names to the block number and a list
# of field names and definitions:
#
#   type RegFileParse = {Key -> (Index, [(FieldName, FieldValue)])}
#
# For example, a file containing the following lines:
#
#   *REG        0
#       FPGA_VERSION            0
#
# returns the following parse: {'*REG': ('0', [['FPGA_VERSION', '0']])}


class ParseFail(Exception):
    def __init__(self, line_no, message):
        self.line_no = line_no
        self.message = message

    def __str__(self):
        return 'Line %d: %s' % (self.line_no, self.message)


# Implements line reading so that we can keep track of line numbers.  Also
# supports one level of undo on the process of iterating through the lines
class read_lines:
    def __init__(self, input_file):
        self.__input = input_file
        self.__last = None
        self.__undo = False
        self.line_no = 0

    def __iter__(self):
        return self

    def __next__(self):
        if not self.__undo:
            self.__last = self.read_line()
        self.__undo = False
        return self.__last
    next = __next__     # For Python2 compatibility

    def undo(self):
        self.__undo = True

    def read_line(self):
        while True:
            line = next(self.__input)
            self.line_no += 1
            content = line.lstrip(' ')
            if content[0] not in '#\n':
                break
        assert content[-1] == '\n', 'Unexpected end of input'
        return len(line) - len(content), content[:-1]

    def check(self, test, message):
        if not test:
            raise ParseFail(self.line_no, message)


# Parses lines at and above the given indentation, returns a nested list of the
# resulting parse.
def parse_indent_level(new_indent, lines):
    result = []
    for indent, value in lines:
        if indent < new_indent:
            lines.undo()
            break
        elif indent > new_indent:
            lines.undo()
            lines.check(result, 'Invalid indentation')
            lines.check(not result[-1][1], 'Sub-fields already parsed')
            result[-1][1] = parse_indent_level(indent, lines)
        else:
            result.append([value, []])
    return result

def parse_indented_file(file_name):
    lines = read_lines(open(file_name))
    return parse_indent_level(0, lines)


# This applies specific knowlege about the format of the register file to
# simplify the returned result.
def parse_register_file(file_name):
    parse = parse_indented_file(file_name)
    result = {}
    for reg_value, fields_in in parse:
        reg, value = reg_value.split()
        fields = []
        for field, sub_field in fields_in:
            assert not sub_field
            fields.append(field.split(None, 1))
        result[reg] = (value, fields)
    return result
