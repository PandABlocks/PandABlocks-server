#!/usr/bin/env python
# Python script to perform standard deviation computation using numpy

from __future__ import print_function

import sys
import numpy

def compute_standard_deviation(n, b, a):
    a = numpy.float128(a)
    b = numpy.float128(b)
    # To avoid loss of precision, as b*b can lose bits, convert b/n to its
    # integer and fractional parts.
    bn_f, bn_i = numpy.modf(b / n)
#     print(bn_f, bn_i)
    n_s2 = numpy.float64((a - b * bn_i) - b * bn_f)
#     # Note that in the most extreme cases (when the magnitude of the mean is
#     # very large and n is also very large) the calculation b*b/n can lose
#     # significant bits and miscompute the standard deviation.
#     n_s2 = numpy.float64(a - b * b / n)
    return numpy.sqrt(n_s2 / n)

# Input in same format as C test harness

n = int(sys.argv[1], 0) & 0xFFFFFFFF
b = int(sys.argv[2], 0) & 0xFFFFFFFFFFFFFFFF
ah = int(sys.argv[3], 0) & 0xFFFFFFFF
al = int(sys.argv[4], 0) & 0xFFFFFFFFFFFFFFFF

a = al + (ah << 64)

s = compute_standard_deviation(n, b, a)

print("%u %u 0x%08x%016x => %.16e" % (n, b, ah, al, s))
