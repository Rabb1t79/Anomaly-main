#!/usr/bin/env python3
"""Offline triage for the repository's legacy plugin DLL. No external packages."""
import hashlib, json, math, re, struct
from collections import Counter
from pathlib import Path

TARGET = Path("plugins/TimeAccel/plugin_old.dll")
EXPECTED_SIZE = 21120000
EXPECTED_GIT_BLOB = "5b8d3ca371315c68837246efe7dd6c8a2e9a5ee0"

def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]
def cstr(b, o, limit=4096):
    if o < 0 or o >= len(b): return ""
    e = b.find(b"\0", o, min(len(b), o + limit))
    if e < 0: e = min(len(b), o + limit)
    return b[o:e].decode("ascii", "replace")

def entropy(buf):
    if not buf: return 0.0
    counts = Counter(buf)
    n = len(buf)
    return round(-sum((v/n) * math.log2(v/n) for v in counts.values()), 5)

def strings_ascii(b, minlen=5):
    return [m.group().decode("ascii", "replace") for m in re.finditer(rb"[\x20-\x7e]{%d,}" % minlen, b)]

def strings_utf16(b, minlen=5):
    out = []
    for m in re.finditer(rb"(?:[\x20-\x7e]\x00){%d,}" % minlen, b):
        try: out.append(m.group().decode("utf-16le", "replace"))
        except Exception: pass
    return out

