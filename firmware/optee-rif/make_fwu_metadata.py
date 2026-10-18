#!/usr/bin/env python3
"""Derive PSA FWU metadata (v2, 120 B) variants from Digi's stock file.

  trial  : active=1 (fip-b), previous=0 (fip-a), bank1 VALID (not accepted).
           TF-A BL2 boots fip-b; after FWU_MAX_TRIAL_REBOOT-1 = 3 boots that
           never get accepted it falls back to fip-a (plat_fwu_get_boot_idx).
  accept : active=1, previous=0, both banks ACCEPTED.
Bank states: 0xFC accepted, 0xFE valid, 0xFF invalid. CRC32 covers bytes 4..end.

Usage: make_fwu_metadata.py STOCK_METADATA OUTDIR
"""
import struct, sys, zlib

stock = bytearray(open(sys.argv[1], 'rb').read())
crc, ver, act, prev = struct.unpack_from('<IIII', stock, 0)
assert len(stock) == 120 and ver == 2 and zlib.crc32(stock[4:]) == crc

def variant(active, previous, bank0, bank1):
    m = bytearray(stock)
    struct.pack_into('<II', m, 8, active, previous)
    m[24], m[25] = bank0, bank1
    struct.pack_into('<I', m, 0, zlib.crc32(m[4:]))
    return bytes(m)

out = sys.argv[2]
open(f'{out}/metadata-trial-fip-b.bin', 'wb').write(variant(1, 0, 0xFC, 0xFE))
open(f'{out}/metadata-accept-fip-b.bin', 'wb').write(variant(1, 0, 0xFC, 0xFC))
