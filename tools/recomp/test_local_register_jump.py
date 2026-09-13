"""
`jmp reg` back into the same function.

Hand-written asm (Black's XMV colour converter, sub_002401D2) calls a shared
block with `mov ebx, ret_label; jmp block` and the block returns with
`jmp ebx`. Lifted as an indirect tail call, the target is not a function, the
call fails, the frame is abandoned and the stack is smashed. The translator
now dispatches such a jump to the labels the register is loaded with.

Run: python3 -m unittest tools.recomp.test_local_register_jump
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
        entry_point=BASE, kernel_thunk_addr=BASE, origin="local-jump-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(image),
                 "_addr": BASE, "size": len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


IMAGE = bytes.fromhex(
    "BB0A000100"      # 0:  mov ebx, 0x0001000A   (return label)
    "E906000000"      # 5:  jmp 16
    "40"              # 10: inc eax              <- return point
    "C3"              # 11: ret
    "90909090"        # 12: padding
    "41"              # 16: inc ecx              <- shared block
    "FFE3")           # 17: jmp ebx


class LocalRegisterJumpTest(unittest.TestCase):
    def test_register_jump_dispatches_to_local_label(self):
        code = _translate(IMAGE)
        self.assertIn("loc_0001000A:", code)
        self.assertIn("if (_jt == 0x0001000Au) goto loc_0001000A;", code)

    def test_unknown_value_still_falls_back_to_tail_call(self):
        code = _translate(IMAGE)
        self.assertIn("RECOMP_ITAIL(_jt)", code)

    def test_constant_outside_function_is_not_a_local_target(self):
        image = bytearray(IMAGE)
        image[1:5] = (0x00400000).to_bytes(4, "little")   # mov ebx, elsewhere
        code = _translate(bytes(image))
        self.assertNotIn("local jmp", code)
        self.assertIn("RECOMP_ITAIL(ebx)", code)


if __name__ == "__main__":
    unittest.main()
