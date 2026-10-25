# The manual verdicts, per module and per retail build. Imported by Discover2.py, Switches.py
# and BogusCheck.py (Jython), and read by tools/discover.py (CPython): keep it plain data, no
# Ghidra imports at module level. A build is recognised by the PE TimeDateStamp of the imported
# image; the module by the image's name (ffxivgame.exe -> 'ffxivgame').
#
# The pipeline reports anything these lists miss (Conflicts, BogusCheck, SwitchAudit).
BUILDS = {
    # ffxivgame.exe, 2012.09.19.0001 (1.23b), linked 2012-09-11
    0x504f671f: '2012.09.19.0001',
}

# NOT_CODE: false function starts (data records decoded as code, or aligned starts inside a real
# instruction). REDECODE: the real decode points those false starts were hiding.
# SWITCHES: switches whose case count cannot be read from a guard in their own function.
VERDICTS = {
    ('ffxivgame', '2012.09.19.0001'): {
        'NOT_CODE': [
            # false function starts inside a real instruction (seeded by data that looks like a
            # pointer, e.g. the ASCII "rbp"/"sep" = 0x00706272/0x00706573)
            0x004f424d, 0x00645f73, 0x00657874, 0x00706273, 0x00706577, 0x00706862, 0x007a3cb4, 0x007a4743,
            # on the last byte of a call rel32 (e8 .. .. .. ff)
            0x0098e8e0, 0x0098e7a0,
            # data decoded as code: OpenSSL aes-586 Td table (0xe4b740..0xe4bf40), other OpenSSL
            # tables, a 2-entry jump table after jmp [eax*4+0xba7ac0], and the 1982-entry jump
            # table of the switch at 0x57ed4e (0x57eda4..0x580c9c)
            0x00e4bf20, 0x00e53570, 0x00e48e80, 0x00ba7ac0, 0x00580056,
            # an aligned start on a 64 KB boundary inside `mov [eax],ecx` at 0x53ffff (exposed once
            # the calls before it got their fall-through back)
            0x00540000,
            # inside `jmp ___CxxFrameHandler3` (e9 ..) of an EH handler stub at 0xeee427
            0x00eee42e,
        ],
        'REDECODE': [0x004f424a, 0x00645f72, 0x00657872, 0x00706272, 0x00706573, 0x00706860, 0x007a3cb1,
                     0x007a4741, 0x0098e8e1, 0x0098e7a1,
                     0x0053ffff, 0x00eee42c,
                     0x0048301c],  # push 0x206 ... after je 0x483042: real code Ghidra left undecoded
        'CLEAR': [0x00d09f8e],  # mov edi,edi padding after a no-return call, before the jump table at 0xd09f90
        'NO_FALLTHROUGH': [0x00d09f89],  # call _CxxThrowException (0x9d1b9f) just before that padding
        'KEEP': [0x00e43c10],   # OpenSSL OPENSSL_instrument_halt (pushfd/rdtsc/hlt): real, rarely called
        'SWITCHES': {
            0x007d9401: 6,  # 6 entries then cc padding
            0x009e5e11: 8,  # cmp eax,7 / ja
            0x00bbc536: 6,  # ecx in {0,3,5,..}; 6 entries then cc padding
            0x00bbc5dd: 6,  # same shape
            0x00bbd73d: 6,  # same shape
            0x00dccea4: 7,  # edx = [ecx+0x10], no guard; the next table (0xdcd1d8) starts 7 entries on
            0x00dccf86: 7,  # the same field again; 7 entries then code
        },
    },
}


def build_of(prog):
    mem = prog.getMemory()
    base = prog.getImageBase()
    lfanew = mem.getInt(base.add(0x3c))
    ts = mem.getInt(base.add(lfanew + 8)) & 0xFFFFFFFF
    if ts not in BUILDS:
        raise Exception('unknown build: PE timestamp 0x%08x; add it to discovery/verdicts.py' % ts)
    return BUILDS[ts]


def verdicts(prog):
    module = prog.getName().split('.')[0].lower()
    key = (module, build_of(prog))
    if key not in VERDICTS:
        raise Exception('no manual verdicts for %s %s; add them to discovery/verdicts.py' % key)
    return VERDICTS[key]
