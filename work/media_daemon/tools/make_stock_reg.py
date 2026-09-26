#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 yi-mediad contributors
"""
make_stock_reg.py - turn a captured stock ISP register table into
isp_cfg/stock_reg.c for mediad's --wrap'd isp_set_load_reg().

Input is one `loadreg_NNN.bin` dumped by the register-capture tool (private workspace, work/ioctl_trace/isp_trace.c) while
stock rmm runs on the camera (see other.md). Use a settled frame (not 000).
The output is vendor-derived and gitignored.

Usage: make_stock_reg.py <loadreg_NNN.bin> isp_cfg/stock_reg.c
"""
import sys

def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    src, out = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    if len(d) < 0x4000:
        raise SystemExit("unexpected table size %d" % len(d))
    with open(out, "w") as f:
        f.write("/* GENERATED from a captured stock rmm register table by "
                "tools/make_stock_reg.py - do not commit. */\n")
        f.write("unsigned int isp_stock_reg_len = %du;\n" % len(d))
        f.write("unsigned char isp_stock_reg[%d] = {\n" % len(d))
        for i in range(0, len(d), 16):
            f.write("  " + ",".join("0x%02x" % b for b in d[i:i + 16]) + ",\n")
        f.write("};\n")
    print("wrote %s (%d bytes)" % (out, len(d)))

if __name__ == "__main__":
    main()
