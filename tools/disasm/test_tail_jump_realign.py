"""A jump target the sweep stepped over, and an immediate that is really data.

Black's XInitDevices (0x0027D058) sits right after a run of data in the XPP
section. The linear sweep leaves that run one byte out of phase, so 0x0027D058
is never an instruction boundary, and the tail-jump pass dropped the only jump
to it -- the thunk every caller goes through. The function became a stub that
returns at once and the title's USB stack never started.

The address in front of it, 0x0027D028, is a status byte the driver writes
(`mov byte ptr [27D028h], 2`) and also takes by address. The immediate pass
probed it as code, ran on into XInitDevices, and started a function there that
covered the real entry.

    python3 -m unittest tools/disasm/test_tail_jump_realign.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.functions import FunctionDetector  # noqa: E402


class _Insn:
    def __init__(self, addr, jump_target=None, imm_ref=None, memory_ref=None):
        self.address = addr
        self.is_jump = jump_target is not None
        self.is_cond_jump = False
        self.jump_target = jump_target
        self.imm_ref = imm_ref
        self.memory_ref = memory_ref


class _Section:
    executable = True
    virtual_addr = 0x0027CF20
    virtual_size = 0x60B8


class _Image:
    def get_section_at_va(self, addr):
        return _Section()


class _Func:
    def __init__(self, start, end):
        self.start = start
        self.end = end


class _Engine:
    def __init__(self, insns, bodies=()):
        self.instructions = {i.address: i for i in insns}
        self._bodies = set(bodies)
        self.decoded = []

    def probes_as_function_body(self, addr):
        return addr in self._bodies

    def probes_as_returning_body(self, addr):
        return addr in self._bodies

    def probes_as_vcall_thunk(self, addr):
        return False

    # Upstream's immediate-reference pass asks these before splitting a
    # decoded instruction; nothing in these fixtures is inside one.
    def instruction_covering(self, addr):
        return None

    def probes_as_prologue(self, addr):
        return False

    def probes_as_constant_stub(self, addr):
        return False

    def decode_at(self, addr):
        self.decoded.append(addr)
        self.instructions[addr] = _Insn(addr)
        return 1


def _detector(insns, functions, bodies=()):
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = _Engine(insns, bodies)
    det.image = _Image()
    det.functions = {f.start: f for f in functions}
    det._candidates = {}
    det._alias_entries = {}
    det.added = []
    det._add_candidate = lambda addr, conf, why: det.added.append((addr, why))
    det._pass_cond_branch_orphans = lambda bodies, starts: False
    return det


THUNK = 0x0027DD24          # jmp 0x0027D058
ENTRY = 0x0027D058          # XInitDevices
STATUS = 0x0027D028         # the driver's status byte


class TailJumpRealignTest(unittest.TestCase):
    def test_a_target_the_sweep_stepped_over_is_realigned(self):
        det = _detector([_Insn(THUNK, jump_target=ENTRY)],
                        [_Func(THUNK, THUNK + 5)], bodies={ENTRY})
        self.assertTrue(det._pass_tail_jump_targets([]))
        self.assertEqual(det.engine.decoded, [ENTRY])
        self.assertIn((ENTRY, "tail_jump_target"), det.added)

    def test_bytes_that_do_not_decode_as_a_body_are_left_alone(self):
        det = _detector([_Insn(THUNK, jump_target=ENTRY)],
                        [_Func(THUNK, THUNK + 5)], bodies=set())
        self.assertFalse(det._pass_tail_jump_targets([]))
        self.assertEqual(det.engine.decoded, [])
        self.assertEqual(det.added, [])


class ImmediateDataTest(unittest.TestCase):
    def test_an_address_also_used_as_a_memory_operand_is_data(self):
        insns = [_Insn(0x0027D2F2, imm_ref=STATUS),
                 _Insn(0x0027D2FD, memory_ref=STATUS)]
        det = _detector(insns, [], bodies={STATUS})
        self.assertFalse(det._pass_imm_ref_targets([_Section()]))
        self.assertEqual(det.added, [])

    def test_an_address_only_taken_as_an_immediate_is_still_a_function(self):
        det = _detector([_Insn(0x0027D2F2, imm_ref=STATUS)], [],
                        bodies={STATUS})
        self.assertTrue(det._pass_imm_ref_targets([_Section()]))
        self.assertIn((STATUS, "imm_ref_target"), det.added)


if __name__ == "__main__":
    unittest.main()
