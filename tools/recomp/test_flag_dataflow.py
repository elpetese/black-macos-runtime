"""
Flag state across a loop's back edge.

Black's XMV decoder (sub_00240F38) ends with

    mov eax, 0xB
    sub eax, [ebp+0x20]
L:  je  done
    ...
    dec eax
    jmp L

The je reads ZF from `sub` on entry and from `dec` on every later pass. Blocks
used to be walked once in address order, so the back edge was still unknown at
L and the branch became the never-assigned `_flags` fallback: the loop could
not exit and walked edi off the mapped heap. Both predecessors define je as
`eax == 0`, which is what the translator must now emit -- and only when they
agree.

Run: python3 -m unittest tools.recomp.test_flag_dataflow
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
        entry_point=BASE, kernel_thunk_addr=BASE, origin="flag-dataflow-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def _loop(back_edge_dec):
    return bytes.fromhex(
        "B80B000000"      # 0:  mov eax, 0xB
        "2B4520"          # 5:  sub eax, [ebp+0x20]
        "7406"            # 8:  L: je 16
        "8D7F01"          # 10: lea edi, [edi+1]   (flags preserved)
        + back_edge_dec + # 13: dec eax / dec ecx
        "EBF8"            # 14: jmp L
        "C3")             # 16: ret


class FlagDataflowTest(unittest.TestCase):
    def test_loop_head_je_uses_flags_from_both_predecessors(self):
        code = _translate(_loop("48"))           # dec eax
        je = [l for l in code.splitlines() if "/* je" in l]
        self.assertEqual(len(je), 1, code)
        self.assertIn("(eax == 0)", je[0])
        self.assertNotIn("_flags /*", je[0])

    def test_disagreeing_predecessors_stay_unknown(self):
        code = _translate(_loop("49"))           # dec ecx
        je = [l for l in code.splitlines() if "/* je" in l]
        self.assertEqual(len(je), 1, code)
        self.assertIn("_flags", je[0])
        self.assertNotIn("(eax == 0)", je[0])
        self.assertNotIn("(ecx == 0)", je[0])


if __name__ == "__main__":
    unittest.main()
