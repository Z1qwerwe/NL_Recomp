#!/usr/bin/env python3
"""RPX-Analyzer: liest eine Wii-U-.rpx/.rpl ein und listet alle Imports nach Library.

Nutzung:
    python rpx_analyzer.py <pfad/zur/datei.rpx> [--json out.json] [--md out.md]

Zeigt dir, welche Systemfunktionen (coreinit, gx2, vpad, ...) das Spiel braucht,
also genau das, was eine Recomp-Runtime nachbauen muss.
"""
import argparse
import json
import struct
import sys
import zlib
from collections import defaultdict

SHT_SYMTAB = 2
SHT_RPL_EXPORTS = 0x80000001
SHT_RPL_IMPORTS = 0x80000002
SHF_RPL_ZLIB = 0x08000000
SHF_EXECINSTR = 0x4


class Section:
    def __init__(self, idx, name_off, stype, flags, addr, off, size, link, info, align, entsize):
        self.idx, self.name_off, self.type, self.flags = idx, name_off, stype, flags
        self.addr, self.off, self.size, self.link = addr, off, size, link
        self.info, self.align, self.entsize = info, align, entsize
        self.name = ""
        self._data = None


def load(path):
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"\x7fELF" or raw[4] != 1 or raw[5] != 2:
        sys.exit("Keine gueltige 32-bit Big-Endian ELF/RPX-Datei")
    abi = raw[7:9]
    (shoff,) = struct.unpack_from(">I", raw, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", raw, 0x2E)

    secs = []
    for i in range(shnum):
        v = struct.unpack_from(">10I", raw, shoff + i * shentsize)
        secs.append(Section(i, *v))

    def data(s):
        if s._data is None:
            if s.off == 0 or s.size == 0:
                s._data = b""
            else:
                d = raw[s.off:s.off + s.size]
                if s.flags & SHF_RPL_ZLIB:
                    d = zlib.decompress(d[4:])
                s._data = d
        return s._data

    strtab = data(secs[shstrndx])
    for s in secs:
        end = strtab.find(b"\0", s.name_off)
        s.name = strtab[s.name_off:end].decode("ascii", "replace")
    return raw, abi, secs, data


def analyze(path):
    raw, abi, secs, data = load(path)
    imports = defaultdict(lambda: {"functions": set(), "data": set()})
    exports = []

    import_secs = {}
    for s in secs:
        if s.type == SHT_RPL_IMPORTS:
            # Name: .fimport_<lib> (Funktionen) oder .dimport_<lib> (Daten)
            kind = "functions" if s.name.startswith(".fimport_") else "data"
            lib = s.name.split("_", 1)[1] if "_" in s.name else s.name
            lib = lib[:-4] if lib.endswith(".rpl") else lib
            import_secs[s.idx] = (lib, kind)

    for s in secs:
        if s.type != SHT_SYMTAB:
            continue
        symdata = data(s)
        strdata = data(secs[s.link])
        for off in range(0, len(symdata), 16):
            name_off, value, size, info, other, shndx = struct.unpack_from(">IIIBBH", symdata, off)
            if shndx in import_secs and name_off:
                end = strdata.find(b"\0", name_off)
                name = strdata[name_off:end].decode("ascii", "replace")
                lib, kind = import_secs[shndx]
                imports[lib][kind].add(name)

    for s in secs:
        if s.type == SHT_RPL_EXPORTS:
            d = data(s)
            count, _sig = struct.unpack_from(">II", d, 0)
            for i in range(count):
                _val, name_off = struct.unpack_from(">II", d, 8 + i * 8)
                name_off &= 0x7FFFFFFF
                end = d.find(b"\0", name_off)
                exports.append(d[name_off:end].decode("ascii", "replace"))

    code_size = sum(len(data(s)) for s in secs if s.flags & SHF_EXECINSTR)
    return {
        "file": path,
        "abi": abi.hex(),
        "sections": len(secs),
        "code_bytes": code_size,
        "approx_instructions": code_size // 4,
        "imports": {lib: {k: sorted(v) for k, v in d.items()} for lib, d in sorted(imports.items())},
        "exports": sorted(exports),
    }


def to_markdown(r):
    lines = [f"# RPX-Analyse: `{r['file'].split('/')[-1]}`", ""]
    total_f = sum(len(d["functions"]) for d in r["imports"].values())
    total_d = sum(len(d["data"]) for d in r["imports"].values())
    lines += [
        f"- Code: {r['code_bytes']:,} Bytes (~{r['approx_instructions']:,} PowerPC-Instruktionen)",
        f"- Libraries: {len(r['imports'])}",
        f"- Importierte Funktionen: {total_f}, importierte Daten-Symbole: {total_d}",
        "",
        "| Library | Funktionen | Daten |",
        "|---|---:|---:|",
    ]
    for lib, d in sorted(r["imports"].items(), key=lambda x: -len(x[1]["functions"])):
        lines.append(f"| {lib} | {len(d['functions'])} | {len(d['data'])} |")
    for lib, d in sorted(r["imports"].items()):
        lines += ["", f"## {lib}", ""]
        for fn in d["functions"]:
            lines.append(f"- `{fn}`")
        for dv in d["data"]:
            lines.append(f"- `{dv}` *(Daten)*")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rpx")
    ap.add_argument("--json")
    ap.add_argument("--md")
    a = ap.parse_args()
    r = analyze(a.rpx)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(r, f, indent=2)
    md = to_markdown(r)
    if a.md:
        with open(a.md, "w") as f:
            f.write(md)
    # Kurzfassung in der Konsole
    print("\n".join(md.split("\n## ")[0].splitlines()))


if __name__ == "__main__":
    main()
