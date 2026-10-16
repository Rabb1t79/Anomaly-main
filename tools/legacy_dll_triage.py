#!/usr/bin/env python3
"""Read-only PE/static-string triage for the archived Anomaly plugin DLL.

The report intentionally redacts likely credentials. It never executes the target DLL.
"""
from __future__ import annotations

import hashlib
import json
import math
import re
import struct
import shutil
import sys
from pathlib import Path


def u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]


def u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]


def cstr(data: bytes, off: int, limit: int = 4096) -> str:
    if off < 0 or off >= len(data):
        return ""
    end = data.find(b"\0", off, min(len(data), off + limit))
    if end < 0:
        end = min(len(data), off + limit)
    return data[off:end].decode("ascii", errors="replace")


def entropy(buf: bytes) -> float:
    if not buf:
        return 0.0
    counts = [0] * 256
    for byte in buf:
        counts[byte] += 1
    total = len(buf)
    return round(-sum((n / total) * math.log2(n / total) for n in counts if n), 4)


def redact(text: str) -> str:
    text = re.sub(
        r"(?i)\b(key|token|secret|license|licence|password|passwd|credential|auth)\s*[:=]\s*([^\s,;\"']{4,})",
        lambda m: m.group(1) + "=[REDACTED]",
        text,
    )
    text = re.sub(r"(?i)\b(?:[0-9a-f]{32,})\b", "[REDACTED_HEX_CANDIDATE]", text)
    text = re.sub(r"\b[A-Za-z0-9+/]{48,}={0,2}\b", "[REDACTED_LONG_TOKEN_CANDIDATE]", text)
    return text[:1000]


def scan_strings(data: bytes):
    found = set()
    for match in re.finditer(rb"[\x20-\x7e]{5,}", data):
        found.add(match.group(0).decode("ascii", errors="replace"))
    # Printable UTF-16LE strings (common Windows resource/string representation).
    for match in re.finditer(rb"(?:[\x20-\x7e]\x00){5,}", data):
        found.add(match.group(0).decode("utf-16le", errors="replace"))
    return sorted(found, key=lambda x: (x.lower(), x))


