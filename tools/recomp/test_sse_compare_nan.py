"""Branches after comiss/ucomiss must agree with the hardware flags for a NaN.

COMISS sets (ZF, PF, CF) to: greater 000, less 001, equal 100, unordered 111.
C treats every ordered comparison with a NaN as false, so `a < b` for jb,
`a <= b` for jbe and `a == b` for je fell through where the CPU jumps, and
`a != b` for jne was true where the CPU falls through. This compiles the
conditions the lifter emits and checks all four outcomes against the flags.
"""
import os
import shutil
import subprocess
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Operand  # noqa: E402
from tools.recomp.lifter import _make_condition  # noqa: E402

# (name, ZF, PF, CF) per outcome
OUTCOMES = {"greater": (0, 0, 0), "less": (0, 0, 1),
            "equal": (1, 0, 0), "unordered": (1, 1, 1)}
# jcc -> the flags it reads, as a function of (ZF, PF, CF)
JCC = {
    "ja":  lambda z, p, c: not c and not z,
    "jae": lambda z, p, c: not c,
    "jb":  lambda z, p, c: bool(c),
    "jbe": lambda z, p, c: bool(c or z),
    "je":  lambda z, p, c: bool(z),
    "jne": lambda z, p, c: not z,
    "jp":  lambda z, p, c: bool(p),
    "jnp": lambda z, p, c: not p,
}
VALUES = {"greater": ("2.0", "1.0"), "less": ("1.0", "2.0"),
          "equal": ("1.0", "1.0"), "unordered": ("(0.0/0.0)", "1.0")}
OPS = [Operand(type="reg", reg="xmm0"), Operand(type="reg", reg="xmm1")]


@pytest.mark.skipif(shutil.which("clang") is None and shutil.which("cc") is None,
                    reason="needs a C compiler")
@pytest.mark.parametrize("setter", ["comiss", "ucomisd"])
def test_every_branch_matches_the_hardware_flags(setter):
    lines = ["#include <stdio.h>", "int main(void){ double _fca, _fcb;"]
    expect = []
    for jcc, fn in JCC.items():
        cond, _ = _make_condition(jcc, setter, OPS)
        for name, (z, p, c) in OUTCOMES.items():
            a, b = VALUES[name]
            lines.append(f"_fca = {a}; _fcb = {b};")
            lines.append(f'printf("%d", (int)({cond}));')
            expect.append((jcc, name, int(fn(z, p, c))))
    lines.append("return 0; }")
    with tempfile.TemporaryDirectory() as d:
        src, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
        open(src, "w").write("\n".join(lines))
        cc = shutil.which("clang") or shutil.which("cc")
        subprocess.check_call([cc, "-w", "-O0", src, "-o", exe])
        got = subprocess.check_output([exe]).decode()
    bad = [(j, n, w, g) for (j, n, w), g in zip(expect, got) if str(w) != g]
    assert not bad, f"branch, outcome, hardware, lifted: {bad}"


def test_packed_sqrt_rsqrt_rcp_write_all_four_lanes():
    from tools.recomp.disasm import Instruction
    from tools.recomp.lifter import Lifter
    for m, helper in (("sqrtps", "XMM_SQRT"), ("rsqrtps", "XMM_RSQRT"),
                      ("rcpps", "XMM_RCP")):
        insn = Instruction(0x10, 3, m, "xmm0, xmm1", "0f5100",
                           operands=[Operand(type="reg", reg="xmm0"),
                                     Operand(type="reg", reg="xmm1")])
        out = " ".join(Lifter().lift_instruction(insn))
        assert f"xmm0 = {helper}(xmm1);" in out, (m, out)
        assert ".f[0]" not in out, (m, out)


# `comiss; lahf; test ah, mask; jcc` -- MSVC's float-compare idiom. lahf used
# to lift to a comment, leaving AH as whatever eax held.
@pytest.mark.skipif(shutil.which("cc") is None, reason="needs a C compiler")
def test_lahf_after_comiss_matches_the_hardware_flags():
    from tools.recomp.lifter import _lift_lahf
    stmt = _lift_lahf("comiss", OPS)[0]
    lines = ["#include <stdint.h>", "#include <stdio.h>",
             "#define RECOMP_PARITY8(x) (!__builtin_parity((x) & 0xFF))",
             "int main(void){ double _fca, _fcb; uint32_t eax;"]
    expect = []
    for name, (z, p, c) in OUTCOMES.items():
        a, b = VALUES[name]
        lines.append(f"_fca = {a}; _fcb = {b}; eax = 0xDEADBEEFu;")
        lines.append(stmt)
        lines.append('printf("%02X ", (eax >> 8) & 0xFF);')
        expect.append("%02X" % (0x02 | (z << 6) | (p << 2) | c))
    lines.append("return 0; }")
    with tempfile.TemporaryDirectory() as d:
        src, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
        open(src, "w").write("\n".join(lines))
        subprocess.check_call([shutil.which("cc"), "-w", src, "-o", exe])
        got = subprocess.check_output([exe]).decode().split()
    assert got == expect
