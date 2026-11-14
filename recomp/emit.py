"""Uebersetzt PowerPC-Funktionen aus der RPX in C++-Code fuer die Runtime."""
import os
import struct

from ppc import Ins


def mask(mb, me):
    m = 0
    if mb <= me:
        for b in range(mb, me + 1):
            m |= 1 << (31 - b)
    else:
        for b in range(0, me + 1):
            m |= 1 << (31 - b)
        for b in range(mb, 32):
            m |= 1 << (31 - b)
    return m


def R(n):
    return f"c.r[{n}]"


def RA0(n):
    return "0u" if n == 0 else f"c.r[{n}]"


def F(n):
    return f"c.f[{n}]"


def P1(n):
    return f"c.ps1[{n}]"


def ea_d(i):
    return f"(uint32_t)({RA0(i.rA)} + {i.imm})"


def ea_x(i):
    return f"(uint32_t)({RA0(i.rA)} + c.r[{i.rB}])"


def rc0(i, val):
    return f" c.setcr0((int32_t){val});" if i.Rc else ""


class FuncEmitter:
    def __init__(self, rpx, func_starts, start, size):
        self.rpx = rpx
        self.func_starts = func_starts
        self.start, self.end = start, start + size
        t = rpx.text
        self.ins = []
        for a in range(start, start + size, 4):
            w, = struct.unpack_from(">I", t.data, a - t.addr)
            self.ins.append(Ins(a, w))
        self.labels = set()
        self.has_bctr = any(i.name == "bcctr" and not i.LK for i in self.ins)
        self._find_labels()

    def inside(self, a):
        return self.start <= a < self.end

    def _find_labels(self):
        prev_end_block = True
        for i in self.ins:
            if prev_end_block and self.has_bctr:
                self.labels.add(i.addr)
            if i.name in ("b", "bc") and not i.LK and self.inside(i.target):
                self.labels.add(i.target)
            prev_end_block = i.name in ("b", "bclr", "bcctr") or (i.name == "bc")

    # --- Sprung-Hilfen -----------------------------------------------------
    def cond(self, i):
        BO, BI = i.BO, i.BI
        parts, pre = [], ""
        if not (BO & 4):
            pre = "c.ctr--; "
            parts.append("c.ctr == 0" if BO & 2 else "c.ctr != 0")
        if not (BO & 16):
            bit = f"((c.cr >> {31 - BI}) & 1)"
            parts.append(bit if BO & 8 else f"!{bit}")
        return pre, (" && ".join(parts) if parts else "true")

    def jump(self, target, i):
        """Code fuer einen Sprung (ohne Link) zu target."""
        if self.inside(target) and target in self.labels:
            return f"goto L_{target:08x};"
        if target in self.rpx.imports:
            return f"hle_call(c, 0x{target:08x}); return;"
        if target in self.func_starts:
            return f"fn_{target:08x}(c); return;"
        return f"call_indirect(c, 0x{target:08x}); return;"

    def call(self, target, i):
        nxt = i.addr + 4
        if target in self.rpx.imports:
            return f"c.lr = 0x{nxt:08x}; hle_call(c, 0x{target:08x});"
        if target in self.func_starts:
            return f"c.lr = 0x{nxt:08x}; fn_{target:08x}(c);"
        return f"c.lr = 0x{nxt:08x}; call_indirect(c, 0x{target:08x});"

    def bctr_dispatch(self):
        cases = " ".join(f"case 0x{a:08x}: goto L_{a:08x};" for a in sorted(self.labels))
        return (f"switch (c.ctr) {{ {cases} default: call_indirect(c, c.ctr); return; }}")

    # --- Instruktionen -----------------------------------------------------
    def emit_ins(self, i):
        n = i.name
        d, a, b = i.rD, i.rA, i.rB
        nxt = i.addr + 4

        # Spruenge
        if i.addr in self.rpx.branch_fixups:
            i.target = self.rpx.branch_fixups[i.addr]
        if n == "b":
            return self.call(i.target, i) if i.LK else self.jump(i.target, i)
        if n == "bc":
            pre, cnd = self.cond(i)
            body = self.call(i.target, i) if i.LK else self.jump(i.target, i)
            return f"{pre}if ({cnd}) {{ {body} }}"
        if n == "bclr":
            pre, cnd = self.cond(i)
            if i.LK:
                return f"{pre}if ({cnd}) {{ uint32_t t = c.lr; c.lr = 0x{nxt:08x}; call_indirect(c, t); }}"
            return f"{pre}if ({cnd}) return;"
        if n == "bcctr":
            pre, cnd = self.cond(i)
            if i.LK:
                return f"{pre}if ({cnd}) {{ c.lr = 0x{nxt:08x}; call_indirect(c, c.ctr); }}"
            disp = self.bctr_dispatch()
            return f"{pre}if ({cnd}) {{ {disp} }}"

        # Integer-Arithmetik
        if n == "addi":
            return f"{R(d)} = {RA0(a)} + {i.imm};"
        if n == "addis":
            return f"{R(d)} = {RA0(a)} + 0x{(i.uimm << 16) & 0xFFFFFFFF:08x}u;"
        if n in ("addic", "addic."):
            s = f"{{ uint32_t x = {R(a)}; uint32_t r = x + {i.imm}; c.ca = r < x; {R(d)} = r;"
            return s + (" c.setcr0((int32_t)r);" if n == "addic." else "") + " }"
        if n == "subfic":
            return (f"{{ uint32_t x = {R(a)}; uint32_t r = (uint32_t){i.imm} - x; "
                    f"c.ca = x <= (uint32_t){i.imm}; {R(d)} = r; }}")
        if n == "mulli":
            return f"{R(d)} = (uint32_t)((int32_t){R(a)} * {i.imm});"
        if n == "add":
            return f"{R(d)} = {R(a)} + {R(b)};{rc0(i, R(d))}"
        if n == "addc":
            return f"{{ uint32_t r = {R(a)} + {R(b)}; c.ca = r < {R(a)}; {R(d)} = r; }}{rc0(i, R(d))}"
        if n == "adde":
            return (f"{{ uint64_t r = (uint64_t){R(a)} + {R(b)} + c.ca; c.ca = r >> 32; "
                    f"{R(d)} = (uint32_t)r; }}{rc0(i, R(d))}")
        if n == "addze":
            return f"{{ uint64_t r = (uint64_t){R(a)} + c.ca; c.ca = r >> 32; {R(d)} = (uint32_t)r; }}{rc0(i, R(d))}"
        if n == "addme":
            return (f"{{ uint64_t r = (uint64_t){R(a)} + c.ca + 0xFFFFFFFFu; c.ca = r >> 32; "
                    f"{R(d)} = (uint32_t)r; }}{rc0(i, R(d))}")
        if n == "subf":
            return f"{R(d)} = {R(b)} - {R(a)};{rc0(i, R(d))}"
        if n == "subfc":
            return (f"{{ uint32_t x = {R(a)}, y = {R(b)}; c.ca = x <= y; {R(d)} = y - x; }}"
                    f"{rc0(i, R(d))}")
        if n == "subfe":
            return (f"{{ uint64_t r = (uint64_t)(~{R(a)}) + {R(b)} + c.ca; c.ca = r >> 32; "
                    f"{R(d)} = (uint32_t)r; }}{rc0(i, R(d))}")
        if n == "subfze":
            return (f"{{ uint64_t r = (uint64_t)(~{R(a)}) + c.ca; c.ca = r >> 32; "
                    f"{R(d)} = (uint32_t)r; }}{rc0(i, R(d))}")
        if n == "subfme":
            return (f"{{ uint64_t r = (uint64_t)(~{R(a)}) + c.ca + 0xFFFFFFFFu; c.ca = r >> 32; "
                    f"{R(d)} = (uint32_t)r; }}{rc0(i, R(d))}")
        if n == "neg":
            return f"{R(d)} = (uint32_t)(-(int32_t){R(a)});{rc0(i, R(d))}"
        if n == "mullw":
            return f"{R(d)} = (uint32_t)((int32_t){R(a)} * (int32_t){R(b)});{rc0(i, R(d))}"
        if n == "mulhw":
            return f"{R(d)} = (uint32_t)(((int64_t)(int32_t){R(a)} * (int32_t){R(b)}) >> 32);{rc0(i, R(d))}"
        if n == "mulhwu":
            return f"{R(d)} = (uint32_t)(((uint64_t){R(a)} * {R(b)}) >> 32);{rc0(i, R(d))}"
        if n == "divw":
            return f"{R(d)} = ppc_divw({R(a)}, {R(b)});{rc0(i, R(d))}"
        if n == "divwu":
            return f"{R(d)} = {R(b)} ? {R(a)} / {R(b)} : 0;{rc0(i, R(d))}"

        # Logik (rS steht im rD-Feld, Ziel ist rA)
        logic = {"and": "&", "or": "|", "xor": "^"}
        if n in logic:
            if n == "or" and d == b and not i.Rc:
                return f"{R(a)} = {R(d)};"
            return f"{R(a)} = {R(d)} {logic[n]} {R(b)};{rc0(i, R(a))}"
        if n == "andc":
            return f"{R(a)} = {R(d)} & ~{R(b)};{rc0(i, R(a))}"
        if n == "orc":
            return f"{R(a)} = {R(d)} | ~{R(b)};{rc0(i, R(a))}"
        if n == "nor":
            return f"{R(a)} = ~({R(d)} | {R(b)});{rc0(i, R(a))}"
        if n == "nand":
            return f"{R(a)} = ~({R(d)} & {R(b)});{rc0(i, R(a))}"
        if n == "eqv":
            return f"{R(a)} = ~({R(d)} ^ {R(b)});{rc0(i, R(a))}"
        if n == "ori":
            if i.raw == 0x60000000:
                return ";"
            return f"{R(a)} = {R(d)} | 0x{i.uimm:x}u;"
        if n == "oris":
            return f"{R(a)} = {R(d)} | 0x{i.uimm << 16:x}u;"
        if n == "xori":
            return f"{R(a)} = {R(d)} ^ 0x{i.uimm:x}u;"
        if n == "xoris":
            return f"{R(a)} = {R(d)} ^ 0x{i.uimm << 16:x}u;"
        if n == "andi.":
            return f"{R(a)} = {R(d)} & 0x{i.uimm:x}u; c.setcr0((int32_t){R(a)});"
        if n == "andis.":
            return f"{R(a)} = {R(d)} & 0x{i.uimm << 16:x}u; c.setcr0((int32_t){R(a)});"
        if n == "extsb":
            return f"{R(a)} = (uint32_t)(int32_t)(int8_t){R(d)};{rc0(i, R(a))}"
        if n == "extsh":
            return f"{R(a)} = (uint32_t)(int32_t)(int16_t){R(d)};{rc0(i, R(a))}"
        if n == "cntlzw":
            return f"{R(a)} = ppc_cntlzw({R(d)});{rc0(i, R(a))}"
        if n == "slw":
            return f"{R(a)} = ({R(b)} & 0x20) ? 0 : {R(d)} << ({R(b)} & 31);{rc0(i, R(a))}"
        if n == "srw":
            return f"{R(a)} = ({R(b)} & 0x20) ? 0 : {R(d)} >> ({R(b)} & 31);{rc0(i, R(a))}"
        if n == "sraw":
            return f"{R(a)} = ppc_sraw(c, {R(d)}, {R(b)});{rc0(i, R(a))}"
        if n == "srawi":
            sh = i.SH
            return (f"{{ int32_t x = (int32_t){R(d)}; c.ca = (x < 0) && (x & {(1 << sh) - 1}); "
                    f"{R(a)} = (uint32_t)(x >> {sh}); }}{rc0(i, R(a))}")
        if n == "rlwinm":
            m = mask(i.MB, i.ME)
            return f"{R(a)} = rotl32({R(d)}, {i.SH}) & 0x{m:08x}u;{rc0(i, R(a))}"
        if n == "rlwnm":
            m = mask(i.MB, i.ME)
            return f"{R(a)} = rotl32({R(d)}, {R(b)} & 31) & 0x{m:08x}u;{rc0(i, R(a))}"
        if n == "rlwimi":
            m = mask(i.MB, i.ME)
            return (f"{R(a)} = (rotl32({R(d)}, {i.SH}) & 0x{m:08x}u) | ({R(a)} & 0x{~m & 0xFFFFFFFF:08x}u);"
                    f"{rc0(i, R(a))}")

        # Vergleiche
        crf = i.crfD
        if n == "cmpi":
            return f"c.setcr({crf}, (int32_t){R(a)}, (int32_t){i.imm});"
        if n == "cmpli":
            return f"c.setcr_u({crf}, {R(a)}, {i.uimm}u);"
        if n == "cmp":
            return f"c.setcr({crf}, (int32_t){R(a)}, (int32_t){R(b)});"
        if n == "cmpl":
            return f"c.setcr_u({crf}, {R(a)}, {R(b)});"

        # Loads/Stores
        loads = {"lwz": "ld32", "lbz": "ld8", "lhz": "ld16", "lha": "lds16"}
        for base, fn in loads.items():
            if n == base:
                return f"{R(d)} = {fn}({ea_d(i)});"
            if n == base + "u":
                return f"{{ uint32_t ea = {R(a)} + {i.imm}; {R(d)} = {fn}(ea); {R(a)} = ea; }}"
            if n == base + "x":
                return f"{R(d)} = {fn}({ea_x(i)});"
            if n == base + "ux":
                return f"{{ uint32_t ea = {R(a)} + {R(b)}; {R(d)} = {fn}(ea); {R(a)} = ea; }}"
        stores = {"stw": "st32", "stb": "st8", "sth": "st16"}
        for base, fn in stores.items():
            if n == base:
                return f"{fn}({ea_d(i)}, {R(d)});"
            if n == base + "u":
                return f"{{ uint32_t ea = {R(a)} + {i.imm}; {fn}(ea, {R(d)}); {R(a)} = ea; }}"
            if n == base + "x":
                return f"{fn}({ea_x(i)}, {R(d)});"
            if n == base + "ux":
                return f"{{ uint32_t ea = {R(a)} + {R(b)}; {fn}(ea, {R(d)}); {R(a)} = ea; }}"
        if n == "lwbrx":
            return f"{R(d)} = bswap32(ld32({ea_x(i)}));"
        if n == "lhbrx":
            return f"{R(d)} = bswap16((uint16_t)ld16({ea_x(i)}));"
        if n == "stwbrx":
            return f"st32({ea_x(i)}, bswap32({R(d)}));"
        if n == "sthbrx":
            return f"st16({ea_x(i)}, bswap16((uint16_t){R(d)}));"
        if n == "lmw":
            return f"{{ uint32_t ea = {ea_d(i)}; for (int k = {d}; k < 32; k++, ea += 4) c.r[k] = ld32(ea); }}"
        if n == "stmw":
            return f"{{ uint32_t ea = {ea_d(i)}; for (int k = {d}; k < 32; k++, ea += 4) st32(ea, c.r[k]); }}"
        if n in ("lswi", "stswi"):
            nb = b or 32
            fn = "ppc_lswi" if n == "lswi" else "ppc_stswi"
            return f"{fn}(c, {RA0(a)}, {d}, {nb});"
        if n == "lwarx":
            return f"{R(d)} = ld32({ea_x(i)}); c.reserve = {ea_x(i)};"
        if n == "stwcx.":
            return f"st32({ea_x(i)}, {R(d)}); c.setcr_flags(0, 2 | (c.so & 1));"

        # Float Loads/Stores
        if n in ("lfs", "lfsu", "lfsx", "lfsux"):
            ea = ea_d(i) if n in ("lfs", "lfsu") else ea_x(i)
            upd = n.endswith("u") or n.endswith("ux")
            ea = (f"{R(a)} + {i.imm}" if n == "lfsu" else f"{R(a)} + {R(b)}") if upd else ea
            s = f"{{ uint32_t ea = {ea}; {F(d)} = {P1(d)} = (double)ldf32(ea);"
            return s + (f" {R(a)} = ea; }}" if upd else " }")
        if n in ("lfd", "lfdu", "lfdx", "lfdux"):
            upd = n in ("lfdu", "lfdux")
            ea = ea_d(i) if n in ("lfd", "lfdu") else ea_x(i)
            if upd:
                ea = f"{R(a)} + {i.imm}" if n == "lfdu" else f"{R(a)} + {R(b)}"
            s = f"{{ uint32_t ea = {ea}; {F(d)} = ldf64(ea);"
            return s + (f" {R(a)} = ea; }}" if upd else " }")
        if n in ("stfs", "stfsu", "stfsx", "stfsux"):
            upd = n in ("stfsu", "stfsux")
            ea = ea_d(i) if n in ("stfs", "stfsu") else ea_x(i)
            if upd:
                ea = f"{R(a)} + {i.imm}" if n == "stfsu" else f"{R(a)} + {R(b)}"
            s = f"{{ uint32_t ea = {ea}; stf32(ea, (float){F(d)});"
            return s + (f" {R(a)} = ea; }}" if upd else " }")
        if n in ("stfd", "stfdu", "stfdx", "stfdux"):
            upd = n in ("stfdu", "stfdux")
            ea = ea_d(i) if n in ("stfd", "stfdu") else ea_x(i)
            if upd:
                ea = f"{R(a)} + {i.imm}" if n == "stfdu" else f"{R(a)} + {R(b)}"
            s = f"{{ uint32_t ea = {ea}; stf64(ea, {F(d)});"
            return s + (f" {R(a)} = ea; }}" if upd else " }")
        if n == "stfiwx":
            return f"st32({ea_x(i)}, (uint32_t)fbits({F(d)}));"

        # Float-Arithmetik
        fa, fb, fc = F(a), F(b), F(i.rC)
        single = {"fadds": f"{fa} + {fb}", "fsubs": f"{fa} - {fb}", "fmuls": f"{fa} * {fc}",
                  "fdivs": f"{fa} / {fb}", "fmadds": f"{fa} * {fc} + {fb}",
                  "fmsubs": f"{fa} * {fc} - {fb}", "fnmadds": f"-({fa} * {fc} + {fb})",
                  "fnmsubs": f"-({fa} * {fc} - {fb})", "fres": f"1.0 / {fb}"}
        if n in single:
            return f"{F(d)} = {P1(d)} = (double)(float)({single[n]});"
        double = {"fadd": f"{fa} + {fb}", "fsub": f"{fa} - {fb}", "fmul": f"{fa} * {fc}",
                  "fdiv": f"{fa} / {fb}", "fmadd": f"{fa} * {fc} + {fb}",
                  "fmsub": f"{fa} * {fc} - {fb}", "fnmadd": f"-({fa} * {fc} + {fb})",
                  "fnmsub": f"-({fa} * {fc} - {fb})", "frsqrte": f"1.0 / std::sqrt({fb})",
                  "fsel": f"({fa} >= 0.0) ? {fc} : {fb}", "fmr": fb, "fneg": f"-{fb}",
                  "fabs": f"std::fabs({fb})", "fnabs": f"-std::fabs({fb})"}
        if n in double:
            return f"{F(d)} = {double[n]};"
        if n == "frsp":
            return f"{F(d)} = {P1(d)} = (double)(float){fb};"
        if n in ("fctiwz", "fctiw"):
            fn = "ppc_fctiwz" if n == "fctiwz" else "ppc_fctiw"
            return f"{F(d)} = bitsf((uint64_t)(uint32_t){fn}({fb}));"
        if n in ("fcmpu", "fcmpo"):
            return f"c.setcr_f({crf}, {fa}, {fb});"
        if n == "mffs":
            return f"{F(d)} = bitsf((uint64_t)c.fpscr);"
        if n == "mtfsf":
            return f"c.fpscr = (uint32_t)fbits({fb});"
        if n in ("mtfsb0", "mtfsb1", "mtfsfi", "mcrfs"):
            return ";"

        # Paired Singles
        if n in ("psq_l", "psq_lu", "psq_st", "psq_stu", "psq_lx", "psq_lux", "psq_stx", "psq_stux"):
            x = n.endswith("x")
            upd = n in ("psq_lu", "psq_stu", "psq_lux", "psq_stux")
            if x:
                ea = f"{RA0(a) if not upd else R(a)} + {R(b)}"
            else:
                off = i.raw & 0xFFF
                off = off - 0x1000 if off & 0x800 else off
                ea = f"{RA0(a) if not upd else R(a)} + {off}"
            fn = "psq_load" if "_l" in n else "psq_store"
            s = f"{{ uint32_t ea = (uint32_t)({ea}); {fn}(c, ea, {d}, {i.W}, {i.I});"
            return s + (f" {R(a)} = ea; }}" if upd else " }")
        ps = self.emit_ps(i)
        if ps:
            return ps

        # Condition Register
        if n in ("crxor", "cror", "crand", "crnor", "creqv", "crandc", "crorc", "crnand"):
            op = {"crxor": "x ^ y", "cror": "x | y", "crand": "x & y", "crnor": "!(x | y)",
                  "creqv": "!(x ^ y)", "crandc": "x & !y", "crorc": "x | !y",
                  "crnand": "!(x & y)"}[n]
            return (f"{{ uint32_t x = (c.cr >> {31 - a}) & 1, y = (c.cr >> {31 - b}) & 1; "
                    f"c.setcrbit({d}, ({op}) & 1); }}")
        if n == "mcrf":
            return f"c.setcr_flags({crf}, (c.cr >> {28 - 4 * i.crfS}) & 0xF);"
        if n == "mfcr":
            return f"{R(d)} = c.cr;"
        if n == "mtcrf":
            crm = (i.raw >> 12) & 0xFF
            m = 0
            for k in range(8):
                if crm & (1 << (7 - k)):
                    m |= 0xF << (28 - 4 * k)
            return f"c.cr = (c.cr & 0x{~m & 0xFFFFFFFF:08x}u) | ({R(d)} & 0x{m:08x}u);"

        # SPRs
        if n == "mfspr":
            spr = i.spr
            if spr == 8:
                return f"{R(d)} = c.lr;"
            if spr == 9:
                return f"{R(d)} = c.ctr;"
            if spr == 1:
                return f"{R(d)} = c.get_xer();"
            if 912 <= spr <= 919:
                return f"{R(d)} = c.gqr[{spr - 912}];"
            return f"{R(d)} = ppc_mfspr(c, {spr});"
        if n == "mtspr":
            spr = i.spr
            if spr == 8:
                return f"c.lr = {R(d)};"
            if spr == 9:
                return f"c.ctr = {R(d)};"
            if spr == 1:
                return f"c.set_xer({R(d)});"
            if 912 <= spr <= 919:
                return f"c.gqr[{spr - 912}] = {R(d)};"
            return f"ppc_mtspr(c, {spr}, {R(d)});"
        if n == "mftb":
            return f"{R(d)} = ppc_mftb({i.spr});"
        if n == "mfmsr":
            return f"{R(d)} = c.msr;"
        if n == "mtmsr":
            return f"c.msr = {R(d)};"

        # Cache/Sync = nichts zu tun
        if n in ("sync", "isync", "eieio", "dcbst", "dcbf", "dcbt", "dcbtst", "dcbi", "icbi"):
            return ";"
        if n in ("dcbz", "dcbz_l"):
            return f"ppc_dcbz({ea_x(i)});"
        if n == "sc":
            return "ppc_syscall(c);"
        if n == "tw":
            return f"ppc_trap(c, 0x{i.addr:08x});"
        return None

    def emit_ps(self, i):
        n = i.name
        d, a, b, cc = i.rD, i.rA, i.rB, i.rC
        if not n.startswith("ps_"):
            return None

        def both(e0, e1):
            return f"{{ double t0 = {e0}, t1 = {e1}; {F(d)} = t0; {P1(d)} = t1; }}"

        def r(x):
            return f"(double)(float)({x})"
        fa, fb, fc = F(a), F(b), F(cc)
        pa, pb, pc = P1(a), P1(b), P1(cc)
        table = {
            "ps_add": (r(f"{fa} + {fb}"), r(f"{pa} + {pb}")),
            "ps_sub": (r(f"{fa} - {fb}"), r(f"{pa} - {pb}")),
            "ps_mul": (r(f"{fa} * {fc}"), r(f"{pa} * {pc}")),
            "ps_div": (r(f"{fa} / {fb}"), r(f"{pa} / {pb}")),
            "ps_madd": (r(f"{fa} * {fc} + {fb}"), r(f"{pa} * {pc} + {pb}")),
            "ps_msub": (r(f"{fa} * {fc} - {fb}"), r(f"{pa} * {pc} - {pb}")),
            "ps_nmadd": (r(f"-({fa} * {fc} + {fb})"), r(f"-({pa} * {pc} + {pb})")),
            "ps_nmsub": (r(f"-({fa} * {fc} - {fb})"), r(f"-({pa} * {pc} - {pb})")),
            "ps_muls0": (r(f"{fa} * {fc}"), r(f"{pa} * {fc}")),
            "ps_muls1": (r(f"{fa} * {pc}"), r(f"{pa} * {pc}")),
            "ps_madds0": (r(f"{fa} * {fc} + {fb}"), r(f"{pa} * {fc} + {pb}")),
            "ps_madds1": (r(f"{fa} * {pc} + {fb}"), r(f"{pa} * {pc} + {pb}")),
            "ps_sum0": (r(f"{fa} + {pb}"), pc),
            "ps_sum1": (fc, r(f"{fa} + {pb}")),
            "ps_sel": (f"({fa} >= 0.0) ? {fc} : {fb}", f"({pa} >= 0.0) ? {pc} : {pb}"),
            "ps_res": (r(f"1.0 / {fb}"), r(f"1.0 / {pb}")),
            "ps_rsqrte": (r(f"1.0 / std::sqrt({fb})"), r(f"1.0 / std::sqrt({pb})")),
            "ps_neg": (f"-{fb}", f"-{pb}"),
            "ps_abs": (f"std::fabs({fb})", f"std::fabs({pb})"),
            "ps_nabs": (f"-std::fabs({fb})", f"-std::fabs({pb})"),
            "ps_mr": (fb, pb),
            "ps_merge00": (fa, fb),
            "ps_merge01": (fa, pb),
            "ps_merge10": (pa, fb),
            "ps_merge11": (pa, pb),
        }
        if n in table:
            return both(*table[n])
        if n in ("ps_cmpu0", "ps_cmpo0"):
            return f"c.setcr_f({i.crfD}, {fa}, {fb});"
        if n in ("ps_cmpu1", "ps_cmpo1"):
            return f"c.setcr_f({i.crfD}, {pa}, {pb});"
        return None

    def emit(self):
        out = [f"void fn_{self.start:08x}(PPCContext& c) {{", f"  NL_ENTER(0x{self.start:08x});"]
        for i in self.ins:
            if i.addr in self.labels:
                out.append(f"L_{i.addr:08x}:")
            code = self.emit_ins(i)
            if code is None:
                code = f"ppc_unimpl(c, 0x{i.addr:08x}, 0x{i.raw:08x}); /* {i.name} */"
            out.append("  " + code)
        out.append("}")
        return "\n".join(out)


