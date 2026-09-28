#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 yi-mediad contributors

# Host-side extractor: read the ISP tuning (cfg_arr + isp_cfg_pt) out of your own
# rmm binary and write one blob per entry, in the 2019 (520) or native 521 layout
# (--layout); --emit-c writes a C wrapper. Output is vendor data: never commit it.
"""
extract_isp_cfg.py - build-time extractor for a sensor's ISP tuning config that"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from isp_layout_521 import LAYOUTS, SIZES_520, SIZES_521, DYNAMIC_TOP

TOTAL = (SIZES_520['isp_test_param'] + SIZES_520['isp_3a_param']
         + SIZES_520['isp_tunning_param'] + SIZES_520['isp_dynamic_param'])  # 96944


def load_elf(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        raise SystemExit("expected ELF32 little-endian")
    phoff = struct.unpack_from("<I", d, 0x1C)[0]
    phnum = struct.unpack_from("<H", d, 0x2C)[0]
    phes = struct.unpack_from("<H", d, 0x2A)[0]
    segs = []
    for i in range(phnum):
        o = phoff + i * phes
        if struct.unpack_from("<I", d, o)[0] != 1:
            continue
        segs.append((struct.unpack_from("<I", d, o + 8)[0],
                     struct.unpack_from("<I", d, o + 8)[0] + struct.unpack_from("<I", d, o + 16)[0],
                     struct.unpack_from("<I", d, o + 4)[0]))
    return d, segs


def v2o(segs, va):
    for a, b, o in segs:
        if a <= va < b:
            return o + (va - a)
    return None


def rd_str(d, off, n):
    end = d.find(b"\0", off, off + n)
    return d[off:(end if end >= 0 else off + n)].decode("ascii", "replace")


def remap_struct(name, src):
    """Copy every 520 field of `name` from its 521 counterpart."""
    out = bytearray(SIZES_520[name])
    for _f, o520, s520, o521, s521 in LAYOUTS[name]:
        if o521 is None:
            continue
        n = s520 if s520 < s521 else s521
        out[o520:o520 + n] = src[o521:o521 + n]
    return bytes(out)


def remap_dynamic(src):
    """isp_dynamic_param: scalar head + 14x nested isp_dynamic_config."""
    out = bytearray(SIZES_520['isp_dynamic_param'])
    cfg520 = cfg521 = None
    for _f, o520, s520, o521, s521 in DYNAMIC_TOP:
        if _f == 'isp_dynamic_cfg':
            cfg520, cfg521 = o520, o521
            continue
        if o521 is None:
            continue
        n = s520 if s520 < s521 else s521
        out[o520:o520 + n] = src[o521:o521 + n]
    for i in range(14):
        a = cfg520 + i * SIZES_520['isp_dynamic_config']
        b = cfg521 + i * SIZES_521['isp_dynamic_config']
        out[a:a + SIZES_520['isp_dynamic_config']] = remap_struct(
            'isp_dynamic_config', src[b:b + SIZES_521['isp_dynamic_config']])
    return bytes(out)


def map_config(d, segs, cfg_ptr):
    """Return the 96944-byte blob in the 2019 open-tree layout."""
    fo = v2o(segs, cfg_ptr)
    ptrs = struct.unpack_from("<4I", d, fo)   # test, 3a, tunning, iso
    order = ['isp_test_param', 'isp_3a_param', 'isp_tunning_param']
    parts = []
    for p, name in zip(ptrs[:3], order):
        po = v2o(segs, p)
        if po is None:
            return None
        sz = SIZES_521[name]
        parts.append(remap_struct(name, d[po:po + sz]))
    po = v2o(segs, ptrs[3])
    if po is None:
        return None
    parts.append(remap_dynamic(d[po:po + SIZES_521['isp_dynamic_param']]))
    out = b"".join(parts)
    assert len(out) == TOTAL, len(out)
    return out


# Stock gca_cfg[] has two extra leading entries (36 B vs our 28 B), so every
# later isp_tunning_param field sits 8 bytes further on in the stock blob.
V833_TUNNING_GCA_OFF = 87144     # start of gca_cfg (both layouts)
V833_TUNNING_GCA_SIZE = 28       # our ISP_GCA_MAX(7)*4
STOCK_TUNNING_GCA_SIZE = 36      # stock ISP_GCA_MAX(9)*4 (CT_W/CT_H prepended)
V833_TUNNING_PLTM_OFF = 87304    # pltm_cfg in our V833 struct
STOCK_TUNNING_PLTM_OFF = 87312   # pltm_cfg in the stock/isp522 struct


def fix_tunning_v833(t):
    """Re-align a stock 521 `isp_tunning_param` to the V833 struct layout."""
    out = bytearray(len(t))
    out[:V833_TUNNING_GCA_OFF] = t[:V833_TUNNING_GCA_OFF]
    # our gca_cfg[i] == stock gca_cfg[i+2] (skip CT_W/CT_H).
    g0 = V833_TUNNING_GCA_OFF
    out[g0:g0 + V833_TUNNING_GCA_SIZE] = \
        t[g0 + STOCK_TUNNING_GCA_SIZE - V833_TUNNING_GCA_SIZE:
          g0 + STOCK_TUNNING_GCA_SIZE]
    # The rest moves down by 8; the output length stays fixed (the last 8 bytes
    # stay zero).
    tail = t[g0 + STOCK_TUNNING_GCA_SIZE:]
    d0 = g0 + V833_TUNNING_GCA_SIZE
    out[d0:d0 + len(tail)] = tail
    return bytes(out)


def map_config_native(d, segs, cfg_ptr):
    """Return the 521 isp_param_config blob (133116 bytes) in the V833 layout;
    only isp_tunning_param needs realigning (fix_tunning_v833)."""
    total = (SIZES_521['isp_test_param'] + SIZES_521['isp_3a_param']
             + SIZES_521['isp_tunning_param'] + SIZES_521['isp_dynamic_param'])
    fo = v2o(segs, cfg_ptr)
    ptrs = struct.unpack_from("<4I", d, fo)   # test, 3a, tunning, iso
    order = ['isp_test_param', 'isp_3a_param', 'isp_tunning_param',
             'isp_dynamic_param']
    parts = []
    for p, name in zip(ptrs, order):
        po = v2o(segs, p)
        if po is None:
            return None
        sec = d[po:po + SIZES_521[name]]
        if name == 'isp_tunning_param':
            sec = fix_tunning_v833(sec)
        parts.append(sec)
    out = b"".join(parts)
    assert len(out) == total, len(out)
    return out


def emit_c(path, day, night):
    """Write the gc3003_cfg.c wrapper embedding the day/night blobs."""
    def arr(name, blob):
        lines = ["const unsigned int %s_len = %du;" % (name, len(blob)),
                 "const unsigned char %s[%d] = {" % (name, len(blob))]
        for i in range(0, len(blob), 16):
            lines.append("  " + ",".join("0x%02x" % b for b in blob[i:i + 16]) + ",")
        lines.append("};")
        return "\n".join(lines)
    with open(path, "w") as f:
        f.write("/* GENERATED from the user's own rmm.bin by "
                "tools/extract_isp_cfg.py - do not commit. */\n")
        f.write(arr("isp_gc3003_day", day) + "\n")
        f.write(arr("isp_gc3003_night", night) + "\n")
    print("wrote %s (day=%d night=%d)" % (path, len(day), len(night)))


def main():
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("rmm")
    ap.add_argument("prefix")
    ap.add_argument("sensor", nargs="?", default="gc3003_mipi")
    ap.add_argument("width", nargs="?", type=int)
    ap.add_argument("height", nargs="?", type=int)
    ap.add_argument("fps", nargs="?", type=int)
    ap.add_argument("--layout", choices=("520", "521"), default="520",
                    help="520: remap into the 2019 struct (default); "
                         "521: raw native 521 blob for the ISP521=1 build")
    ap.add_argument("--emit-c", metavar="FILE",
                    help="also write a gc3003_cfg.c embedding the day/night blobs")
    a = ap.parse_args()

    rmm, prefix = a.rmm, a.prefix
    sensor, width, height, fps = a.sensor, a.width, a.height, a.fps
    mapper = map_config_native if a.layout == "521" else map_config

    d, segs = load_elf(rmm)
    needle = sensor.encode() + b"\0"
    found = 0
    day = night = None
    i = d.find(needle)
    while i != -1:
        base = i
        cfgname = rd_str(d, base + 20, 50)
        w, h, f = struct.unpack_from("<iii", d, base + 72)
        wdr, ir = struct.unpack_from("<ii", d, base + 84)
        cfgptr = struct.unpack_from("<I", d, base + 92)[0]
        if (cfgname and w > 0 and v2o(segs, cfgptr) is not None
                and (width is None or w == width)
                and (height is None or h == height)
                and (fps is None or f == fps)):
            blob = mapper(d, segs, cfgptr)
            if blob:
                out = "%s_%s.bin" % (prefix, cfgname)
                open(out, "wb").write(blob)
                print("wrote %s (%d bytes) w=%d h=%d fps=%d wdr=%d ir=%d"
                      % (out, len(blob), w, h, f, wdr, ir))
                found += 1
                if ir:
                    night = blob
                elif "day" in cfgname:
                    day = blob
        i = d.find(needle, i + 1)
    if not found:
        raise SystemExit("no cfg_arr entry found for %s" % sensor)
    print("total %d config(s), layout %s" % (found, a.layout))
    if a.emit_c:
        if not day or not night:
            raise SystemExit("--emit-c needs both a day and a night config "
                             "(got day=%s night=%s)" % (bool(day), bool(night)))
        emit_c(a.emit_c, day, night)


if __name__ == "__main__":
    main()
