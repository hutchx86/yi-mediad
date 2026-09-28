#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 yi-mediad contributors

# Convert a native-521 isp_param_config blob from the 4-byte-enum ABI (133116 B)
# to the Melis RTOS short-enum ABI (133068 B). Only isp_dynamic_param differs:
# its 17-enum trigger shrinks from 68 to 20 bytes; everything else is identical.
"""Usage: convert_enum_abi.py <day.bin> <night.bin> <out_c> (writes *_short.bin and the
gc3003_cfg_521_short.c wrapper embedding them."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from extract_isp_cfg import emit_c

SIZE_4 = 133116
SIZE_1 = 133068
DYN = 124648          # offset of isp_dynamic_param (same in both ABIs)
DYN4 = 8468
DYN1 = 8420
TRIGER_N = 17
LUM_OFF_4, LUM_OFF_1 = 68, 20
GAIN_OFF_4, GAIN_OFF_1 = 124, 76
CFG_OFF_4, CFG_OFF_1 = 180, 132


def to_short_enums(src):
    assert len(src) == SIZE_4, "expected %d bytes, got %d" % (SIZE_4, len(src))
    out = bytearray(SIZE_1)
    out[0:DYN] = src[0:DYN]                      # test/3a/tunning identical
    d4 = src[DYN:DYN + DYN4]
    # triger: 17 four-byte enums -> 17 one-byte enums (padding 17..20 stays 0)
    for i in range(TRIGER_N):
        out[DYN + i] = d4[4 * i]
    out[DYN + LUM_OFF_1:DYN + LUM_OFF_1 + 56] = d4[LUM_OFF_4:LUM_OFF_4 + 56]
    out[DYN + GAIN_OFF_1:DYN + GAIN_OFF_1 + 56] = d4[GAIN_OFF_4:GAIN_OFF_4 + 56]
    out[DYN + CFG_OFF_1:DYN + CFG_OFF_1 + (DYN4 - CFG_OFF_4)] = \
        d4[CFG_OFF_4:DYN4]
    return bytes(out)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__.strip().splitlines()[-1])
    dayf, nightf, outc = sys.argv[1:4]
    day = to_short_enums(open(dayf, "rb").read())
    night = to_short_enums(open(nightf, "rb").read())
    dshort = dayf.replace(".bin", "_short.bin")
    nshort = nightf.replace(".bin", "_short.bin")
    open(dshort, "wb").write(day)
    open(nshort, "wb").write(night)
    print("wrote %s (%d), %s (%d)" % (dshort, len(day), nshort, len(night)))
    emit_c(outc, day, night)


if __name__ == "__main__":
    main()
