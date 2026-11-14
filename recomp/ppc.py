"""PowerPC (Espresso / Gekko) Instruktions-Decoder."""


def bits(x, a, b):
    """Bits a..b (IBM-Nummerierung, 0 = MSB) eines 32-bit Worts."""
    return (x >> (31 - b)) & ((1 << (b - a + 1)) - 1)


def simm16(x):
    v = x & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


class Ins:
    __slots__ = ("addr", "raw", "op", "xo", "name",
                 "rD", "rA", "rB", "rC", "imm", "uimm", "target", "BO", "BI",
                 "LK", "AA", "Rc", "OE", "SH", "MB", "ME", "crfD", "crfS", "spr", "W", "I")

    def __init__(self, addr, raw):
        self.addr, self.raw = addr, raw
        self.op = raw >> 26
        self.rD = bits(raw, 6, 10)
        self.rA = bits(raw, 11, 15)
        self.rB = bits(raw, 16, 20)
        self.rC = bits(raw, 21, 25)
        self.imm = simm16(raw)
        self.uimm = raw & 0xFFFF
        self.Rc = raw & 1
        self.LK = raw & 1
        self.AA = (raw >> 1) & 1
        self.OE = bits(raw, 21, 21)
        self.SH = bits(raw, 16, 20)
        self.MB = bits(raw, 21, 25)
        self.ME = bits(raw, 26, 30)
        self.BO = self.rD
        self.BI = self.rA
        self.crfD = bits(raw, 6, 8)
        self.crfS = bits(raw, 11, 13)
        self.spr = (bits(raw, 16, 20) << 5) | bits(raw, 11, 15)
        self.xo = 0
        self.target = None
        self.W = 0
        self.I = 0
        self.name = decode_name(self)


_OP = {
    7: "mulli", 8: "subfic", 10: "cmpli", 11: "cmpi", 12: "addic", 13: "addic.",
    14: "addi", 15: "addis", 16: "bc", 17: "sc", 18: "b", 20: "rlwimi", 21: "rlwinm",
    23: "rlwnm", 24: "ori", 25: "oris", 26: "xori", 27: "xoris", 28: "andi.", 29: "andis.",
    32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu", 36: "stw", 37: "stwu", 38: "stb", 39: "stbu",
    40: "lhz", 41: "lhzu", 42: "lha", 43: "lhau", 44: "sth", 45: "sthu", 46: "lmw", 47: "stmw",
    48: "lfs", 49: "lfsu", 50: "lfd", 51: "lfdu", 52: "stfs", 53: "stfsu", 54: "stfd", 55: "stfdu",
    56: "psq_l", 57: "psq_lu", 60: "psq_st", 61: "psq_stu",
}

_OP19 = {16: "bclr", 528: "bcctr", 0: "mcrf", 150: "isync", 33: "crnor", 129: "crandc",
         193: "crxor", 225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror",
         50: "rfi"}

_OP31 = {
    0: "cmp", 32: "cmpl", 4: "tw", 8: "subfc", 10: "addc", 11: "mulhwu", 19: "mfcr",
    20: "lwarx", 23: "lwzx", 24: "slw", 26: "cntlzw", 28: "and", 40: "subf", 54: "dcbst",
    55: "lwzux", 60: "andc", 75: "mulhw", 83: "mfmsr", 86: "dcbf", 87: "lbzx", 104: "neg",
    119: "lbzux", 124: "nor", 136: "subfe", 138: "adde", 144: "mtcrf", 146: "mtmsr",
    150: "stwcx.", 151: "stwx", 183: "stwux", 200: "subfze", 202: "addze", 215: "stbx",
    232: "subfme", 234: "addme", 235: "mullw", 246: "dcbtst", 247: "stbux", 266: "add",
    278: "dcbt", 279: "lhzx", 284: "eqv", 311: "lhzux", 316: "xor", 339: "mfspr",
    343: "lhax", 371: "mftb", 375: "lhaux", 407: "sthx", 412: "orc", 439: "sthux",
    444: "or", 459: "divwu", 467: "mtspr", 470: "dcbi", 476: "nand", 491: "divw",
    534: "lwbrx", 535: "lfsx", 536: "srw", 567: "lfsux", 595: "mfsr", 598: "sync",
    599: "lfdx", 631: "lfdux", 662: "stwbrx", 663: "stfsx", 695: "stfsux", 727: "stfdx",
    759: "stfdux", 790: "lhbrx", 792: "sraw", 824: "srawi", 854: "eieio", 918: "sthbrx",
    922: "extsh", 954: "extsb", 982: "icbi", 983: "stfiwx", 1014: "dcbz", 597: "lswi",
    725: "stswi", 533: "lswx", 661: "stswx",
}