def inspect_pe(data: bytes):
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("File does not have a valid DOS MZ header.")
    pe_off = u32(data, 0x3C)
    if pe_off + 24 > len(data) or data[pe_off:pe_off + 4] != b"PE\0\0":
        raise ValueError("PE signature is missing or out of bounds.")
    coff = pe_off + 4
    machine = u16(data, coff)
    section_count = u16(data, coff + 2)
    timestamp = u32(data, coff + 4)
    optional_size = u16(data, coff + 16)
    characteristics = u16(data, coff + 18)
    opt = coff + 20
    magic = u16(data, opt)
    if magic == 0x20B:
        arch = "PE32+"
        directory_off = opt + 112
        image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    elif magic == 0x10B:
        arch = "PE32"
        directory_off = opt + 96
        image_base = u32(data, opt + 28)
    else:
        arch = f"unknown optional header magic 0x{magic:04x}"
        directory_off = opt
        image_base = 0

    sections = []
    sec_off = opt + optional_size
    for idx in range(section_count):
        off = sec_off + idx * 40
        if off + 40 > len(data):
            break
        name = data[off:off + 8].split(b"\0", 1)[0].decode("ascii", errors="replace")
        virtual_size, virtual_address, raw_size, raw_off = struct.unpack_from("<IIII", data, off + 8)
        flags = u32(data, off + 36)
        raw = data[raw_off:raw_off + raw_size] if raw_off < len(data) else b""
        sections.append({
            "name": name,
            "virtual_address": f"0x{virtual_address:x}",
            "virtual_size": virtual_size,
            "raw_size": raw_size,
            "raw_offset": f"0x{raw_off:x}",
            "entropy": entropy(raw),
            "characteristics": f"0x{flags:08x}",
        })

    def rva_to_off(rva: int):
        for s in sections:
            va = int(s["virtual_address"], 16)
            span = max(s["virtual_size"], s["raw_size"])
            if va <= rva < va + span:
                off = int(s["raw_offset"], 16) + (rva - va)
                return off if off < len(data) else None
        if rva < len(data):
            return rva
        return None

    imports = []
    exports = []
    if directory_off + 16 * 8 <= len(data):
        export_rva, export_size = struct.unpack_from("<II", data, directory_off)
        import_rva, import_size = struct.unpack_from("<II", data, directory_off + 8)
        if import_rva:
            imp_off = rva_to_off(import_rva)
            if imp_off is not None:
                for i in range(4096):
                    d = imp_off + i * 20
                    if d + 20 > len(data):
                        break
                    original_thunk, _, _, name_rva, first_thunk = struct.unpack_from("<IIIII", data, d)
                    if not (original_thunk or name_rva or first_thunk):
                        break
                    name_off = rva_to_off(name_rva)
                    dll_name = cstr(data, name_off) if name_off is not None else "(invalid DLL name RVA)"
                    thunk_rva = original_thunk or first_thunk
                    thunk_off = rva_to_off(thunk_rva)
                    symbols = []
                    if thunk_off is not None:
                        width = 8 if magic == 0x20B else 4
                        ordinal_mask = 1 << (width * 8 - 1)
                        for j in range(8192):
                            toff = thunk_off + j * width
                            if toff + width > len(data):
                                break
                            value = int.from_bytes(data[toff:toff + width], "little")
                            if value == 0:
                                break
                            if value & ordinal_mask:
                                symbols.append(f"ordinal:{value & 0xffff}")
                            else:
                                name_off = rva_to_off(value)
                                if name_off is None or name_off + 2 > len(data):
                                    symbols.append("?(invalid import name RVA)")
                                else:
                                    symbols.append(cstr(data, name_off + 2, 1024))
                    imports.append({"dll": dll_name, "symbols": symbols[:1000]})
        if export_rva:
            exp_off = rva_to_off(export_rva)
            if exp_off is not None and exp_off + 40 <= len(data):
                fields = struct.unpack_from("<IIHHIIIIIII", data, exp_off)
                name_count = fields[7]
                names_rva = fields[9]
                names_off = rva_to_off(names_rva)
                if names_off is not None:
                    for i in range(min(name_count, 10000)):
                        entry = names_off + i * 4
                        if entry + 4 > len(data):
                            break
                        name_rva = u32(data, entry)
                        name_off = rva_to_off(name_rva)
                        if name_off is not None:
                            exports.append(cstr(data, name_off, 1024))

    return {
        "machine": f"0x{machine:04x}",
        "machine_name": {0x8664: "AMD64/x86-64", 0x14c: "i386", 0xaa64: "ARM64"}.get(machine, "unknown"),
        "optional_header": arch,
        "image_base": f"0x{image_base:x}",
        "pe_timestamp_unix": timestamp,
        "characteristics": f"0x{characteristics:04x}",
        "section_count": section_count,
        "sections": sections,
        "imports": imports,
        "exports": sorted(set(exports)),
    }


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: legacy_dll_triage.py INPUT_DLL OUTPUT_JSON")
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    data = src.read_bytes()
    pe = inspect_pe(data)
    strings = scan_strings(data)
    terms = re.compile(
        r"anomaly|nte|damage|gameplay|ability|input|press|release|key|token|license|secret|auth|sha|md5|https?://|nanally|replay|hook|abi|plugin|ue5|runtime|function|effect|attack|chain|反射",
        re.IGNORECASE,
    )
    selected = [redact(s) for s in strings if terms.search(s)]
    # Avoid publishing an embedded credential. Generic imports and non-secret UI strings remain.
    report = {
        "source_file": src.name,
        "size_bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "git_blob_sha1": hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest(),
        "pe": pe,
        "interesting_strings_count": len(selected),
        "interesting_strings": selected[:1500],
        "all_ascii_or_utf16_string_count": len(strings),
        "notes": [
            "Static inspection only; the DLL was not loaded or executed.",
            "Long hex/base64-like and obvious credential assignments are redacted from this public report.",
            "Strings and imports are clues, not proof of runtime control flow or recovered C++ source.",
        ],
    }
    dst.parent.mkdir(parents=True, exist_ok=True)
    # This binary is already present in the public source repository. Include the exact
    # bytes in a short-lived analysis artifact for offline disassembly; never execute it.
    shutil.copy2(src, dst.parent / src.name)
    dst.write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\\n", encoding="utf-8")
    print(f"input={src.name} bytes={len(data)} sha256={report['sha256']} git_blob_sha1={report['git_blob_sha1']}")
    print(f"architecture={pe['machine_name']} sections={pe['section_count']} imports={len(pe['imports'])} exports={len(pe['exports'])}")
    print(f"interesting_strings={len(selected)} (credential-like values redacted); report={dst}")


if __name__ == "__main__":
    main()