def add_secondary_entries(rpx, log=print):
    """Sprungziele mitten in Funktionen (z.B. GHS-Register-Sicherung) werden eigene Funktionen."""
    import bisect
    t = rpx.text
    starts = [a for a, _ in rpx.func_syms]
    ends = {a: a + sz for a, sz in rpx.func_syms}
    start_set = set(starts)
    extra = {}
    for off in range(0, t.size, 4):
        w, = struct.unpack_from(">I", t.data, off)
        if w >> 26 not in (16, 18):
            continue
        a = t.addr + off
        i = Ins(a, w)
        tgt = rpx.branch_fixups.get(a, i.target)
        if tgt is None or tgt in start_set or not (t.addr <= tgt < t.addr + t.size):
            continue
        k = bisect.bisect_right(starts, a) - 1
        own_start = starts[k] if k >= 0 else None
        same = own_start is not None and own_start <= tgt < ends[own_start]
        if same and not i.LK:
            continue  # normaler Sprung innerhalb der Funktion
        k2 = bisect.bisect_right(starts, tgt) - 1
        if k2 < 0:
            continue
        cs = starts[k2]
        if tgt < ends[cs]:
            extra[tgt] = ends[cs] - tgt
    rpx.func_syms = sorted(set(rpx.func_syms) | set(extra.items()))
    log(f"  {len(extra)} zusaetzliche Einsprungpunkte")
    return extra


