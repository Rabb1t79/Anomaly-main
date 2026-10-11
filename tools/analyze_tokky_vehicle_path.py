#!/usr/bin/env python3
"""Temporary static triage of the repository's Tokky-NTE.dll; no binary execution."""
from __future__ import annotations
import os, re, struct
from pathlib import Path
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

DLL = Path("Tokky-NTE.dll")
OUT = Path("analysis-output")
OUT.mkdir(exist_ok=True)
data = DLL.read_bytes()
pe = pefile.PE(str(DLL), fast_load=False)
base = pe.OPTIONAL_HEADER.ImageBase
lines = []
def out(s=""):
    lines.append(str(s))

out(f"FILE {DLL} size={len(data)} imagebase={base:#x} entry_rva={pe.OPTIONAL_HEADER.AddressOfEntryPoint:#x} machine={pe.FILE_HEADER.Machine:#x}")
out("SECTIONS")
for sec in pe.sections:
    out(f"  {sec.Name.decode(errors='replace').rstrip(chr(0))} VA={base+sec.VirtualAddress:#x} RVA={sec.VirtualAddress:#x} raw={sec.PointerToRawData:#x}+{sec.SizeOfRawData:#x} flags={sec.Characteristics:#x}")
out("IMPORTS")
for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
    funcs = []
    for imp in entry.imports:
        n = imp.name.decode(errors="replace") if imp.name else f"ordinal:{imp.ordinal}"
        funcs.append(n)
    out(f"  {entry.dll.decode(errors='replace')}: {', '.join(funcs)}")
try:
    for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
        out(f"EXPORT {base+exp.address:#x} {exp.name.decode(errors='replace') if exp.name else exp.ordinal}")
except Exception:
    pass

# Build a lightweight list of printable ASCII and UTF-16 strings with PE VAs.
strs = []
for m in re.finditer(rb"[\x20-\x7e]{5,}", data):
    off = m.start()
    try:
        rva = pe.get_rva_from_offset(off)
        s = m.group().decode("ascii", errors="replace")
        strs.append((base+rva, off, s, "ascii"))
    except Exception:
        pass
for m in re.finditer(rb"(?:[\x20-\x7e]\x00){5,}", data):
    off = m.start()
    try:
        rva = pe.get_rva_from_offset(off)
        s = m.group()[::2].decode("ascii", errors="replace")
        strs.append((base+rva, off, s, "utf16"))
    except Exception:
        pass
# Deduplicate same-address strings, favor the longer decoded sequence.
by_addr = {}
for x in strs:
    old = by_addr.get(x[0])
    if old is None or len(x[2]) > len(old[2]):
        by_addr[x[0]] = x
strs = sorted(by_addr.values())
terms = re.compile(r"(vehicle|spawn|summon|cheatmanager|cheatspawn|htgame|htvehicle|vehicleid|setmaxenginetorque|invehiclet emple|drivable|vehiclecomponent|vehicledata|playercontroller|possess|setowner|spawnactor|spawnobject|my pc|my_pc)", re.I)
relevant = [s for s in strs if terms.search(s[2].replace("InVehicleTemple","InVehicleTemple"))]
out("RELEVANT STRINGS")
for va, off, s, enc in relevant:
    out(f"  {va:#x} off={off:#x} {enc} {s[:240]}")
# Map executable bytes and identify RIP-relative/direct refs to these strings.
md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
md.skipdata = True
decoded = []
for sec in pe.sections:
    if not (sec.Characteristics & 0x20000000):  # IMAGE_SCN_MEM_EXECUTE
        continue
    raw = sec.get_data()
    addr = base + sec.VirtualAddress
    insns = list(md.disasm(raw, addr))
    decoded.extend(insns)
    out(f"DISASM_SECTION {sec.Name.decode(errors='replace').rstrip(chr(0))}: {len(insns)} instructions")
addr_ranges = []
for va, off, s, enc in relevant:
    addr_ranges.append((va, va + max(len(s) + 1, 8), va, s, off, enc))
xref = []
for idx, ins in enumerate(decoded):
    targets = []
    for op in ins.operands:
        if op.type == X86_OP_MEM and op.mem.base == 41: # X86_REG_RIP (stable value in capstone x86)
            targets.append((ins.address + ins.size + op.mem.disp, "rip-mem"))
        elif op.type == X86_OP_IMM:
            val = int(op.imm) & 0xffffffffffffffff
            if base <= val < base + pe.OPTIONAL_HEADER.SizeOfImage:
                targets.append((val, "imm"))
    for target, kind in targets:
        for lo, hi, strva, s, off, enc in addr_ranges:
            if lo <= target < hi:
                xref.append((idx, target, kind, strva, s, off, enc))
# Score references to likely implementation strings above UI labels.
def score(s):
    z=s.lower()
    val=0
    for pat, pts in [
      ("cheatspawnvehicle",100),("htvehiclesummonsubsystem",95),("clientspawnbicyclevehicle",90),
      ("clientspawnboat",90),("invehicletemple",85),("spawnactor",80),("spawnobject",75),
      ("vehicleid",70),("cheatmanager",68),("setmaxenginetorque",65),("vehicledata",60),
      ("htvehiclecomponent",55),("summonvehicle",52),("vehicle",20),("spawn",15)
    ]:
        if pat in z: val=max(val,pts)
    return val
xref.sort(key=lambda x:(-score(x[4]), x[0]))
out("XREFS TO TARGET STRINGS")
if not xref:
    out("  NONE FOUND in decoded executable sections")
seen = set()
for i, (idx, target, kind, strva, s, off, enc) in enumerate(xref):
    key=(idx,strva)
    if key in seen: continue
    seen.add(key)
    out(f"\n=== XREF {i+1} instr={decoded[idx].address:#x} target={target:#x} kind={kind} string_va={strva:#x} str={s[:180]} ===")
    lo=max(0,idx-12); hi=min(len(decoded),idx+13)
    # Prevent overly large duplicate report; limit each reference to 25 instructions.
    for ins in decoded[lo:hi]:
        raw=" ".join(f"{b:02x}" for b in ins.bytes)
        marker=">>" if ins.address == decoded[idx].address else "  "
        out(f"{marker} {ins.address:#x}: {raw:<32} {ins.mnemonic} {ins.op_str}")
    if len(seen) >= 140: break

text = "\n".join(lines) + "\n"
(OUT / "tokky_vehicle_xrefs.txt").write_text(text, encoding="utf-8")
# Stable concise summary on Actions log for retrieval without downloading artifacts.
print(text[:105000], end="")
