# Generates definitions of named registers from *REG entry in config file.

from __future__ import print_function

import sys

from parse_indent import parse_register_file
registers = parse_register_file(sys.argv[1])

base, fields = registers['*REG']


# For each field process the field definition: it is a register base optionally
# followed by a range.
def fixup_fields(name, value):
    values = value.split()
    start = int(values[0])
    if values[1:]:
        middle, end = values[1:]
        assert middle == '..', 'Malformed range'
        count = int(end) - start + 1
    else:
        count = 1
    return name, start, count

fields = [fixup_fields(name, value) for name, value in fields]


print('''\
/* Definitions of register names from *REG fields of the register configuration
 * file.  This file is re-read and confirmed equal on startup.
 *
 * This file is automatically generated from server/named_registers.py and
 * config_d/registers.
 *
 * DO NOT EDIT THIS FILE, edit the sources instead! */
''')

# First generate the #define statements
print('#define REG_BLOCK_BASE %s' % base)
print()
for name, value, _ in fields:
    print('#define %s %s' % (name, value))

# Now generate offset name table used for checking.
print('''
static struct named_register named_registers[] = {''')
for name, _, count in fields:
    print('    [%s] = { "%s", %d, false },' % (name, name, count))
print('};')