class _LazyFile:
    """Schreibt nur, wenn sich der Inhalt geaendert hat (spart Neu-Kompilieren)."""
    def __init__(self, path):
        self.path, self.parts = path, []

    def write(self, s):
        self.parts.append(s)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        data = "".join(self.parts)
        try:
            with open(self.path, encoding="utf-8") as f:
                if f.read() == data:
                    return
        except OSError:
            pass
        with open(self.path, "w", encoding="utf-8") as f:
            f.write(data)


def generate(rpx, out_dir, funcs_per_file=1500, only=None, log=print):
    os.makedirs(out_dir, exist_ok=True)
    funcs = rpx.func_syms
    if only:
        funcs = [f for f in funcs if f[0] in only]
    func_starts = {a for a, _ in rpx.func_syms}

    with _LazyFile(os.path.join(out_dir, "funcs.h")) as h:
        h.write("#pragma once\n#include \"ppc_runtime.h\"\n")
        for a, _ in rpx.func_syms:
            h.write(f"void fn_{a:08x}(PPCContext& c);\n")

    files, unimpl = [], {}
    for k in range(0, len(funcs), funcs_per_file):
        name = f"recomp_{k // funcs_per_file:03d}.cpp"
        files.append(name)
        with _LazyFile(os.path.join(out_dir, name)) as f:
            f.write('#include "funcs.h"\n#include <cmath>\n\n')
            for a, sz in funcs[k:k + funcs_per_file]:
                fe = FuncEmitter(rpx, func_starts, a, sz)
                code = fe.emit()
                for i in fe.ins:
                    if fe.emit_ins(i) is None:
                        unimpl[i.name] = unimpl.get(i.name, 0) + 1
                f.write(code + "\n\n")
        log(f"  {name}: {min(k + funcs_per_file, len(funcs))}/{len(funcs)} Funktionen")

    # Funktionstabelle + Imports
    with _LazyFile(os.path.join(out_dir, "tables.cpp")) as f:
        f.write('#include "funcs.h"\n\n')
        f.write("const FuncEntry g_func_table[] = {\n")
        for a, _ in rpx.func_syms:
            f.write(f"  {{0x{a:08x}u, fn_{a:08x}}},\n")
        f.write("};\n")
        f.write(f"const size_t g_func_count = {len(rpx.func_syms)};\n\n")
        f.write("const ImportEntry g_import_table[] = {\n")
        for addr, imp in sorted(rpx.imports.items()):
            f.write(f'  {{0x{addr:08x}u, "{imp.lib}", "{imp.name}", {str(imp.is_func).lower()}}},\n')
        f.write("};\n")
        f.write(f"const size_t g_import_count = {len(rpx.imports)};\n")
        f.write(f"const uint32_t g_entry = 0x{rpx.entry:08x}u;\n")
        f.write(f"const uint32_t g_sda_base = 0x{rpx.sda_base:08x}u;\n")
        f.write(f"const uint32_t g_sda2_base = 0x{rpx.sda2_base:08x}u;\n")
        f.write(f"const uint32_t g_stack_size = 0x{rpx.stack_size:08x}u;\n")

    with _LazyFile(os.path.join(out_dir, "sources.cmake")) as f:
        f.write("set(GENERATED_SOURCES\n")
        for name in files + ["tables.cpp"]:
            f.write(f"  ${{GENERATED_DIR}}/{name}\n")
        f.write(")\n")
    return files, unimpl
