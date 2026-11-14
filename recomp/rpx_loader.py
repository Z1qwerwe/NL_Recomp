"""Laedt eine Wii-U-.rpx: Sections, Symbole, Imports, Relocations.

Die RPX ist auf feste virtuelle Adressen gelinkt (.text ab 0x02000000, Daten ab
0x10000000). Diese Adressen behalten wir 1:1 in der Runtime bei, deshalb muessen
die Relocations nur einmal angewendet werden, mit S = Symbolwert aus der Datei.
"""
import struct
import zlib
from dataclasses import dataclass, field

SHT_SYMTAB, SHT_NOBITS, SHT_RELA = 2, 8, 4
SHT_RPL_IMPORTS = 0x80000002
SHF_RPL_ZLIB = 0x08000000
SHF_EXEC = 0x4

R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA = 1, 4, 5, 6
R_PPC_REL24, R_PPC_REL14, R_PPC_REL32 = 10, 11, 26
R_PPC_GHS_REL16_HA, R_PPC_GHS_REL16_HI, R_PPC_GHS_REL16_LO = 251, 252, 253


@dataclass
class Section:
    idx: int
    name: str
    type: int
    flags: int
    addr: int
    size: int
    link: int
    info: int
    data: bytearray = field(default_factory=bytearray)


@dataclass
class Import:
    lib: str
    name: str
    addr: int
    is_func: bool


@dataclass
class Rpx:
    entry: int
    sections: list
    func_syms: list            # [(addr, size)]
    imports: dict              # addr -> Import
    reloc_stats: dict
    text_refs: set = field(default_factory=set)
    branch_fixups: dict = field(default_factory=dict)   # Befehlsadresse -> echtes Sprungziel (Imports)
    import_imm_sites: dict = field(default_factory=dict)  # Befehlsadresse -> 'ha'/'hi'/'lo' (Immediate zeigt auf Import)
    sda_base: int = 0
    sda2_base: int = 0
    stack_size: int = 0x100000

    def section_at(self, addr):
        for s in self.sections:
            if s.addr and s.addr <= addr < s.addr + s.size and s.type != SHT_RELA:
                return s
        return None

    @property
    def text(self):
        return next(s for s in self.sections if s.name == ".text")

    def read32(self, addr):
        s = self.section_at(addr)
        return struct.unpack_from(">I", s.data, addr - s.addr)[0]


