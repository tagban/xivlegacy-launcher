# Ghidra's "non-returning" verdicts, checked. Its analyzer cascades: one wrong call (a function it
# believes cannot return) truncates the caller after that call, which can make the caller look
# non-returning too. In ffxivgame.exe this cut the body of CRT functions after calls to free().
#
# A function is kept non-returning only if no RET is reachable from its entry when every call is
# assumed to return (calls into kept functions excepted). The others are cleared, and every call
# to a cleared function gets its fall-through disassembled and its caller's body fixed up.
#
# Args: <out.txt> [FIX]   (without FIX: report only)
# Jython headless post-script.
# @category XIVLegacyRecomp
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.app.cmd.function import CreateFunctionCmd
from ghidra.program.model.symbol import RefType

import verdicts as manual
MANUAL_NO_FALLTHROUGH = set(manual.verdicts(currentProgram).get('NO_FALLTHROUGH', []))

args = getScriptArgs()
out_path = args[0]
fix = len(args) > 1 and args[1] == 'FIX'
prog = currentProgram
listing = prog.getListing()
fm = prog.getFunctionManager()
rm = prog.getReferenceManager()
text = prog.getMemory().getBlock('.text')
sp = prog.getAddressFactory().getDefaultAddressSpace()


def returns(f, kept):
    """Whether a RET is reachable from f's entry, following jumps and call fall-throughs (except
    after calls into `kept` non-returning functions), decoding bytes Ghidra left undecoded."""
    seen = set()
    todo = [f.getEntryPoint()]
    steps = 0
    while todo and steps < 20000:
        a = todo.pop()
        while a is not None and text.contains(a) and a.getOffset() not in seen:
            steps += 1
            seen.add(a.getOffset())
            ins = listing.getInstructionAt(a)
            if ins is None:
                break  # undecoded: cannot tell from here
            m = ins.getMnemonicString()
            if m == 'RET':
                return True
            ft = ins.getFlowType()
            for t in ins.getFlows():
                if ft.isJump() and text.contains(t):
                    todo.append(t)
            if ft.isCall():
                tgt = [x.getOffset() for x in ins.getFlows()]
                if any(x in kept for x in tgt):
                    break
                a = ins.getMaxAddress().add(1)  # assume the call returns
                continue
            if ft.isTerminal() or (ft.isJump() and ft.isUnConditional()):
                break
            a = ins.getMaxAddress().add(1)
    return False


# CRT functions that really end the process (named by Function ID). Calls to ExitProcess and the
# like are imports, which the reachability walk treats as returning, so these are seeded.
EXITS = set(['__invoke_watson', '_exit', '__exit', '_abort', '___report_gsfailure', '__amsg_exit',
             '__CxxThrowException@8', '_CxxThrowException'])

noret = [f for f in fm.getFunctions(True) if f.hasNoReturn() and not f.isExternal() and not f.isThunk()]
# Fixpoint: start from the known exits; a function that cannot reach RET even then is kept.
kept = set(f.getEntryPoint().getOffset() for f in noret if f.getName() in EXITS)
changed = True
while changed:
    changed = False
    for f in noret:
        e = f.getEntryPoint().getOffset()
        if e in kept:
            continue
        if not returns(f, kept):
            kept.add(e)
            changed = True

cleared = [f for f in noret if f.getEntryPoint().getOffset() not in kept]
lines = ['noreturn %d kept %d cleared %d' % (len(noret), len(kept), len(cleared))]
for f in cleared[:60]:
    n = len(list(rm.getReferencesTo(f.getEntryPoint())))
    lines.append('  clear %s %s callers=%d' % (f.getEntryPoint(), f.getName(), n))
for e in sorted(kept)[:60]:
    f = fm.getFunctionAt(sp.getAddress(e))
    lines.append('  keep  %s %s' % (f.getEntryPoint(), f.getName()))

if fix:
    callers = set()
    redecoded = 0
    for f in cleared:
        f.setNoReturn(False)
        for r in rm.getReferencesTo(f.getEntryPoint()):
            if not r.getReferenceType().isCall():
                continue
            ins = listing.getInstructionAt(r.getFromAddress())
            if ins is None or ins.getMnemonicString() != 'CALL':
                continue
            ins.setFlowOverride(ins.getFlowOverride())  # re-evaluate flows
            ft = ins.getMaxAddress().add(1)
            if listing.getInstructionAt(ft) is None and listing.getDefinedDataAt(ft) is None:
                DisassembleCommand(ft, None, True).applyTo(prog, monitor)
                redecoded += 1
            c = fm.getFunctionContaining(r.getFromAddress())
            if c is not None:
                callers.add(c.getEntryPoint())
    # Clearing the flag does not give the call sites back their fall-through (Ghidra removed it
    # when it decided the callee cannot return): every call into a function that returns, with no
    # fall-through, gets it restored.
    from ghidra.program.model.listing import FlowOverride
    restored = 0
    it = listing.getInstructions(text.getStart(), True)
    while it.hasNext():
        ins = it.next()
        if not text.contains(ins.getAddress()):
            break
        # CALL only: Ghidra gives tail-call JMPs the same CALL_RETURN override, and those stay.
        if ins.getMnemonicString() != 'CALL' or ins.getFallThrough() is not None:
            continue
        tgts = [fm.getFunctionAt(t) for t in ins.getFlows()]
        if not tgts or any(t is None or t.hasNoReturn() for t in tgts):
            continue
        if ins.getAddress().getOffset() in MANUAL_NO_FALLTHROUGH:
            continue
        # Ghidra marks calls into a non-returning function CALL_RETURN; the callee returns.
        if ins.getFlowOverride() == FlowOverride.CALL_RETURN:
            ins.setFlowOverride(FlowOverride.NONE)
        if ins.isFallThroughOverridden():
            ins.clearFallThroughOverride()
        ft = ins.getMaxAddress().add(1)
        if listing.getInstructionAt(ft) is None and listing.getDefinedDataAt(ft) is None:
            DisassembleCommand(ft, None, True).applyTo(prog, monitor)
            redecoded += 1
        c = fm.getFunctionContaining(ins.getAddress())
        if c is not None:
            callers.add(c.getEntryPoint())
        restored += 1
    for e in callers:
        c = fm.getFunctionAt(e)
        if c is not None:
            CreateFunctionCmd.fixupFunctionBody(prog, c, monitor)
    lines.append('call fall-throughs restored %d' % restored)
    lines.append('fixed: cleared %d, fall-throughs decoded %d, callers fixed up %d' % (len(cleared), redecoded, len(callers)))

f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print('\n'.join(lines[:3]))
