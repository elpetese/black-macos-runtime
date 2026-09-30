"""A join whose predecessors set the flags differently gets its condition per edge.

test al,al on one path and cmp eax,edx on the other cannot be merged, and the je
at the join used to read the never-assigned _flags: never taken. Each edge now
stores the je's condition into _ej (translator._plan_edge_joins).
"""
import os, subprocess, tempfile
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator

BASE = 0x10000
# 0 test ecx,ecx; 2 jz 8; 4 test al,al; 6 jmp 10; 8 cmp eax,edx; 10 jl 13; 12 nop; 13 ret
# (a jl: the je/jne form of this join is answered by the ZF-only merge, _fz)
IMAGE = bytes.fromhex('85c9740484c0eb0239d07c0190c3')
IMAGE_JE = bytes.fromhex('85c9740484c0eb0239d0740190c3')

def translate(image=IMAGE):
    config._install([config.Section('.text', BASE, len(image), 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='flag-join-edges-test')
    db = {BASE: {'start': hex(BASE), 'end': BASE + len(image), '_addr': BASE, 'size': len(image)}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])

def test_edges_store_their_own_condition():
    code = translate()
    assert 'if (_ej) goto loc_0001000D;' in code, code
    assert code.count('_ej = (') == 2, code
    assert 'if (_flags /* jl' not in code, code
    assert 'int _ej = 0;' in code, code

def test_zero_flag_join_reads_fz():
    code = translate(IMAGE_JE)
    assert 'if (_fz) goto loc_0001000D;' in code, code
    assert 'if (_flags /* je' not in code, code

def test_jump_edge_assigns_before_its_goto():
    lines = [l.strip() for l in translate().splitlines()]
    k = next(i for i, l in enumerate(lines) if l.startswith('goto loc_0001000A;'))
    assert lines[k - 1].startswith('_ej = ('), lines[k - 3:k + 1]