def load(path) -> Rpx:
    raw = open(path, "rb").read()
    assert raw[:4] == b"\x7fELF" and raw[4] == 1 and raw[5] == 2, "keine 32-bit BE ELF"
    entry, = struct.unpack_from(">I", raw, 0x18)
    shoff, = struct.unpack_from(">I", raw, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", raw, 0x2E)

    secs = []
    for i in range(shnum):
        name_off, typ, flags, addr, off, size, link, info, _al, _es = \
            struct.unpack_from(">10I", raw, shoff + i * shentsize)
        if typ == SHT_NOBITS or off == 0:
            d = bytearray()
        else:
            d = raw[off:off + size]
            if flags & SHF_RPL_ZLIB:
                d = zlib.decompress(d[4:])
            d = bytearray(d)
            size = len(d)
        s = Section(i, str(name_off), typ, flags, addr, size, link, info, d)
        secs.append(s)
    strtab = secs[shstrndx].data
    for s in secs:
        o = int(s.name)
        s.name = strtab[o:strtab.index(b"\0", o)].decode()

    # --- Symbole -------------------------------------------------------------
    symtab = next(s for s in secs if s.type == SHT_SYMTAB)
    symstr = secs[symtab.link].data
    syms = []
    for o in range(0, len(symtab.data), 16):
        no, val, size, info, _other, shn = struct.unpack_from(">IIIBBH", symtab.data, o)
        name = symstr[no:symstr.index(b"\0", no)].decode("ascii", "replace") if no else ""
        syms.append((name, val, size, info & 0xF, shn))

    text_idx = next(s.idx for s in secs if s.name == ".text")
    func_syms = sorted({(v, sz) for (_n, v, sz, t, shn) in syms if t == 2 and shn == text_idx and sz})

    imports = {}
    for name, val, _sz, _t, shn in syms:
        if 0 < shn < len(secs) and secs[shn].type == SHT_RPL_IMPORTS and name:
            sec = secs[shn].name
            lib = sec.split("_", 1)[1]
            lib = lib[:-4] if lib.endswith(".rpl") else lib
            imports[val] = Import(lib, name, val, sec.startswith(".fimport"))

    # --- Relocations ---------------------------------------------------------
    stats = {}
    text_sec = secs[text_idx]
    text_refs = set()
    branch_fixups = {}
    import_imm_sites = {}
    import_secs = {s.idx for s in secs if s.type == SHT_RPL_IMPORTS}
    for rs in secs:
        if rs.type != SHT_RELA:
            continue
        target = secs[rs.info]
        d = rs.data
        for o in range(0, len(d), 12):
            r_off, r_info, addend = struct.unpack_from(">IIi", d, o)
            rtype, sidx = r_info & 0xFF, r_info >> 8
            S = syms[sidx][1]
            P = r_off
            v = (S + addend) & 0xFFFFFFFF
            loc = r_off - target.addr
            buf = target.data
            stats[rtype] = stats.get(rtype, 0) + 1
            if target is text_sec and syms[sidx][4] in import_secs and rtype in (4, 5, 6):
                import_imm_sites[r_off - 2] = {4: "lo", 5: "hi", 6: "ha"}[rtype]
            if rtype in (R_PPC_ADDR32,):
                struct.pack_into(">I", buf, loc, v)
                if text_sec.addr <= v < text_sec.addr + text_sec.size:
                    text_refs.add(v)
            elif rtype == R_PPC_REL32:
                struct.pack_into(">I", buf, loc, (v - P) & 0xFFFFFFFF)
            elif rtype == R_PPC_ADDR16_LO:
                struct.pack_into(">H", buf, loc, v & 0xFFFF)
            elif rtype == R_PPC_ADDR16_HI:
                struct.pack_into(">H", buf, loc, v >> 16)
            elif rtype == R_PPC_ADDR16_HA:
                struct.pack_into(">H", buf, loc, ((v + 0x8000) >> 16) & 0xFFFF)
            elif rtype == R_PPC_REL24:
                if v - P >= 0x02000000 or P - v > 0x02000000:
                    branch_fixups[P] = v   # zu weit fuer bl: auf der Konsole ein Trampolin
                ins, = struct.unpack_from(">I", buf, loc)
                delta = (v - P) & 0x03FFFFFC
                struct.pack_into(">I", buf, loc, (ins & 0xFC000003) | delta)
            elif rtype == R_PPC_REL14:
                ins, = struct.unpack_from(">I", buf, loc)
                delta = (v - P) & 0xFFFC
                struct.pack_into(">I", buf, loc, (ins & 0xFFFF0003) | delta)
            elif rtype == R_PPC_GHS_REL16_HA:
                struct.pack_into(">H", buf, loc, (((v - P) + 0x8000) >> 16) & 0xFFFF)
            elif rtype == R_PPC_GHS_REL16_HI:
                struct.pack_into(">H", buf, loc, ((v - P) >> 16) & 0xFFFF)
            elif rtype == R_PPC_GHS_REL16_LO:
                struct.pack_into(">H", buf, loc, (v - P) & 0xFFFF)
            else:
                raise ValueError(f"Unbekannter Relocation-Typ {rtype} bei {r_off:08x}")

    # ADDR16_HA/LO-Paare (lis/addi) koennen auch Funktionszeiger bilden
    for name, val, _sz, t, shn in syms:
        if shn == text_idx and t == 2:
            text_refs.add(val)

    rpx = Rpx(entry, secs, func_syms, imports, stats, text_refs, branch_fixups, import_imm_sites)
    fi = next((s for s in secs if s.type == 0x80000004), None)
    if fi and len(fi.data) >= 0x30:
        rpx.sda_base, rpx.sda2_base, rpx.stack_size = struct.unpack_from(">III", fi.data, 0x24)
    return rpx


def write_image(rpx: Rpx, path):
    """Schreibt ein einfaches Speicherabbild fuer die Runtime.

    Format: 'NLIM', entry, count, dann pro Segment: addr, size, filesize, bytes.
    Import-Sections werden mit 0 gefuellt, die Runtime setzt dort ihre Werte.
    """
    segs = []
    for s in rpx.sections:
        if not s.addr or s.addr >= 0xC0000000 and s.type != SHT_RPL_IMPORTS:
            continue
        if s.type in (SHT_RELA, SHT_SYMTAB):
            continue
        payload = b"" if s.type in (SHT_NOBITS, SHT_RPL_IMPORTS) else bytes(s.data)
        segs.append((s.addr, s.size, payload))
    with open(path, "wb") as f:
        f.write(b"NLIM" + struct.pack("<II", rpx.entry, len(segs)))
        for addr, size, payload in segs:
            f.write(struct.pack("<III", addr, size, len(payload)))
            f.write(payload)
