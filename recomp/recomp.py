#!/usr/bin/env python3
"""Nintendo Land Recompiler: .rpx -> C++-Code + Speicherabbild.

Nutzung:  python recomp.py <Lunch.rpx> <ausgabe-ordner> [--per-file N]
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rpx_loader import load, write_image  # noqa: E402
from emit import generate, add_secondary_entries  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rpx")
    ap.add_argument("out")
    ap.add_argument("--per-file", type=int, default=1500)
    a = ap.parse_args()

    t0 = time.time()
    print(f"Lade {a.rpx} ...")
    rpx = load(a.rpx)
    print(f"  {len(rpx.func_syms)} Funktionen, {len(rpx.imports)} Imports, Einstieg 0x{rpx.entry:08x}")
    os.makedirs(a.out, exist_ok=True)
    write_image(rpx, os.path.join(a.out, "image.bin"))
    add_secondary_entries(rpx)
    print("Erzeuge C++ ...")
    files, unimpl = generate(rpx, a.out, a.per_file)
    if unimpl:
        print("Nicht uebersetzte Instruktionen:", dict(sorted(unimpl.items(), key=lambda x: -x[1])))
    print(f"Fertig: {len(files)} Dateien in {time.time() - t0:.0f}s")


if __name__ == "__main__":
    main()
