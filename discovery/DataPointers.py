# functions reached only through pointers in data (the CRT's initializer/terminator tables, callback
# tables, vtables Ghidra did not type). An image with relocations would list them; ffxivgame.exe
# has none, so .rdata and .data are scanned for aligned dwords that point into .text.
#
# Only dwords in a table count (a neighbour points into .text too). A target becomes a function
# when it is not already one and looks like a start: an existing
# instruction outside every function, or undecoded bytes right after a RET / padding or on a 16-byte
# boundary. Targets inside a function body are counted, not touched (the recompiler adds those as
# entries itself). BogusCheck, later in the round, flags anything that decodes as garbage.
#
# Args: <out.txt>
# Jython headless post-script.
# @category XIVLegacyRecomp
from ghidra.app.cmd.disassemble import DisassembleCommand

out_path = getScriptArgs()[0]
prog = currentProgram
mem = prog.getMemory()
listing = prog.getListing()
fm = prog.getFunctionManager()
sp = prog.getAddressFactory().getDefaultAddressSpace()
text = mem.getBlock('.text')
tlo, thi = text.getStart().getOffset(), text.getEnd().getOffset()


def byte(a):
    return mem.getByte(sp.getAddress(a)) & 0xFF


def looks_like_start(v):
    if v % 16 == 0:
        return True
    p = byte(v - 1)
    if p in (0xC3, 0xCC, 0x90):
        return True
    return byte(v - 3) == 0xC2  # ret imm16


seen = set()
made = inner = skipped = 0
examples = []
for name in ('.rdata', '.data'):
    blk = mem.getBlock(name)
    if blk is None:
        continue
    a = blk.getStart().getOffset()
    end = blk.getStart().getOffset() + min(blk.getSize(), 0x7FFFFFFF)
    # only the initialized part of .data
    while a + 4 <= end:
        try:
            v = mem.getInt(sp.getAddress(a)) & 0xFFFFFFFF
        except:
            break
        a += 4
        if v < tlo or v > thi or v in seen:
            continue
        # Only pointers in a table (the dword before or after also points into .text): init
        # tables, vtables and callback arrays are; ASCII text that happens to look like an
        # address ("ons\0" = 0x00736e6f) is not.
        try:
            prev = mem.getInt(sp.getAddress(a - 8)) & 0xFFFFFFFF
            nxt = mem.getInt(sp.getAddress(a)) & 0xFFFFFFFF
        except:
            prev = nxt = 0
        if not (tlo <= prev <= thi or tlo <= nxt <= thi):
            continue
        seen.add(v)
        t = sp.getAddress(v)
        if fm.getFunctionAt(t) is not None:
            continue
        if fm.getFunctionContaining(t) is not None:
            inner += 1
            continue
        ins = listing.getInstructionAt(t)
        if ins is None:
            if listing.getInstructionContaining(t) is not None or not looks_like_start(v):
                skipped += 1
                continue
            if not DisassembleCommand(t, None, True).applyTo(prog, monitor) or listing.getInstructionAt(t) is None:
                skipped += 1
                continue
        if createFunction(t, None) is not None:
            made += 1
            if len(examples) < 30:
                examples.append('  %s (pointer at %08x)' % (t, a - 4))
        else:
            skipped += 1

# The same for code that takes a function's address as an immediate (push offset _output;
# mov [esi+0x54], offset callback): instructions in .text whose scalar operand is a .text address.
from ghidra.program.model.lang import OperandType
imm_made = imm_inner = 0
it = listing.getInstructions(text.getStart(), True)
while it.hasNext():
    ins = it.next()
    if not text.contains(ins.getAddress()):
        break
    m = ins.getMnemonicString()
    if m not in ('PUSH', 'MOV'):
        continue
    for i in range(ins.getNumOperands()):
        t = ins.getOperandType(i)
        if not (t & OperandType.SCALAR) or (t & OperandType.DYNAMIC):
            continue  # a displacement ([reg+disp]) is a table read, not a code pointer
        sc = ins.getScalar(i)
        if sc is None:
            continue
        v = sc.getUnsignedValue()
        if v < tlo or v > thi or v in seen:
            continue
        seen.add(v)
        a = sp.getAddress(v)
        if fm.getFunctionAt(a) is not None:
            continue
        if fm.getFunctionContaining(a) is not None:
            imm_inner += 1
            continue
        if listing.getInstructionAt(a) is None:
            if listing.getInstructionContaining(a) is not None or not looks_like_start(v):
                continue
            if not DisassembleCommand(a, None, True).applyTo(prog, monitor) or listing.getInstructionAt(a) is None:
                continue
        if createFunction(a, None) is not None:
            imm_made += 1
            if len(examples) < 45:
                examples.append('  %s (immediate at %s)' % (a, ins.getAddress()))

lines = ['data_pointer_functions created %d inner %d skipped %d; immediates created %d inner %d' % (
    made, inner, skipped, imm_made, imm_inner)] + examples
f = open(out_path, 'w')
f.write('\n'.join(lines) + '\n')
f.close()
print(lines[0])
