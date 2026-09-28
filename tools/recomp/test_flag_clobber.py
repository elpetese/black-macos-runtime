"""
A flag setter's operand overwritten before the branch reads the flags.

Black picks a mesh's blend mode in sub_00172EB0 with

    sub eax, 4
    mov eax, dword ptr [0x56f02c]
    je  subtractive

The je reads ZF from the `sub`. It used to be lifted as `if (eax == 0)` after
the load, so it tested the loaded value -- a D3D dirty flag that is usually
0 -- and every opaque mesh was drawn with the subtractive blend. The operand
must be saved before the `mov` and the branch must read the saved value.

Run: python3 -m unittest tools.recomp.test_flag_clobber
"""

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000


def _translate(image):
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="flag-clobber-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


class FlagClobberTest(unittest.TestCase):
    def test_je_after_sub_then_mov_to_same_register(self):
        code = _translate(bytes.fromhex(
            "83E804"          # 0: sub eax, 4
            "A12CF05600"      # 3: mov eax, [0x56f02c]
            "7401"            # 8: je 11
            "90"              # 10: nop
            "C3"))            # 11: ret
        je = [l for l in code.splitlines() if "/* je" in l]
        self.assertEqual(len(je), 1, code)
        # The branch reads the result saved at the sub, not eax after the load.
        self.assertIn("_fa == 0", je[0].replace("(", "").replace(")", ""), code)
        self.assertNotIn("eax == 0", je[0].replace("(", "").replace(")", ""), code)
        snap = code.index("_fa = (uint32_t)(eax)")
        self.assertLess(snap, code.index("MEM32(0x56F02C)"), code)

    def test_unrelated_mov_leaves_condition_alone(self):
        code = _translate(bytes.fromhex(
            "83E804"          # 0: sub eax, 4
            "8B0D2CF05600"    # 3: mov ecx, [0x56f02c]
            "7401"            # 9: je 12
            "90"              # 11: nop
            "C3"))            # 12: ret
        je = [l for l in code.splitlines() if "/* je" in l]
        self.assertEqual(len(je), 1, code)
        self.assertIn("_fa == 0", je[0].replace("(", "").replace(")", ""), code)
        self.assertNotIn("_fr = (uint32_t)", code)

    def test_byte_setter_keeps_its_width(self):
        code = _translate(bytes.fromhex(
            "FEC9"            # 0: dec cl
            "8B0D2CF05600"    # 2: mov ecx, [0x56f02c]
            "7801"            # 8: js 11
            "90"              # 10: nop
            "C3"))            # 11: ret
        js = [l for l in code.splitlines() if "/* js" in l]
        self.assertEqual(len(js), 1, code)
        # Sign of the 8-bit result, saved at the dec before ecx is reloaded.
        self.assertIn("_fas < 0", js[0], code)
        snap = code.index("_fas = (int32_t)(int8_t)(_fa)")
        self.assertLess(snap, code.index("MEM32(0x56F02C)"), code)


if __name__ == "__main__":
    unittest.main()
