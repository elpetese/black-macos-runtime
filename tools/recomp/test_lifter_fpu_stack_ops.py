"""x87 and MXCSR forms Black reaches that used to lift to a bare comment or a
wrong-width access. Found by running every SSE/x87 form in Black's XBE on an
emulator (Unicorn) against the lifted C.

- `fptan; ffree st(0); fincstp; fstp [m]` is how MSVC drops the 1.0 fptan
  pushes. With ffree/fincstp dropped, the fstp stored 1.0 -- tan() was 1 at
  every one of Black's field-of-view sites -- and the stack leaked a slot.
- fld/fstp tbyte were read and written as a float.
- ficom/ficomp compared nothing.
- cvttss2si of a NaN or out-of-range value is 0x80000000 on x86, not whatever
  the host's C cast gives.
- stmxcsr/ldmxcsr were dropped, so `stmxcsr [m]; or [m]..; ldmxcsr [m]` read
  stack garbage.
"""
import os
import shutil
import subprocess
import sys
import tempfile

import pytest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp.disasm import Disassembler  # noqa: E402
from tools.recomp.lifter import Lifter  # noqa: E402
from tools.recomp.translator import FP_STACK_MACROS  # noqa: E402

RUNTIME = os.path.join(os.path.dirname(__file__), "..", "..", "templates", "runtime")


def _lift(hexbytes):
    d = Disassembler()
    out = []
    for cs in d._cs.disasm(bytes.fromhex(hexbytes), 0x1000):
        out += Lifter().lift_instruction(d._decode_instruction(cs))
    return out


def test_no_form_falls_back_to_a_comment():
    for hx in ("ddc0", "d9f7", "d9f6", "dbe2", "da10", "da18",
               "db2d00100000", "db3d00100000", "0fae18", "0fae10"):
        for line in _lift(hx):
            assert "/* FPU:" not in line and "RECOMP_UNIMPL" not in line, (hx, line)


@pytest.mark.skipif(shutil.which("cc") is None, reason="needs a C compiler")
def test_fptan_idiom_stores_the_tangent_and_balances_the_stack():
    # fld dword [0x100]; fptan; ffree st(0); fincstp; fstp dword [0x104]
    body = _lift("d90500010000" "d9f2" "ddc0" "d9f7" "d91d04010000")
    # fld tbyte [0x200]; fstp tbyte [0x210]  (round trip)
    body += _lift("db2d00020000" "db3d10020000")
    # cvttss2si eax, xmm0 with xmm0 = NaN
    body += _lift("f30f2cc0")
    src = "\n".join([
        "#define RECOMP_GENERATED_CODE 1", '#include "recomp_types.h"',
        "#include <stdio.h>", "#include <string.h>",
        "ptrdiff_t g_xbox_mem_offset;",
        "RECOMP_TLS uint32_t g_eax,g_ecx,g_edx,g_esp,g_ebx,g_esi,g_edi;",
        "RECOMP_TLS double g_fp_stack[8]; RECOMP_TLS int g_fp_top;",
        "RECOMP_TLS int g_fp_cmp; RECOMP_TLS uint16_t g_fp_cc, g_fp_control_word = 0x37F;",
        "RECOMP_TLS RecompXmm g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7;",
        "double recomp_fptan(double x, uint32_t va){ (void)va; return tan(x); }",
        "static uint8_t mem[0x1000];",
        "int main(void){",
        "g_xbox_mem_offset = (ptrdiff_t)mem;",
        "*(float*)(mem+0x100) = 0.5f;",
        # 3.0 as an 80-bit extended real
        "static const uint8_t t3[10] = {0,0,0,0,0,0,0,0xC0,0x00,0x40};",
        "memcpy(mem+0x200, t3, 10);",
        "g_xmm0.u[0] = 0x7FC00000u;",
    ] + FP_STACK_MACROS + body + [
        'printf("%.6f %d %d %08X\\n", *(float*)(mem+0x104), g_fp_top,'
        ' memcmp(mem+0x200, mem+0x210, 10), g_eax);',
        "return 0; }",
    ])
    with tempfile.TemporaryDirectory() as d:
        c, exe = os.path.join(d, "t.c"), os.path.join(d, "t")
        open(c, "w").write(src)
        subprocess.check_call(["cc", "-w", "-I", RUNTIME, c, "-o", exe, "-lm"])
        tan, top, cmp80, cvt = subprocess.check_output([exe]).decode().split()
    assert abs(float(tan) - 0.546302) < 1e-5
    assert top == "0"
    assert cmp80 == "0"
    assert cvt == "80000000"
