# list functions overlapping [lo, hi): entry, body ranges, instruction count, first instruction,
# references to the entry. For reading discovery verdicts; read-only.
# Args: <out.txt> <lo hex> <hi hex>
# Jython headless post-script.
# @category XIVLegacyRecomp
args = getScriptArgs()
out_path, lo, hi = args[0], int(args[1], 16), int(args[2], 16)
prog = currentProgram
sp = prog.getAddressFactory().getDefaultAddressSpace()
fm = prog.getFunctionManager()
listing = prog.getListing()
rm = prog.getReferenceManager()

lines = []
for f in fm.getFunctions(sp.getAddress(lo), True):
    e = f.getEntryPoint().getOffset()
    if e >= hi:
        break
    body = f.getBody()
    n = 0
    it = listing.getInstructions(body, True)
    while it.hasNext():
        it.next()
        n += 1
    first = listing.getInstructionAt(f.getEntryPoint())
    refs = [r.getFromAddress().getOffset() for r in rm.getReferencesTo(f.getEntryPoint())]
    ranges = ' '.join('%08x-%08x' % (r.getMinAddress().getOffset(), r.getMaxAddress().getOffset() + 1) for r in body)
    lines.append('%08x insns=%d refs=%d %s | %s | from %s' % (
        e, n, len(refs), ranges, first, ' '.join('%08x' % x for x in refs[:4])))

f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print('listed %d functions' % len(lines))
