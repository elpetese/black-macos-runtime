"""Comparison snapshots must survive a join of different CMP operands."""
from tools.recomp import config
from tools.recomp.translator import (FunctionTranslator, _merge_flag_states,
                                     _incoming_flag_state)
from tools.recomp.disasm import Operand

BASE = 0x10000

def translate_join(consumer=bytes.fromhex('0f95c0c3')):
    # test ecx,ecx; jz alternate; cmp eax,edx; jmp join; nop;
    # alternate: cmp ebx,esi; join: consumer.
    image = bytes.fromhex('85c9740539d0eb039039f3') + consumer
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                              entry_point=BASE, kernel_thunk_addr=BASE,
                              origin='flag-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])

def test_different_cmp_operands_join_for_setne():
    code = translate_join()
    assert 'CMP_NE(_fa, _fb)' in code, code
    assert '_flags /* setne */' not in code

def test_different_cmp_operands_join_for_cmovne():
    code = translate_join(bytes.fromhex('0f45c7c3'))
    assert 'if (CMP_NE(_fa, _fb)) eax = edi;' in code, code

def test_unknown_path_is_not_guessed():
    assert _merge_flag_states([None, ('cmp', [])]) is None

def test_mixed_operations_are_not_guessed():
    a = Operand(type='reg', reg='eax')
    b = Operand(type='reg', reg='edx')
    assert _merge_flag_states([('cmp', [a, b]), ('test', [a, b])]) is None

def test_mixed_widths_are_not_guessed():
    wide = [Operand(type='reg', reg='eax'), Operand(type='reg', reg='edx')]
    narrow = [Operand(type='reg', reg='al'), Operand(type='reg', reg='dl')]
    assert _merge_flag_states([('cmp', wide), ('cmp', narrow)]) is None


def test_sse_compares_from_different_operands_join():
    a = [Operand(type='reg', reg='xmm4'), Operand(type='mem', reg='')]
    b = [Operand(type='reg', reg='xmm0'), Operand(type='reg', reg='xmm4')]
    assert _merge_flag_states([('comiss', a), ('comiss', b)]) == ('comiss', a)

def test_sse_compare_does_not_join_with_a_different_setter():
    a = [Operand(type='reg', reg='xmm4'), Operand(type='reg', reg='xmm0')]
    assert _merge_flag_states([('comiss', a), ('cmp', a)]) is None
    assert _merge_flag_states([('comiss', a), ('ucomiss', a)]) is None


def test_test_and_cmp_joining_at_a_jne_answer_from_the_evaluated_zero_flag():
    # test byte [eax+0x30], 8 ; jmp J   /   cmp eax, 0 ; setne al ; J: test al, al ; jne
    # Reduced to the shape that matters: two different setters, one je/jne.
    #   0: test ecx, ecx ; jz 8       (setter A: test)
    #   4: cmp eax, edx  ; jmp 9      (setter B: cmp)
    #   8: (falls to) nop
    #   9: jne 0xd  ; nop ; ret
    image = bytes.fromhex('85c9740439d0eb00' '90' '7502' '90' 'c3')
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='zf-join-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image),
                 '_addr': BASE, 'size': len(image)}}
    code = FunctionTranslator(image, db).translate_function(BASE, db[BASE])
    assert '_fz = ((_fa & _fb) == 0);' in code or '_fz = (_fa == _fb);' in code, code


def test_cmp_and_test_state_merge_marker():
    a = [Operand(type='reg', reg='eax'), Operand(type='reg', reg='edx')]
    b = [Operand(type='reg', reg='ecx'), Operand(type='reg', reg='ecx')]
    # _merge_flag_states still refuses (a jl after the join would be wrong)...
    assert _merge_flag_states([('cmp', a), ('test', b)]) is None
    # ...but the block's incoming state carries ZF for a je/jne.
    known = {1: ('cmp', a), 2: ('test', b)}
    m = _incoming_flag_state([1, 2], known, False)
    assert m is not None and m[0] == '__zf_from_snapshot'
    assert _incoming_flag_state([1, 1], known, False) == ('cmp', a)
    # Different widths join too: each cmp/test writes _fz at its own width.
    # Black's sub_001DB910: `cmp ax, [edx+0x40]` (16-bit) on one path,
    # `test al, al` (8-bit, normalised to cmp al, 0) on the other, then je.
    narrow = [Operand(type='reg', reg='al'), Operand(type='reg', reg='al')]
    m = _incoming_flag_state([1, 3], {1: ('cmp', a), 3: ('test', narrow)}, False)
    assert m is not None and m[0] == '__zf_from_snapshot'
    word = [Operand(type='reg', reg='ax'), Operand(type='mem', mem_base='edx', mem_disp=0x40, mem_size=2)]
    byte0 = [Operand(type='reg', reg='al'), Operand(type='imm', imm=0)]
    m = _incoming_flag_state([1, 2], {1: ('cmp', word), 2: ('cmp', byte0)}, False)
    assert m is not None and m[0] == '__zf_from_snapshot'
    # An arithmetic setter joins too (it writes _fz from its result)...
    m = _incoming_flag_state([1, 2], {1: ('cmp', a), 2: ('sub', a)}, False)
    assert m is not None and m[0] == '__zf_from_snapshot'
    # ...but a setter that does not write _fz keeps the join unknown.
    assert _incoming_flag_state([1, 2], {1: ('cmp', a), 2: ('bt', a)}, False) is None
