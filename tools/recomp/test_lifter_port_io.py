"""
x86 port I/O lifting.

`in`/`out` used to be emitted as a TODO comment, so an `in al, dx` left al
holding whatever it held before. Black's D3D vblank ISR reads the interlaced
field from port 0x80C0 that way; the field never changed and the XMV intro
video waited forever for the other one.

Run: python3 -m unittest tools.recomp.test_lifter_port_io
"""

import pathlib
import unittest

from tools.recomp.disasm import Operand
from tools.recomp.lifter import Instruction, Lifter, next_flag_state

_HEADER = (pathlib.Path(__file__).resolve().parents[2]
           / "templates" / "runtime" / "recomp_types.h")


def _insn(mnemonic, op_str, operands):
    insn = Instruction(0x1000, 2, mnemonic, op_str, "")
    insn.operands = operands
    return insn


def _reg(name):
    return Operand(type="reg", reg=name)


class PortIoLiftTest(unittest.TestCase):
    def test_in_byte_from_dx(self):
        out = Lifter().lift_instruction(
            _insn("in", "al, dx", [_reg("al"), _reg("dx")]))
        self.assertEqual(len(out), 1)
        self.assertIn("SET_LO8(eax, recomp_port_in(LO16(edx), 1))", out[0])

    def test_in_dword_from_immediate_port(self):
        out = Lifter().lift_instruction(
            _insn("in", "eax, 0x1f", [_reg("eax"), Operand(type="imm", imm=0x1F)]))
        self.assertIn("eax = recomp_port_in(0x001Fu, 4)", out[0])

    def test_out_byte_to_dx(self):
        out = Lifter().lift_instruction(
            _insn("out", "dx, al", [_reg("dx"), _reg("al")]))
        self.assertIn("recomp_port_out(LO16(edx), 1, LO8(eax));", out[0])

    def test_port_io_preserves_flags(self):
        state = ("cmp", [_reg("eax"), _reg("ecx")])
        insn = _insn("in", "al, dx", [_reg("al"), _reg("dx")])
        self.assertEqual(next_flag_state(insn, state), state)

    def test_runtime_header_declares_the_helpers(self):
        header = _HEADER.read_text(encoding="utf-8")
        self.assertIn("uint32_t recomp_port_in(uint32_t port, uint32_t size);", header)
        self.assertIn("void recomp_port_out(uint32_t port, uint32_t size, uint32_t value);", header)


if __name__ == "__main__":
    unittest.main()
