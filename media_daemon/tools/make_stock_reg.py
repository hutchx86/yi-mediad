#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-only
# Copyright (C) 2026 yi-mediad contributors

# Turn a load-reg table captured from your own camera's stock rmm (a settled
# frame) into isp_cfg/stock_reg_tbl.c for the STOCK_REG=1 build. Output is gitignored.
"""Usage: make_stock_reg.py <loadreg_NNN.bin> isp_cfg/stock_reg_tbl.c
(the output is vendor-derived: do not commit it)"""
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
