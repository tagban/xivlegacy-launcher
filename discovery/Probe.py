# print Ghidra's view of instructions at the given addresses: flow type, fall-through, overrides,
# containing function and the instruction after. For investigating discovery results; read-only.
# Args: <hex addr> [<hex addr> ...]
# Jython headless post-script.
# @category XIVLegacyRecomp
prog = currentProgram
listing = prog.getListing()
fm = prog.getFunctionManager()
sp = prog.getAddressFactory().getDefaultAddressSpace()
for h in getScriptArgs():
    a = sp.getAddress(int(h, 16))
    ins = listing.getInstructionAt(a)
    f = fm.getFunctionContaining(a)
    fn = fm.getFunctionContaining(a)
    print('NAME %s: %s' % (a, fn.getName() if fn else None))
    if ins is None:
        cu = listing.getCodeUnitContaining(a)
        print('PROBE %s: no instruction; code unit %s; function %s' % (a, cu, f))
        continue
    print('PROBE %s: %s | flow %s | fallthrough %s | override %s | ft_overridden %s | function %s' % (
        a, ins, ins.getFlowType(), ins.getFallThrough(), ins.getFlowOverride(), ins.isFallThroughOverridden(),
        f.getEntryPoint() if f else None))
    for t in ins.getFlows():
        tf = fm.getFunctionAt(t)
        print('PROBE   -> %s function %s noreturn %s' % (t, tf.getName() if tf else None, tf.hasNoReturn() if tf else None))
