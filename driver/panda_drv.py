# Python script for generatign panda_drv.h from *DRV entries in registers
# configuration file.

import sys
import os

# Add local python directory to path so we can pick up our local modules
root_dir = os.path.join(os.path.dirname(__file__), '..')
sys.path.append(os.path.join(root_dir, 'python'))

from zebra2.configparser import ConfigParser

config_dir = os.path.join(root_dir, 'config_d')
config = ConfigParser(config_dir)

drv = config.blocks['*DRV']
base = drv.base

print '''/* Register definitions derived from config_d/registers *DRV section.
 *
 * This file is automatically generated from driver/panda_drv.py.
 * Do not edit this file. */'''
for name, field in drv.fields.items():
    reg = int(field.reg[0])
    print '#define %s 0x%05x' % (name, (base << 12) | (reg << 2))