def main():
    if not TARGET.exists():
        raise SystemExit(f"ERROR: expected repository file missing: {TARGET}")
    b = TARGET.read_bytes()
    sha256 = hashlib.sha256(b).hexdigest()
    git_blob = hashlib.sha1(f"blob {len(b)}\0".encode() + b).hexdigest()
    report = {
        "scope": "static triage only; no execution, patching, or external package downloads",
        "file": str(TARGET),
        "size_bytes": len(b),
        "sha256": sha256,
        "git_blob_sha1": git_blob,
        "expected_size_bytes": EXPECTED_SIZE,
        "expected_git_blob_sha1": EXPECTED_GIT_BLOB,
        "identity_match": len(b) == EXPECTED_SIZE and git_blob == EXPECTED_GIT_BLOB,
    }
    if len(b) < 0x100 or b[:2] != b"MZ":
        report["error"] = "Not a recognizable DOS/PE file"
        Path("legacy-plugin-triage.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        raise SystemExit("Input is not a recognizable PE file; report written")

    pe = u32(b, 0x3c)
    if pe + 24 > len(b) or b[pe:pe+4] != b"PE\0\0":
        report["error"] = "Invalid PE signature/offset"
        Path("legacy-plugin-triage.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        raise SystemExit("Invalid PE signature; report written")
    coff = pe + 4
    machine, nsec, timestamp, symptr, nsyms, optsize, characteristics = struct.unpack_from("<HHIIIHH", b, coff)
    opt = coff + 20
    magic = u16(b, opt)
    is64 = magic == 0x20b
    dd_off = opt + (112 if is64 else 96)
    num_dirs_off = opt + (108 if is64 else 92)
    num_dirs = u32(b, num_dirs_off) if opt + (num_dirs_off-opt)+4 <= len(b) else 0
    image_base = u64(b, opt+24) if is64 else u32(b, opt+28)
    entry_rva = u32(b, opt+16)
    size_image = u32(b, opt+56)
    size_headers = u32(b, opt+60)
    section_table = opt + optsize
    sections = []
    for i in range(nsec):
        o = section_table + i*40
        if o+40 > len(b): break
        name = b[o:o+8].split(b"\0",1)[0].decode("ascii","replace")
        vsize, va, rawsize, rawptr = struct.unpack_from("<IIII", b, o+8)
        chars = u32(b, o+36)
        raw = b[rawptr:min(len(b), rawptr+rawsize)] if rawptr < len(b) else b""
        sections.append({"name":name,"virtual_size":vsize,"virtual_address":hex(va),
                         "raw_size":rawsize,"raw_offset":hex(rawptr),"entropy":entropy(raw),
                         "characteristics":hex(chars),"raw_end":rawptr+rawsize})
    def rva_off(rva):
        if rva < size_headers: return rva if rva < len(b) else None
        for s in sections:
            va = int(s["virtual_address"],16)
            span = max(s["virtual_size"], s["raw_size"])
            if va <= rva < va + span:
                off = int(s["raw_offset"],16) + (rva-va)
                return off if off < len(b) else None
        return None
    def directory(idx):
        if idx >= num_dirs: return (0,0)
        o = dd_off + idx*8
        if o+8 > len(b): return (0,0)
        return struct.unpack_from("<II",b,o)
    imports=[]
    imp_rva, imp_size = directory(1)
    io = rva_off(imp_rva) if imp_rva else None
    if io is not None:
        for _ in range(4096):
            if io+20 > len(b): break
            oft, tds, fwd, name_rva, ft = struct.unpack_from("<IIIII",b,io)
            if not any((oft,tds,fwd,name_rva,ft)): break
            dll = cstr(b,rva_off(name_rva) or -1)
            thunk_rva = oft or ft
            to = rva_off(thunk_rva) if thunk_rva else None
            names=[]
            if to is not None:
                width = 8 if is64 else 4
                for j in range(4096):
                    q=to+j*width
                    if q+width>len(b): break
                    val=u64(b,q) if is64 else u32(b,q)
                    if val==0: break
                    ordinal_bit = (1 << (width*8-1))
                    if val & ordinal_bit:
                        names.append("ordinal:"+str(val & 0xffff))
                    else:
                        no=rva_off(val & (0x7fffffffffffffff if is64 else 0x7fffffff))
                        if no is not None and no+2<len(b): names.append(cstr(b,no+2,1024))
            imports.append({"dll":dll,"symbols":names[:500]})
            io += 20
    exports=[]
    exp_rva, exp_size = directory(0)
    eo = rva_off(exp_rva) if exp_rva else None
    if eo is not None and eo+40<=len(b):
        base, nfunc, nname = struct.unpack_from("<IIIIII",b,eo+16)[:6:1] if False else (None,None,None,None,None,None)
        # IMAGE_EXPORT_DIRECTORY fields are read by their actual offsets.
        base=u32(b,eo+16); nfunc=u32(b,eo+20); nname=u32(b,eo+24)
        names_rva=u32(b,eo+32); ords_rva=u32(b,eo+36)
        no=rva_off(names_rva); oo=rva_off(ords_rva)
        if no is not None and oo is not None:
            for i in range(min(nname,10000)):
                if no+i*4+4>len(b) or oo+i*2+2>len(b): break
                nrva=u32(b,no+i*4); ordinal=u16(b,oo+i*2)+base
                n=cstr(b,rva_off(nrva) or -1)
                exports.append({"name":n,"ordinal":ordinal})
    ascii_s = strings_ascii(b)
    wide_s = strings_utf16(b)
    all_s = sorted(set(ascii_s + wide_s))
    keywords = re.compile(r"(crypt|decrypt|encrypt|xor|compress|decompress|inflate|unpack|pack|payload|loader|virtualprotect|virtualalloc|loadlibrary|getprocaddress|ntdll|bcrypt|crypt32|zlib|lz4|upx|vmprotect|themida|gameplayeffect|damage|target|tick|timer|anomaly|ue5|process.?event)",re.I)
    interesting = [s for s in all_s if keywords.search(s)]
    max_raw_end = max([size_headers] + [s["raw_end"] for s in sections])
    overlay = max(0, len(b)-max_raw_end)
    report.update({
        "pe":{"offset":hex(pe),"machine":hex(machine),"sections_count":nsec,
              "timestamp_raw":timestamp,"optional_header_magic":hex(magic),
              "architecture":"PE32+" if is64 else ("PE32" if magic==0x10b else "unknown"),
              "image_base":hex(image_base),"entrypoint_rva":hex(entry_rva),
              "size_of_image":size_image,"size_of_headers":size_headers,
              "characteristics":hex(characteristics),"data_directories_count":num_dirs},
        "sections":sections,
        "imports":imports,
        "exports":exports,
        "string_counts":{"ascii_runs":len(ascii_s),"utf16le_runs":len(wide_s),"unique_combined":len(all_s)},
        "interesting_strings":interesting[:1500],
        "overlay_bytes_after_last_section":overlay,
        "limitations":["Static triage only. High entropy, suspicious imports, or strings do not prove encryption or successful decryption.",
                       "No DLL code was executed. This report does not reconstruct control flow or establish the actual damage-chain mechanism."]
    })
    Path("legacy-plugin-triage.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    with open("legacy-plugin-triage.txt","w",encoding="utf-8") as f:
        f.write("LEGACY PLUGIN DLL — STATIC TRIAGE\n")
        f.write(f"File: {TARGET}\nSize: {len(b)}\nSHA256: {sha256}\nGit blob SHA1: {git_blob}\nIdentity match: {report['identity_match']}\n\n")
        f.write(json.dumps(report["pe"],indent=2)+"\n\nSECTIONS\n")
        f.write(json.dumps(sections,indent=2)+"\n\nIMPORTS\n")
        f.write(json.dumps(imports,indent=2)+"\n\nEXPORTS\n")
        f.write(json.dumps(exports,indent=2)+"\n\nINTERESTING STRINGS\n")
        f.write("\n".join(interesting[:1500])+"\n")
        f.write(f"\nOverlay bytes after last section: {overlay}\n")
        f.write("\nLIMITATIONS\n- Static triage only; no decryption claim.\n- No DLL execution, control-flow reconstruction, or claim about damage-chain mechanism.\n")
    print(f"identity_match={report['identity_match']} size={len(b)} sha256={sha256} git_blob={git_blob}")
    print(f"sections={len(sections)} imports={len(imports)} exports={len(exports)} interesting_strings={len(interesting)}")
    if not report["identity_match"]:
        raise SystemExit("WARNING: file identity does not match expected repository blob; reports still emitted")

if __name__ == "__main__":
    main()