_OP59 = {18: "fdivs", 20: "fsubs", 21: "fadds", 24: "fres", 25: "fmuls", 28: "fmsubs",
         29: "fmadds", 30: "fnmsubs", 31: "fnmadds"}

_OP63_10 = {0: "fcmpu", 12: "frsp", 14: "fctiw", 15: "fctiwz", 32: "fcmpo", 38: "mtfsb1",
            40: "fneg", 64: "mcrfs", 70: "mtfsb0", 72: "fmr", 134: "mtfsfi", 136: "fnabs",
            264: "fabs", 583: "mffs", 711: "mtfsf"}
_OP63_5 = {18: "fdiv", 20: "fsub", 21: "fadd", 23: "fsel", 25: "fmul", 26: "frsqrte",
           28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd"}

_OP4_10 = {0: "ps_cmpu0", 32: "ps_cmpo0", 40: "ps_neg", 64: "ps_cmpu1", 72: "ps_mr",
           96: "ps_cmpo1", 136: "ps_nabs", 264: "ps_abs", 528: "ps_merge00",
           560: "ps_merge01", 592: "ps_merge10", 624: "ps_merge11", 1014: "dcbz_l"}
_OP4_5 = {10: "ps_sum0", 11: "ps_sum1", 12: "ps_muls0", 13: "ps_muls1", 14: "ps_madds0",
          15: "ps_madds1", 18: "ps_div", 20: "ps_sub", 21: "ps_add", 23: "ps_sel",
          24: "ps_res", 25: "ps_mul", 26: "ps_rsqrte", 28: "ps_msub", 29: "ps_madd",
          30: "ps_nmsub", 31: "ps_nmadd"}
_OP4_6 = {6: "psq_lx", 38: "psq_lux", 7: "psq_stx", 39: "psq_stux"}


def decode_name(i: Ins):
    op, raw = i.op, i.raw
    if raw == 0:
        return "<zero>"
    if op == 18:
        off = raw & 0x03FFFFFC
        if off & 0x02000000:
            off -= 0x04000000
        i.target = off if i.AA else (i.addr + off) & 0xFFFFFFFF
        return "b"
    if op == 16:
        off = raw & 0xFFFC
        if off & 0x8000:
            off -= 0x10000
        i.target = off if i.AA else (i.addr + off) & 0xFFFFFFFF
        return "bc"
    if op in _OP:
        return _OP[op]
    if op == 19:
        i.xo = bits(raw, 21, 30)
        return _OP19.get(i.xo, f"op19_{i.xo}")
    if op == 31:
        i.xo = bits(raw, 22, 30) if bits(raw, 22, 30) in (8, 10, 11, 40, 75, 104, 136, 138, 200,
                                                          202, 232, 234, 235, 266, 459, 491) \
            else bits(raw, 21, 30)
        return _OP31.get(i.xo, f"op31_{i.xo}")
    if op == 59:
        i.xo = bits(raw, 26, 30)
        return _OP59.get(i.xo, f"op59_{i.xo}")
    if op == 63:
        x10 = bits(raw, 21, 30)
        if x10 in _OP63_10:
            i.xo = x10
            return _OP63_10[x10]
        i.xo = bits(raw, 26, 30)
        return _OP63_5.get(i.xo, f"op63_{x10}")
    if op == 4:
        x10 = bits(raw, 21, 30)
        if x10 in _OP4_10:
            i.xo = x10
            return _OP4_10[x10]
        x6 = bits(raw, 25, 30)
        if x6 in _OP4_6:
            i.xo = x6
            i.W, i.I = bits(raw, 21, 21), bits(raw, 22, 24)
            return _OP4_6[x6]
        i.xo = bits(raw, 26, 30)
        return _OP4_5.get(i.xo, f"op4_{x10}")
    if op in (56, 57, 60, 61):
        i.W, i.I = bits(raw, 16, 16), bits(raw, 17, 19)
    return f"op{op}"
