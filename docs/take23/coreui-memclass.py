#!/usr/bin/env python3
# take23: CoreUI's memory class table (_gThemeMemoryPerformanceClasses) read from 24A446's dyld shared cache.
# Needs take19's cache reader: /dev/shm/t19/agE-scratch/dtools/dsc.py (the cache: /mnt/giochi/Linux/oneios/take18/work/dsc).
# The table is found from its first name: the C string "512MB" of CoreUI (scanstr.py 512MB -> 0x1bb5c6d46), the pointer to
# it in CoreUI's __DATA_CONST,__const (ptrscan.py CoreUI 0x1bb5c6d46 -> 0x1e1555cb0); entries are {value, constant, name}.
import sys, struct
sys.path.insert(0, '/dev/shm/t19/agE-scratch/dtools')
import dsc
c = dsc.Cache()
base = 0x1e1555ca0
data = c.read(base, 11 * 24)
def cstr(v):
    d = c.read(c.base + (v & 0x3FFFFFFFF), 48)
    return d.split(b'\0')[0].decode(errors='replace') if d else '?'
for i in range(11):
    value, const, name = struct.unpack_from('<QQQ', data, i * 24)
    print('%2d  %-30s %s' % (value, cstr(const), cstr(name)))
