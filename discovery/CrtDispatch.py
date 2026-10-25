# the entries of the MSVC CRT's x87 math dispatch tables (exp, pow, atan2 ...), reached only through
# `jmp [ebx]` with ebx = table + 0x10 + an offset from the argument's FP class.
#
# Each math function is a stub `mov edx, <table>; jmp <wrapper>`; the wrapper calls a dispatcher,
# _trandisp1 (one argument: 4 entries, at offsets 0..12) or _trandisp2 (two: 16 entries, 0..60),
# which ends `add ebx, eax; add ebx, 0x10; jmp dword ptr [ebx]`. The tables sit in .rdata at odd
# addresses, so the aligned scan in DataPointers never sees them, and the code they point at is
# reached from nowhere else: Ghidra leaves it undecoded. Found here from the dispatchers back to the
# tables; every entry becomes a function.
#
# Args: <out.txt>
# Jython headless post-script.
# @category XIVLegacyRecomp
from ghidra.app.cmd.disassemble import DisassembleCommand
from ghidra.program.model.lang import OperandType
import jarray

out_path = getScriptArgs()[0]
prog = currentProgram
mem = prog.getMemory()
listing = prog.getListing()
fm = prog.getFunctionManager()
refs = prog.getReferenceManager()
sp = prog.getAddressFactory().getDefaultAddressSpace()
text = mem.getBlock('.text')
tlo, thi = text.getStart().getOffset(), text.getEnd().getOffset()

TAIL = jarray.array([b - 256 if b > 127 else b for b in (0x03, 0xD8, 0x83, 0xC3, 0x10, 0xFF, 0x23)], 'b')  # add ebx,eax; add ebx,0x10; jmp [ebx]

lines = []
dispatchers = {}  # entry -> number of table entries
a = text.getStart()
while True:
    hit = mem.findBytes(a, text.getEnd(), TAIL, None, True, monitor)
    if hit is None:
        break
    a = hit.add(1)
    f = fm.getFunctionContaining(hit)
    if f is None:
        lines.append('  dispatcher tail at %s outside every function: skipped' % hit)
        continue
    fxam = 0
    it = listing.getInstructions(f.getBody(), True)
    while it.hasNext():
        if it.next().getMnemonicString() == 'FXAM':
            fxam += 1
    dispatchers[f.getEntryPoint()] = 16 if fxam >= 2 else 4
    lines.append('  dispatcher %s (tail %s): %d entries' % (f.getEntryPoint(), hit, dispatchers[f.getEntryPoint()]))


def callers(entry, mnemonics):
    """(instruction, containing function) for each CALL/JMP to entry"""
    out = []
    for r in refs.getReferencesTo(entry):
        ins = listing.getInstructionAt(r.getFromAddress())
        if ins is not None and ins.getMnemonicString() in mnemonics:
            out.append(ins)
    return out


tables = {}  # table address -> entries
for d, n in dispatchers.items():
    for c in callers(d, ('CALL',)):
        w = fm.getFunctionContaining(c.getAddress())
        if w is None:
            continue
        for j in callers(w.getEntryPoint(), ('JMP', 'CALL')):
            prev = listing.getInstructionBefore(j.getAddress())
            if prev is None or prev.getMnemonicString() != 'MOV' or prev.getNumOperands() != 2:
                continue
            reg = prev.getRegister(0)
            if reg is None or reg.getName() != 'EDX' or not (prev.getOperandType(1) & OperandType.SCALAR):
                continue
            tables[prev.getScalar(1).getUnsignedValue()] = n

made = existing = inner = 0
for t, n in sorted(tables.items()):
    targets = []
    for k in range(n):
        v = mem.getInt(sp.getAddress(t + 0x10 + 4 * k)) & 0xFFFFFFFF
        if tlo <= v <= thi and v not in targets:
            targets.append(v)
    created = []
    for v in targets:
        e = sp.getAddress(v)
        if fm.getFunctionAt(e) is not None:
            existing += 1
            continue
        if fm.getFunctionContaining(e) is not None:
            inner += 1
        if listing.getInstructionAt(e) is None:
            if not DisassembleCommand(e, None, True).applyTo(prog, monitor) or listing.getInstructionAt(e) is None:
                lines.append('  %08x: entry %s does not decode' % (t, e))
                continue
        if createFunction(e, None) is not None:
            made += 1
            created.append(str(e))
    lines.append('  table %08x (%d entries): %s' % (t, n, ' '.join(created) if created else 'nothing new'))

head = 'crt_dispatch dispatchers %d tables %d created %d existing %d inner %d' % (
    len(dispatchers), len(tables), made, existing, inner)
f = open(out_path, 'w')
f.write('\n'.join([head] + lines) + '\n')
f.close()
print(head)
