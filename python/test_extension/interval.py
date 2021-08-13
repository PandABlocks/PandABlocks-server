from __future__ import division

# Simple conversion from centre/range to start end interval

class Range:
    def __init__(self, n):
        self.centre = 0
        self.range = 0

    def set_centre(self, centre):
        self.centre = centre
        return self._interval()

    def set_range(self, range):
        self.range = range
        return self._interval()

    def _interval(self):
        start = self.centre - self.range // 2
        return (start, start + self.range)


def Extension(count):
    return ExtensionHelper(Range, count)
