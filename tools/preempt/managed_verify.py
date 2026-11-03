#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
# See LICENSE.

"""Verify and seal linked LLPM v2 images. No third-party Python dependencies.

This checks compiler contracts, not publisher authenticity. Only the pinned
driver may grant MACHINE_VERIFIED. Link sealing checks actual decoded bytes,
native/body separation, executable section membership, call closure, relocation
policy and the final loader build identity. Unknown instructions fail closed.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from functools import cache
import json
from pathlib import Path
import re
import struct
import subprocess

MAGIC = 0x4D504C4C
PROFILE = 0x4C4C414D17010001
MAX_RECORD_BYTES = 8 * 1024 * 1024
MAX_SECTIONS = 4096
MASK64 = (1 << 64) - 1


class VerificationError(ValueError):
    """An image cannot acquire or retain a managed certificate."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def unpack(data: bytes, offset: int, fmt: str) -> tuple:
    size = struct.calcsize("<" + fmt)
    require(0 <= offset <= len(data) and size <= len(data) - offset, "truncated image")
    return struct.unpack_from("<" + fmt, data, offset)


def slice_bytes(data: bytes, offset: int, size: int) -> bytes:
    require(0 <= offset <= len(data) and 0 <= size <= len(data) - offset, "section outside image")
    return data[offset:offset + size]


def cstring(data: bytes, offset: int) -> str:
    require(0 <= offset < len(data), "invalid section name")
    end = data.find(b"\0", offset)
    require(end >= 0, "unterminated section name")
    return data[offset:end].decode("ascii", errors="strict")


@dataclass(frozen=True)
class Section:
    name: str
    address: int
    offset: int
    size: int
    file_size: int
    executable: bool
    writable: bool
    relro: bool = False

    def contains(self, address: int, size: int = 1) -> bool:
        return self.address <= address and 0 <= size <= self.size and address - self.address <= self.size - size


@dataclass
class Image:
    data: bytes
    kind: str
    architecture: int
    abi: int
    base: int
    build_id: bytes
    sections: list[Section]
    relocatable: bool
    anchor_symbols: dict[str, int] = field(default_factory=dict)
    executable_regions: list[Section] = field(default_factory=list)
    anchor_sizes: dict[str, int] = field(default_factory=dict)

    def executable_section(self, address: int, size: int = 1) -> Section:
        matches = [s for s in self.sections if s.executable and s.contains(address, size)]
        require(len(matches) == 1, f"PC extent 0x{address:x}+{size} is not uniquely executable")
        return matches[0]

    def read_pc(self, address: int, size: int) -> bytes:
        section = self.executable_section(address, size)
        start = address - section.address
        require(start + size <= section.file_size, "executable zero-fill is unqualified")
        return slice_bytes(self.data, section.offset + start, size)

    def immutable_data(self, address: int, size: int) -> Section:
        matches = [s for s in self.sections if not s.executable and s.contains(address, size) and
                   address - s.address + size <= s.file_size and (not s.writable or s.relro)]
        require(len(matches) == 1, "guard bindings are not in uniquely immutable image data")
        return matches[0]

    def metadata(self) -> Section:
        names = {".llam.managed", "__llam_managed", ".llamm"}
        matches = [s for s in self.sections if s.name in names]
        require(len(matches) == 1, "missing or duplicate LLPM v2 section")
        section = matches[0]
        require(not section.executable and not section.writable and section.size <= MAX_RECORD_BYTES,
                "metadata section permissions/size are unqualified")
        require(section.size == section.file_size, "metadata cannot contain zero-fill")
        return section

    def executable_identity(self) -> int:
        value = 0xCBF29CE484222325
        sections = sorted(self.executable_regions or [s for s in self.sections if s.executable and s.size],
                          key=lambda s: s.address)
        require(bool(sections), "no executable sections")
        previous_end = 0
        for section in sections:
            require(not section.writable and section.address >= previous_end,
                    "overlapping or writable executable sections")
            require(section.size <= MAX_RECORD_BYTES * 32 and section.file_size == section.size,
                    "executable section size/zero-fill is unqualified")
            previous_end = section.address + section.size
            payload = struct.pack("<QQ", section.address - self.base, section.size)
            payload += slice_bytes(self.data, section.offset, section.size)
            for byte in payload:
                value = ((value ^ byte) * 0x100000001B3) & MASK64
        return value


def read_elf(data: bytes) -> Image:
    require(data[4:7] == b"\x02\x01\x01", "requires little-endian ELF64")
    kind, machine = unpack(data, 16, "HH")
    require(machine in (62, 183) and kind in (1, 2, 3), "unsupported ELF machine/type")
    phoff, shoff = unpack(data, 32, "QQ")
    phentsize, phnum, shentsize, shnum, shstr = unpack(data, 54, "HHHHH")
    require(shentsize == 64 and 0 < shnum <= MAX_SECTIONS and shstr < shnum, "ELF section table")
    require(phnum <= MAX_SECTIONS and (phnum == 0 or phentsize == 56), "ELF segment table")
    loads = []
    load_regions = []
    tls_regions = []
    relro = []
    executable_regions = []
    for i in range(phnum):
        ptype, flags, offset, vaddr, _, filesz, memsz, _ = unpack(data, phoff + i * phentsize, "IIQQQQQQ")
        if ptype == 1 and memsz:
            require(flags & 3 != 3 and filesz <= memsz, "invalid/writable executable ELF segment")
            slice_bytes(data, offset, filesz)
            loads.append(vaddr)
            load_regions.append((vaddr, offset, filesz, memsz, flags))
            if flags & 1:
                require(flags & 4 and filesz == memsz, "unreadable/zero-fill executable ELF segment")
                executable_regions.append(Section("PT_LOAD", vaddr, offset, memsz, filesz, True, False))
        elif ptype == 0x6474E552:
            relro.append((vaddr, vaddr + memsz))
        elif ptype == 7:
            require(filesz <= memsz and vaddr + memsz <= MASK64,
                    "invalid ELF TLS template")
            slice_bytes(data, offset, filesz)
            tls_regions.append((vaddr, filesz, memsz))
    raw = [unpack(data, shoff + i * shentsize, "IIQQQQIIQQ") for i in range(shnum)]
    names = slice_bytes(data, raw[shstr][4], raw[shstr][5])
    sections = []
    build_id = b""
    anchors = {}
    anchor_sizes = {}
    for name_offset, stype, flags, address, offset, size, link, _, _, entsize in raw:
        name = cstring(names, name_offset)
        if flags & 2 and size:
            require(flags & 0x800 == 0, "compressed allocated ELF section")
            if flags & 0x400 and stype == 8:
                # .tbss is zero-fill in each thread's TLS allocation. Its
                # linker address need not belong to any PT_LOAD mapping and
                # must never authorize image metadata or guard pointer slots.
                require(not flags & 4, "executable ELF TLS zero-fill")
                if kind != 1:
                    mappings = [(va, fs, ms) for va, fs, ms in tls_regions
                                if va + fs <= address and size <= ms and
                                address - va <= ms - size]
                    require(len(mappings) == 1, "ELF TLS zero-fill has no unique template")
                continue
            if kind != 1:
                mappings = [(va, po, fs, ms, pf) for va, po, fs, ms, pf in load_regions
                            if va <= address and size <= ms and address - va <= ms - size]
                require(len(mappings) == 1, "ELF allocated section has no unique loader mapping")
                va, po, fs, _, pf = mappings[0]
                require(pf & 4 and bool(flags & 4) == bool(pf & 1) and
                        (not flags & 1 or pf & 2), "ELF section/segment permissions disagree")
                if stype != 8:
                    require(offset == po + address - va and address - va + size <= fs,
                            "ELF section bytes differ from their loader mapping")
                if not flags & 1 and pf & 2:
                    require(any(low <= address and address + size <= high for low, high in relro),
                            "ELF immutable section has a writable loader mapping")
            sections.append(Section(name, address, offset, size, 0 if stype == 8 else size,
                                    bool(flags & 4), bool(flags & 1),
                                    any(low <= address and address + size <= high for low, high in relro)))
        if stype == 7 and name == ".note.gnu.build-id":
            note = slice_bytes(data, offset, size)
            namesz, descsz, ntype = unpack(note, 0, "III")
            desc_offset = 12 + ((namesz + 3) & ~3)
            require(ntype == 3 and slice_bytes(note, 12, namesz) == b"GNU\0" and descsz >= 16,
                    "unsupported ELF build identity")
            build_id = slice_bytes(note, desc_offset, descsz)[:16]
        if stype == 6:
            for item in range(0, size, 16):
                tag, value = unpack(data, offset + item, "qQ")
                require(tag != 22 and not (tag == 30 and value & 4), "ELF text relocations")
        if stype == 11 and kind != 1:
            require(entsize == 24 and size % 24 == 0 and size // 24 <= 262144 and link < len(raw),
                    "ELF dynamic symbol table")
            strings = slice_bytes(data, raw[link][4], raw[link][5])
            for item in range(0, size, 24):
                string, info, visibility, shndx, value, nbytes = unpack(data, offset + item, "IBBHQQ")
                if shndx == 0 or not strings.startswith(b"__llam_managed_anchor_", string):
                    continue
                name = cstring(strings, string)
                require(re.fullmatch(r"__llam_managed_anchor_[0-9a-f]{32}", name) is not None and
                        info & 15 == 1 and info >> 4 in (1, 2) and visibility & 3 in (0, 3) and
                        0 < nbytes <= MAX_RECORD_BYTES, "invalid ELF image anchor symbol")
                key = name.lstrip("_")
                require(key not in anchors, "duplicate ELF image anchor export")
                anchors[key] = value
                anchor_sizes[key] = nbytes
    image = Image(data, "elf", 1 if machine == 62 else 2, 1, min(loads, default=0),
                  build_id, sections, kind == 1, anchor_symbols=anchors,
                  executable_regions=executable_regions, anchor_sizes=anchor_sizes)
    if kind != 1:
        metadata = image.metadata()
        require(not any(s.contains(metadata.address) for s in executable_regions),
                "ELF metadata must be in a separate nonexecutable segment")
    return image


def read_macho(data: bytes) -> Image:
    magic, cpu, _, filetype, ncmds, sizeofcmds, _, _ = unpack(data, 0, "IIIIIIII")
    require(magic == 0xFEEDFACF and cpu in (0x1000007, 0x100000C), "unsupported Mach-O machine")
    require(filetype in (1, 2, 6) and ncmds <= MAX_SECTIONS, "unsupported Mach-O type/commands")
    slice_bytes(data, 32, sizeofcmds)
    cursor = 32
    sections = []
    bases = []
    build_id = b""
    for _ in range(ncmds):
        command, size = unpack(data, cursor, "II")
        require(size >= 8 and size % 4 == 0 and cursor + size <= 32 + sizeofcmds, "invalid Mach-O command")
        if command == 0x19:
            segment = unpack(data, cursor + 8, "16sQQQQIIII")
            segment_name, vmaddr, vmsize, fileoff, filesize, _, initprot, count, segment_flags = segment
            require(count <= MAX_SECTIONS and 72 + count * 80 <= size, "Mach-O sections")
            require(filesize <= vmsize, "Mach-O segment file extent exceeds memory")
            slice_bytes(data, fileoff, filesize)
            if initprot and vmsize:
                bases.append(vmaddr)
            require(filetype == 1 or initprot & 6 != 6, "writable executable Mach-O segment")
            for i in range(count):
                name, _, address, nbytes, offset, _, _, _, flags, _, _, _ = unpack(
                    data, cursor + 72 + i * 80, "16s16sQQIIIIIIII")
                name = name.rstrip(b"\0").decode("ascii")
                zero_fill = flags & 0xFF in (1, 12, 18)
                executable = bool(flags & 0x80000400)
                if filetype != 1 and nbytes:
                    require(vmaddr <= address and nbytes <= vmsize and address - vmaddr <= vmsize - nbytes,
                            "Mach-O section is outside its loader segment")
                    require(initprot & 1 and (not executable or initprot & 4),
                            "Mach-O section/segment permissions disagree")
                    if not zero_fill:
                        require(offset == fileoff + address - vmaddr and address - vmaddr + nbytes <= filesize,
                                "Mach-O section bytes differ from their loader mapping")
                sections.append(Section(name, address, offset, nbytes, 0 if zero_fill else nbytes,
                                        executable, filetype != 1 and bool(initprot & 2),
                                        segment_name.rstrip(b"\0") == b"__DATA_CONST" and bool(segment_flags & 0x10)))
        elif command == 0x1B:
            require(size == 24 and not build_id, "invalid/duplicate Mach-O UUID")
            build_id = slice_bytes(data, cursor + 8, 16)
        cursor += size
    require(cursor == 32 + sizeofcmds, "Mach-O command size mismatch")
    return Image(data, "macho", 1 if cpu == 0x1000007 else 2, 2, min(bases, default=0),
                 build_id, sections, filetype == 1)


def read_pe(data: bytes) -> Image:
    pe, = unpack(data, 0x3C, "I")
    require(slice_bytes(data, pe, 4) == b"PE\0\0", "invalid PE signature")
    machine, count, _, _, _, optsize, _ = unpack(data, pe + 4, "HHIIIHH")
    optional = pe + 24
    magic, = unpack(data, optional, "H")
    require(machine == 0x8664 and magic == 0x20B and count <= MAX_SECTIONS and optsize >= 112,
            "requires PE32+ x86-64")
    base, = unpack(data, optional + 24, "Q")
    directories, = unpack(data, optional + 108, "I")
    require(directories <= 16 and 112 + directories * 8 <= optsize, "PE data directories")
    sections = []
    for i in range(count):
        name, virtual, rva, rawsize, offset, _, _, _, _, flags = unpack(
            data, optional + optsize + i * 40, "8sIIIIIIHHI")
        actual = virtual or rawsize
        sections.append(Section(name.rstrip(b"\0").decode("ascii"), base + rva, offset, actual,
                                min(rawsize, actual), bool(flags & 0x20000000), bool(flags & 0x80000000)))
    def rva_file(rva: int, size: int) -> int:
        matches = [s for s in sections if s.contains(base + rva, size) and
                   base + rva - s.address + size <= s.file_size]
        require(len(matches) == 1, "PE directory outside section")
        return matches[0].offset + base + rva - matches[0].address
    build_id = b""
    anchors = {}
    if directories:
        export_rva, export_size = unpack(data, optional + 112, "II")
        if export_size:
            directory = rva_file(export_rva, 40)
            _, _, _, _, _, _, functions, names, function_rva, names_rva, ordinal_rva = unpack(
                data, directory, "IIHHIIIIIII")
            require(names <= functions <= 65536, "PE export count budget")
            for i in range(names):
                name_rva, = unpack(data, rva_file(names_rva + i * 4, 4), "I")
                name_offset = rva_file(name_rva, 1)
                prefix = b"__llam_managed_anchor_"
                if not data.startswith(prefix, name_offset):
                    continue
                name = slice_bytes(data, name_offset, len(prefix) + 33)
                require(name[-1] == 0 and re.fullmatch(prefix + b"[0-9a-f]{32}\0", name) is not None,
                        "malformed PE image anchor export")
                ordinal, = unpack(data, rva_file(ordinal_rva + i * 2, 2), "H")
                require(ordinal < functions, "PE image anchor ordinal")
                address, = unpack(data, rva_file(function_rva + ordinal * 4, 4), "I")
                require(not export_rva <= address < export_rva + export_size, "forwarded PE image anchor")
                anchors[name[:-1].decode("ascii").lstrip("_")] = base + address
    if directories > 6:
        debug_rva, debug_size = unpack(data, optional + 112 + 6 * 8, "II")
        if debug_size:
            require(debug_size % 28 == 0 and debug_size <= 28 * MAX_SECTIONS, "PE debug directory")
            debug = rva_file(debug_rva, debug_size)
            for cursor in range(debug, debug + debug_size, 28):
                _, _, _, _, dtype, nbytes, _, fileoffset = unpack(data, cursor, "IIHHIIII")
                if dtype == 2 and nbytes >= 24 and slice_bytes(data, fileoffset, 4) == b"RSDS":
                    require(not build_id, "duplicate PE CodeView identity")
                    build_id = slice_bytes(data, fileoffset + 4, 16)
    if directories > 5:
        reloc_rva, reloc_size = unpack(data, optional + 112 + 5 * 8, "II")
        if reloc_size:
            cursor = rva_file(reloc_rva, reloc_size)
            end = cursor + reloc_size
            while cursor < end:
                page, block_size = unpack(data, cursor, "II")
                require(block_size >= 8 and block_size % 2 == 0 and block_size <= end - cursor,
                        "invalid PE relocation block")
                for offset in range(cursor + 8, cursor + block_size, 2):
                    item, = unpack(data, offset, "H")
                    if item >> 12:
                        target = base + page + (item & 4095)
                        require(not any(s.executable and s.contains(target) for s in sections),
                                "PE executable relocation requires an unsupported restore profile")
                cursor += block_size
    return Image(data, "pe", 1, 3, base, build_id, sections, False, anchors)


def read_image(path: Path) -> Image:
    require(path.stat().st_size <= 1024 * 1024 * 1024, "image exceeds verifier budget")
    data = path.read_bytes()
    if data.startswith(b"\x7fELF"):
        return read_elf(data)
    if data.startswith(b"\xcf\xfa\xed\xfe"):
        return read_macho(data)
    if data.startswith(b"MZ"):
        return read_pe(data)
    raise VerificationError("unsupported executable format (fat/bitcode/archive input is not a linked image)")


@dataclass(frozen=True)
class Range:
    begin: int
    end: int
    kind: int


@dataclass(frozen=True)
class Call:
    site: int
    continuation: int
    target: int
    effect: int
    type_id: int
    flags: int = 0
    contract_id: int = 0


@dataclass
class Entry:
    native: int
    body: int
    begin: int
    end: int
    execution_class: int
    effects: int
    type_id: int
    ranges: list[Range]
    calls: list[Call]
    flags: int
    guard_bindings: int
    native_end: int
    required_isa: int = 3


@dataclass
class Record:
    file_offset: int
    flags: int
    build_id: bytes
    executable_identity: int
    entries: list[Entry]
    address_fixups: list[tuple[int, int]]
    anchor_id: bytes


def parse_records(image: Image, *, allow_unsealed: bool = False) -> list[Record]:
    section = image.metadata()
    cursor = section.offset
    end = cursor + section.size
    records = []
    while cursor < end:
        # PE pads the metadata section to FileAlignment; virtual size excludes it.
        h = unpack(image.data, cursor, "IHHQIIIHHHHHHHHHHII16sQQQQQQQ")
        (magic, version, header, total, ne, nr, nc, es, rs, cs, arch, abi,
         mmaj, mmin, rmaj, rmin, endian, flags, optional, build_id, toolchain,
         entries_offset, ranges_offset, calls_offset, identity, reserved0, reserved1) = h
        require(magic == MAGIC and version == 2 and header == 128 and (es, rs, cs) == (96, 32, 48),
                "unknown LLPM v2 layout")
        require((arch, abi) == (image.architecture, image.abi) and
                (mmaj, mmin, rmaj, rmin, endian) == (1, 0, 2, 2, 1), "LLPM target/ABI mismatch")
        require(toolchain == PROFILE and flags & ~63 == 0 and flags & 26 == 26 and
                (optional == 0 or (allow_unsealed and image.kind == "pe" and optional == 1 and not flags & 4)) and
                (reserved0 != 0 or reserved1 != 0), "unqualified LLPM compiler contract")
        require(0 < ne <= 4096 and nr <= 65536 and nc <= 65536 and total <= MAX_RECORD_BYTES and
                total <= end - cursor, "LLPM bounds")
        require(entries_offset == 128 and ranges_offset == 128 + ne * 96 and
                calls_offset == ranges_offset + nr * 32 and total == calls_offset + nc * 48,
                "LLPM table alias/size")
        record_address = section.address + cursor - section.offset
        address_fixups = []
        def address(offset: int) -> int:
            if optional == 1:
                rva, = unpack(image.data, cursor + offset, "Q")
                require(rva <= 0xFFFFFFFF, "noncanonical COFF RVA64")
                result = image.base + rva
                address_fixups.append((cursor + offset, result - record_address - offset))
            else:
                relative, = unpack(image.data, cursor + offset, "q")
                result = record_address + offset + relative
            require(0 < result <= MASK64, "LLPM relative address overflow")
            return result
        entries = []
        next_range = next_call = 0
        previous_end = 0
        for i in range(ne):
            off = 128 + i * 96
            values = unpack(image.data, cursor + off + 32, "IIQIIIIIIQQQ")
            cls, effects, isa, frame, fr, nrange, fc, ncall, eflags, type_id, bindings_raw, native_end_raw = values
            require(cls <= 2 and effects & ~63 == 0 and isa & ~(19 if arch == 2 else 3) == 0 and frame == 1 and
                    eflags & ~31 == 0 and type_id != 0, "LLPM entry contract")
            bindings = address(off + 80) if flags & 32 else 0
            require(bool(flags & 32) or bindings_raw == 0, "undeclared guard bindings")
            native_end = address(off + 88) if native_end_raw else 0
            if bindings:
                require(bindings % 8 == 0, "unaligned native guard pointer pair")
                image.immutable_data(bindings, 16)
            native = address(off)
            body = address(off + 8) if cls else 0
            require(cls or unpack(image.data, cursor + off + 8, "Q")[0] == 0, "legacy managed entry")
            begin, finish = address(off + 16), address(off + 24)
            require(previous_end <= begin < finish and (not cls or begin <= body < finish),
                    "LLPM entry overlap/body extent")
            image.executable_section(native)
            image.executable_section(begin, finish - begin)
            require(not cls or ((native != body or eflags & 1) and effects & 1 and not effects & 32 and isa & 1 and bindings),
                    "native thunk/managed effect contract")
            if native_end:
                require(bindings and not eflags & 1 and native < native_end and
                        (native_end <= begin or native >= finish), "native thunk extent overlaps its body")
                image.executable_section(native, native_end - native)
            require(not cls or eflags & 1 or native_end, "managed native entry has no thunk extent")
            boundary = bool(eflags & 8)
            runtime_bridge = bool(eflags & 16)
            require(not boundary or (cls == 0 and eflags & 1 and bindings and not native_end and native == begin),
                    "guarded foreign boundary shape is invalid")
            require(not runtime_bridge or (cls == 0 and eflags == 17 and effects in (2, 16) and not bindings and
                                           not native_end and native == begin and nrange == 0 and ncall == 1),
                    "runtime bridge shape is invalid")
            require(cls != 1 or not effects & 16, "portable entry has native-affine effects")
            require(fr == next_range and fc == next_call and nrange <= nr - fr and ncall <= nc - fc,
                    "LLPM table ownership")
            ranges = []
            range_end = begin
            for j in range(fr, fr + nrange):
                roff = ranges_offset + j * 32
                owner, kind, rflags = unpack(image.data, cursor + roff, "IHH")
                rb, re_ = address(roff + 8), address(roff + 16)
                alignment, reserved = unpack(image.data, cursor + roff + 24, "II")
                require(owner == i and 1 <= kind <= 7 and rflags == reserved == 0 and
                        range_end <= rb < re_ <= finish and alignment <= 16, "LLPM range overlap/contract")
                if kind == 1:
                    require(cls != 0 and not eflags & 2 and alignment >= (4 if arch == 2 else 3), "legacy/unaligned SAFE")
                ranges.append(Range(rb, re_, kind))
                range_end = re_
            calls = []
            previous_site = begin - 1
            thunk_region = False
            for j in range(fc, fc + ncall):
                coff = calls_offset + j * 48
                owner, effect, cflags = unpack(image.data, cursor + coff, "IHH")
                native_call = bool(cflags & 4)
                runtime_call = bool(cflags & 64)
                guard = cflags & 24
                if native_call and not thunk_region:
                    previous_site = native - 1
                require(not thunk_region or native_call, "body call row after native thunk rows")
                thunk_region = native_call
                site, continuation = address(coff + 8), address(coff + 16)
                if cflags & 1 and not guard and not runtime_call:
                    require(unpack(image.data, cursor + coff + 24, "Q")[0] == 0, "noncanonical indirect target")
                    target = 0
                else:
                    target = address(coff + 24)
                call_type, contract_id = unpack(image.data, cursor + coff + 32, "QQ")
                low, high = (native, native_end) if native_call and not boundary else (begin, finish)
                require(owner == i and effect < 5 and cflags in (0, 4, 13, 21, 37, 65) and
                        (runtime_call or contract_id == 0) and call_type != 0 and
                        previous_site < site < continuation <= high and low <= site and
                        effects & (1 << effect), "unqualified call/effect")
                if runtime_call:
                    require(runtime_bridge and cflags == 65 and effect == runtime_contract_effect(contract_id) and call_type == type_id and
                            call_type == runtime_contract_type(contract_id, arch, abi),
                            "runtime bridge call/ABI contract mismatch")
                    image.immutable_data(target, 8)
                elif guard:
                    require(native_call and cflags & 1 and effect == 1 and guard in (8, 16) and
                            not cflags & 32 and target == bindings + (8 if guard == 16 else 0),
                            "native guard binding/call mismatch")
                else:
                    require(not cflags & 1 or (boundary and native_call and cflags & 32 and effect != 0),
                            "unchecked indirect call in the qualified image profile")
                require(not cflags & 32 or (boundary and native_call and cflags & 1 and not guard and effect != 0 and
                                           call_type == type_id),
                        "foreign body call is outside its guarded boundary")
                require(not any(r.kind == 1 and r.begin <= site < r.end for r in ranges), "call PC is SAFE")
                if not guard and not cflags & 1:
                    image.executable_section(target)
                calls.append(Call(site, continuation, target, effect, call_type, cflags, contract_id))
                previous_site = site
            entries.append(Entry(native, body, begin, finish, cls, effects, type_id, ranges, calls, eflags,
                                 bindings, native_end, isa))
            next_range += nrange
            next_call += ncall
            previous_end = finish
        require(next_range == nr and next_call == nc, "unowned LLPM rows")
        records.append(Record(cursor, flags, build_id, identity, entries, address_fixups,
                              struct.pack("<QQ", reserved0, reserved1)))
        cursor += total
    require(cursor == end and records, "LLPM trailing bytes")
    return records


# The final decoder is independent of LLVM MI flags. These mnemonic sets cover
# baseline integer/FP/SIMD instructions; system, TLS, exclusive and extended-state
# instructions require another profile. Branch/return/call boundaries stay unsafe.
ARM_SAFE = re.compile(
    r"^(add|adds|sub|subs|adc|adcs|sbc|sbcs|and|ands|orr|orn|eor|eon|bic|bics|"
    r"lsl|lsr|asr|ror|lslv|lsrv|asrv|rorv|ubfiz|ubfx|sbfiz|sbfx|bfi|bfxil|extr|"
    r"sxtb|sxth|sxtw|uxtb|uxth|uxtw|madd|msub|mul|mneg|smull|umull|smulh|umulh|"
    r"smaddl|umaddl|smsubl|umsubl|udiv|sdiv|mov|movz|movn|movk|mvn|neg|negs|"
    r"csel|csinc|csinv|csneg|cset|csetm|cinc|cinv|cneg|ccmp|ccmn|clz|cls|rbit|rev|rev16|rev32|"
    r"ldr|ldrb|ldrh|ldrsb|ldrsh|ldrsw|ldur|ldurb|ldurh|ldursb|ldursh|ldursw|"
    r"str|strb|strh|stur|sturb|sturh|ldp|ldpsw|stp|ldnp|stnp|ldar|ldarb|ldarh|"
    r"stlr|stlrb|stlrh|ldadd[a-z]*|ldclr[a-z]*|ldeor[a-z]*|ldset[a-z]*|swp[a-z]*|cas[a-z]*|"
    r"adr|adrp|b|b\.[a-z]+|cbz|cbnz|tbz|tbnz|nop|cmp|cmn|tst|"
    r"fadd|fsub|fmul|fdiv|fmax|fmin|fmaxnm|fminnm|fnmul|fabs|fneg|fsqrt|fmov|"
    r"fcmp|fcmpe|fcsel|fcvt|scvtf|ucvtf|fcvtzs|fcvtzu|fmadd|fmsub|fnmadd|fnmsub|"
    r"dup|ins|umov|smov|ext|movi|mvni|addv|addp|uaddlv|saddlv|"
    r"cmeq|cmgt|cmhi|smax|smin|umax|umin)$")
X86_SAFE = re.compile(
    r"^((mov|lea|add|adc|sub|sbb|and|or|xor|cmp|test|inc|dec|neg|not|shl|shr|sar|rol|ror|"
    r"imul|mul|idiv|div|xchg|xadd|cmpxchg)[bwlq]?|movabsq|mov[sz](bw|bl|bq|wl|wq|lq)|"
    r"cmpxchg(8b|16b)|cmov[a-z]+|set[a-z]+|push[wq]?|pop[wq]?|j[a-z]+|"
    r"cdq|cqo|cwd|cbw|cwde|cdqe|cltq|cqto|cltd|nop[wql]?|"
    r"(mov|add|sub|mul|div|sqrt|min|max|xor|and|or|shuf|unpck[lh])(ss|sd|ps|pd)|"
    r"mov(aps|ups|apd|upd|dqa|dqu|d|q)|ucomis[sd]|cvt(ss2sd|sd2ss|si2ss[q]?|si2sd[q]?)|"
    r"cvtts[sd]2si[q]?|(pxor|pand|pandn|por)|"
    r"(padd|psub|pmul|psll|psrl|psra|pshuf|pack|punpck|pcmpeq|pcmpgt|pmin|pmax)[a-z0-9]+)$")

FORBIDDEN_REGISTERS = re.compile(
    r"\b(z\d+|p\d+|za|zt\d+|ffr|x18|w18|ymm\d+|zmm\d+|k[0-7]|tmm\d+|"
    r"mm[0-7]|st(?:\([0-7]\))?|[cde]r\d+|[cdefgs]s|pkru|ssp)\b")


def qualified_instruction(mnemonic: str, operands: str, architecture: int, required_isa: int) -> None:
    normalized = mnemonic
    if architecture == 1 and normalized.startswith("lock_"):
        normalized = normalized.removeprefix("lock_")
        require(re.fullmatch(r"(add|adc|sub|sbb|and|or|xor|inc|dec|neg|not|xadd|xchg|cmpxchg)[bwlq]?|cmpxchg(8b|16b)",
                             normalized) is not None and "(" in operands,
                "unqualified locked instruction")
    if architecture == 2 and re.search(r"\.(8b|16b|4h|8h|2s|4s|1d|2d)$", mnemonic):
        normalized = mnemonic.rsplit(".", 1)[0]
    pattern = ARM_SAFE if architecture == 2 else X86_SAFE
    require(pattern.fullmatch(normalized) is not None, f"unqualified final opcode: {mnemonic}")
    # Comments contain symbol names, which are not architectural operands.
    actual_operands = operands.split("<", 1)[0].split("#", 1)[0] if architecture == 1 else operands.split("<", 1)[0]
    require(FORBIDDEN_REGISTERS.search(actual_operands.lower()) is None,
            "unqualified final register/TLS state")
    if architecture == 2 and re.fullmatch(r"(ldadd|ldclr|ldeor|ldset|swp|cas)[a-z]*", normalized):
        require(bool(required_isa & 16), "final LSE instruction lacks its ISA contract")


def branch_instruction(mnemonic: str, architecture: int) -> bool:
    if architecture == 2:
        return mnemonic == "b" or mnemonic.startswith("b.") or mnemonic in ("cbz", "cbnz", "tbz", "tbnz")
    return mnemonic.startswith("j")


def managed_body(entry: Entry, decoded: dict[int, tuple[int, str, str]], architecture: int,
                 noreturn_targets: set[int]) -> None:
    """Check every final body byte, including unsafe frame/call/return intervals."""
    if not entry.execution_class:
        return
    calls = {c.site: c for c in entry.calls if not c.flags & 4}
    ranges = iter(entry.ranges)
    current = next(ranges, None)
    pc = entry.begin
    starts = set()
    branches = []
    seen_calls = set()
    while pc < entry.end:
        require(pc in decoded, f"managed body is not on an instruction boundary: 0x{pc:x}")
        size, mnemonic, operands = decoded[pc]
        require(pc + size <= entry.end, "managed body ends inside an instruction")
        starts.add(pc)
        while current is not None and current.end <= pc:
            current = next(ranges, None)
        covered = current is not None and current.begin <= pc and pc + size <= current.end
        if not covered:
            # LLVM may insert alignment padding between the per-MI witnesses.
            require(mnemonic in ("nop", "nopw", "nopl", "nopq"), "unwitnessed managed body instruction")
        elif mnemonic in ("bl", "call", "callq"):
            require(pc in calls and current.kind == 3, "unrecorded managed body call")
            seen_calls.add(pc)
        elif mnemonic in ("ret", "retq"):
            require(current.kind == 5 and not entry.flags & 4, "unqualified managed body return")
            require(not operands.strip() if architecture == 1 else operands.strip() in ("", "x30", "lr"),
                    "managed return uses an unqualified register/stack adjustment")
        else:
            qualified_instruction(mnemonic, operands, architecture, entry.required_isa)
            if branch_instruction(mnemonic, architecture):
                require(not operands.strip().startswith("*"), "indirect managed branch")
                branches.append(direct_target(operands))
        if pc + size == entry.end:
            require(mnemonic in ("ret", "retq", "b", "jmp", "jmpq") or
                    (pc in calls and calls[pc].target in noreturn_targets),
                    "managed body falls through outside its extent")
        pc += size
    require(seen_calls == set(calls), "managed body has a missing call")
    require(entry.body in starts and all(target in starts for target in branches),
            "managed branch escapes its body or enters an instruction")


def decode(path: Path, objdump: Path, architecture: int) -> dict[int, tuple[int, str, str]]:
    result = subprocess.run([str(objdump), "--disassemble", "--print-imm-hex", str(path)],
                            check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    instructions = {}
    for line in result.stdout.splitlines():
        match = re.match(r"\s*([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{2}\s+)+)([^\s]+)(?:\s+(.*))?$", line)
        if match is None:
            # LLVM prints AArch64 raw encodings as one 8-digit word.
            match = re.match(r"\s*([0-9a-fA-F]+):\s+([0-9a-fA-F]{8})\s+([^\s]+)(?:\s+(.*))?$", line)
            size = 4
        else:
            size = len(match[2].split())
        if match is None:
            continue
        address = int(match[1], 16)
        require(address not in instructions, "ambiguous disassembly")
        instructions[address] = (size, match[3].lower(), match[4] or "")
    require(bool(instructions), f"no decoded instructions for architecture {architecture}")
    if architecture == 1:
        # LLVM prints LOCK as a separate disassembly line, but it belongs to
        # the following instruction. Neither ranges nor branches may enter it
        # after the prefix and accidentally certify a different operation.
        for pc, (size, mnemonic, _) in list(instructions.items()):
            if mnemonic == "lock":
                following = instructions.pop(pc + size, None)
                require(following is not None, "unterminated LOCK prefix")
                next_size, next_mnemonic, operands = following
                instructions[pc] = (size + next_size, "lock_" + next_mnemonic, operands)
    return instructions


def direct_target(operands: str) -> int:
    values = re.findall(r"\b0x([0-9a-fA-F]+)\b", operands.split("<", 1)[0])
    require(bool(values), "native thunk has an unresolved direct branch")
    return int(values[-1], 16)


def native_thunk(entry: Entry, decoded: dict[int, tuple[int, str, str]], architecture: int, native_abi: int) -> None:
    """Bounded abstract execution of the final native thunk's control/data flow.

    Track image addresses and pointers loaded from the immutable guard pair.
    Unknown register definitions lose their provenance. Every reachable body
    call and return must be enclosed by the exact enter/leave pair; a branch
    around the enter call cannot be hidden by retaining its metadata row.
    """
    boundary = bool(entry.flags & 8)
    runtime_bridge = bool(entry.flags & 16)
    if not entry.native_end and not boundary and not runtime_bridge:
        return
    begin = entry.begin if boundary or runtime_bridge else entry.native
    end = entry.end if boundary or runtime_bridge else entry.native_end
    calls = {c.site: c for c in entry.calls if c.flags & (4 | 64)}
    expected = [0] if runtime_bridge else ([8, 0] if entry.flags & 4 else [8, 0, 16])
    require([c.flags & 24 for c in calls.values()] == expected, "native thunk guard protocol is incomplete")
    actual = {pc for pc, (_, mnemonic, _) in decoded.items() if begin <= pc < end and
              mnemonic in ("bl", "blr", "call", "callq")}
    require(actual == set(calls), "native thunk has an unrecorded or missing call")
    if entry.flags & 4:
        require(not any(mnemonic.startswith("ret") for pc, (_, mnemonic, _) in decoded.items()
                        if begin <= pc < end), "noreturn native thunk contains a return")
    work = [(begin, 0, False, ())]
    visited = set()
    terminals = 0

    def arm_reg(value: str) -> str:
        value = value.strip().lower()
        if value == "fp":
            return "x29"
        if value == "lr":
            return "x30"
        return "x" + value[1:] if re.fullmatch(r"w\d+", value) else value

    def xreg(value: str) -> str:
        value = value.strip().lower().lstrip("%")
        for full, aliases in (("rax", ("eax", "ax", "al", "ah")), ("rbx", ("ebx", "bx", "bl", "bh")),
                              ("rcx", ("ecx", "cx", "cl", "ch")), ("rdx", ("edx", "dx", "dl", "dh")),
                              ("rsi", ("esi", "si", "sil")), ("rdi", ("edi", "di", "dil")),
                              ("rbp", ("ebp", "bp", "bpl")), ("rsp", ("esp", "sp", "spl"))):
            if value in aliases:
                return full
        return re.sub(r"^(r(?:[8-9]|1[0-5]))[dwb]$", r"\1", value)

    def full_xreg(value: str) -> bool:
        return re.fullmatch(r"%r(ax|bx|cx|dx|si|di|bp|sp|[8-9]|1[0-5])", value.strip()) is not None

    while work:
        pc, depth, body_seen, state = work.pop()
        key = (pc, depth, body_seen, state)
        if key in visited:
            continue
        require(len(visited) < 32768, "native thunk control-flow budget exceeded")
        visited.add(key)
        require(begin <= pc < end and pc in decoded, "native thunk branches outside its extent")
        size, mnemonic, operands = decoded[pc]
        require(pc + size <= end, "native thunk extent ends inside an instruction")
        registers = dict(state)
        next_pc = pc + size
        if pc in calls:
            call = calls[pc]
            guard = call.flags & 24
            if guard or runtime_bridge:
                if runtime_bridge:
                    require(call.flags == 65 and call.effect == runtime_contract_effect(call.contract_id) and call.type_id == entry.type_id and
                            call.type_id == runtime_contract_type(call.contract_id, architecture, native_abi),
                            "invalid runtime bridge ABI")
                else:
                    require(call.type_id == abi_hash(f"void ():0:{architecture}:{native_abi}"), "invalid native guard ABI")
                if architecture == 2:
                    require(mnemonic == "blr" and registers.get(arm_reg(operands)) == ("slot", call.target),
                            "native guard call is detached from its immutable binding")
                else:
                    require(mnemonic in ("call", "callq"), "native guard is not a call")
                    clean = operands.split("#", 1)[0].strip()
                    if re.fullmatch(r"\*(?:-?(?:0x[0-9a-f]+|\d+))?\(%rip\)", clean):
                        match = re.search(r"#\s*0x([0-9a-fA-F]+)", operands)
                        require(match is not None and int(match[1], 16) == call.target,
                                "native guard memory call uses the wrong binding")
                    else:
                        reg = clean.removeprefix("*")
                        require(clean.startswith("*") and full_xreg(reg) and
                                registers.get(xreg(reg)) == ("slot", call.target),
                                "native guard call is detached from its immutable binding")
                if runtime_bridge:
                    require(not body_seen and depth == 0, "runtime bridge operation executes out of order")
                    body_seen = True
                elif guard == 8:
                    require(depth == 0 and not body_seen, "native enter guard executes out of order")
                    depth = 1
                else:
                    require(depth == 1 and body_seen, "native leave guard executes out of order")
                    depth = 0
            else:
                require(depth == 1 and not body_seen,
                        "managed body can be reached without the native enter guard")
                if boundary:
                    require(call.flags == 37 and call.effect != 0 and call.target == 0 and call.type_id == entry.type_id,
                            "foreign boundary has no guarded native operation")
                else:
                    require(call.effect == 0 and call.target == entry.native and
                            direct_target(operands) == (entry.body or entry.begin), "native thunk calls the wrong body")
                body_seen = True
                if entry.flags & 4:
                    terminals += 1
                    continue
            # Baseline C caller-saved registers. Callee-saved binding base
            # addresses survive a call; caller-saved pointer values do not.
            saved = {f"x{i}" for i in range(19, 30)} if architecture == 2 else {
                "rbx", "rbp", "rsp", "r12", "r13", "r14", "r15"}
            if architecture == 1 and native_abi == 3:
                saved.update(("rsi", "rdi"))
            registers = {r: v for r, v in registers.items() if r in saved}
        elif mnemonic in ("ret", "retq"):
            require(depth == 0 and body_seen and not entry.flags & 4,
                    "native thunk returns without its body/leave guard")
            require(not operands.strip() if architecture == 1 else operands.strip() in ("", "x30", "lr"),
                    "native thunk has an unqualified return")
            terminals += 1
            continue
        elif (architecture == 2 and mnemonic in ("brk", "udf")) or mnemonic == "ud2":
            raise VerificationError("native thunk has a reachable unqualified trap")
        elif branch_instruction(mnemonic, architecture):
            require(not operands.strip().startswith("*"), "native thunk has an indirect branch")
            target = direct_target(operands)
            work.append((target, depth, body_seen, tuple(sorted(registers.items()))))
            if mnemonic in ("b", "jmp", "jmpq"):
                continue
        else:
            qualified_instruction(mnemonic, operands, architecture, entry.required_isa)
            if architecture == 2:
                clean = operands.split("<", 1)[0]
                parts = re.split(r",\s*(?![^\[]*\])", clean)
                destination = arm_reg(parts[0]) if parts else ""
                old = dict(registers)
                if re.fullmatch(r"[xw]\d+|sp", destination) and not mnemonic.startswith(("st", "cmp", "tst", "cmn")):
                    registers.pop(arm_reg(destination), None)
                if mnemonic == "ldp" and len(parts) > 1:
                    registers.pop(arm_reg(parts[1]), None)
                if "!" in clean or re.search(r"\],\s*#", clean):
                    memory_base = re.search(r"\[([a-z0-9]+)", clean)
                    if memory_base is not None:
                        registers.pop(arm_reg(memory_base[1]), None)
                if mnemonic in ("adr", "adrp") and len(parts) > 1:
                    registers[destination] = ("addr", direct_target(parts[1]))
                elif mnemonic == "mov" and len(parts) == 2 and re.fullmatch(r"x\d+", parts[0].strip()) and re.fullmatch(r"x\d+", parts[1].strip()):
                    value = old.get(arm_reg(parts[1]))
                    if value is not None:
                        registers[destination] = value
                elif mnemonic == "add" and len(parts) == 3 and parts[0].strip().startswith("x") and parts[1].strip().startswith("x"):
                    base = old.get(arm_reg(parts[1]))
                    imm = re.fullmatch(r"#(0x[0-9a-fA-F]+|\d+)", parts[2].strip())
                    if base is not None and base[0] == "addr" and imm is not None:
                        registers[destination] = ("addr", base[1] + int(imm[1], 0))
                elif mnemonic == "ldr" and len(parts) == 2 and parts[0].strip().startswith("x"):
                    memory = re.fullmatch(r"\[([a-z0-9]+)(?:,\s*#(0x[0-9a-fA-F]+|\d+))?\]", parts[1].strip())
                    if memory is not None:
                        base = old.get(arm_reg(memory[1]))
                        if base is not None and base[0] == "addr":
                            registers[destination] = ("slot", base[1] + (int(memory[2], 0) if memory[2] else 0))
                    elif re.match(r"0x[0-9a-fA-F]+", parts[1].strip()):
                        registers[destination] = ("slot", direct_target(parts[1]))
                elif mnemonic not in ("mov", "add", "sub", "adr", "adrp", "ldr", "ldp", "str", "stp", "cmp", "cmn", "tst", "nop"):
                    # Other qualified operations may define multiple registers
                    # (CAS, pair loads, SIMD lane transfers). Do not infer their
                    # data flow from a single textual destination operand.
                    registers.clear()
            else:
                clean = operands.split("#", 1)[0].strip()
                parts = [part.strip() for part in re.split(r",\s*(?![^\(]*\))", clean)]
                destination = xreg(parts[-1]) if parts and parts[-1].startswith("%") else ""
                old = dict(registers)
                if destination and not mnemonic.startswith(("cmp", "test", "push")):
                    registers.pop(destination, None)
                if mnemonic in ("movq", "movabsq", "leaq") and len(parts) == 2 and full_xreg(parts[1]):
                    if "(%rip)" in parts[0]:
                        match = re.search(r"#\s*0x([0-9a-fA-F]+)", operands)
                        if match is not None and re.fullmatch(r"(?:-?(?:0x[0-9a-f]+|\d+))?\(%rip\)", parts[0]):
                            registers[destination] = ("addr" if mnemonic.startswith("lea") else "slot", int(match[1], 16))
                    elif mnemonic != "leaq" and full_xreg(parts[0]) and xreg(parts[0]) in old:
                        registers[destination] = old[xreg(parts[0])]
                    else:
                        memory = re.fullmatch(r"(-?(?:0x[0-9a-fA-F]+|\d+))?\(%([a-z0-9]+)\)", parts[0])
                        if memory is not None and full_xreg("%" + memory[2]) and old.get(memory[2], (None,))[0] == "addr":
                            registers[destination] = ("addr" if mnemonic == "leaq" else "slot",
                                                      old[memory[2]][1] + (int(memory[1], 0) if memory[1] else 0))
                elif not re.fullmatch(r"(mov[a-z0-9]*|lea[lw]?|add[bwlq]?|adc[bwlq]?|sub[bwlq]?|sbb[bwlq]?|"
                                      r"and[bwlq]?|or[bwlq]?|xor[bwlq]?|inc[bwlq]?|dec[bwlq]?|neg[bwlq]?|not[bwlq]?|"
                                      r"shl[bwlq]?|shr[bwlq]?|sar[bwlq]?|rol[bwlq]?|ror[bwlq]?|"
                                      r"cmp[bwlq]?|test[bwlq]?|push[wq]?|pop[wq]?|nop[wql]?)", mnemonic):
                    # Implicit/multiple definitions (mul/div, xchg, xadd,
                    # cmpxchg, sign extension) invalidate all tracked values.
                    registers.clear()
                if mnemonic.startswith(("push", "pop")):
                    registers.pop("rsp", None)
        require(next_pc < end, "native thunk falls through without a checked return")
        work.append((next_pc, depth, body_seen, tuple(sorted(registers.items()))))
    require(terminals > 0, "native thunk has no checked terminal path")


def abi_hash(signature: str) -> int:
    value = 0xCBF29CE484222325
    for byte in signature.encode("ascii"):
        value = ((value ^ byte) * 0x100000001B3) & MASK64
    return value


@cache
def runtime_contracts() -> dict[int, tuple[str, int]]:
    source = Path(__file__).with_name("managed_runtime_contracts.def").read_text(encoding="ascii")
    contracts = {}
    effects = {"RUNTIME_CRITICAL": 1, "NATIVE_AFFINE": 4}
    for identifier, signature, effect in re.findall(
            r'^LLAM_RUNTIME_CONTRACT\((\d+),\s*[A-Za-z0-9_]+,\s*"([vpiqfh]+)",\s*([A-Z_]+)\)$', source, re.MULTILINE):
        number = int(identifier)
        require(number not in contracts, "duplicate SDK runtime contract")
        require(effect in effects, "unknown SDK runtime effect")
        contracts[number] = (signature, effects[effect])
    require(bool(contracts), "SDK runtime contracts are unavailable")
    return contracts


def runtime_contract_type(contract_id: int, architecture: int, native_abi: int) -> int:
    contract = runtime_contracts().get(contract_id)
    require(contract is not None, "unknown SDK runtime contract")
    signature, _ = contract
    types = {"v": "void", "p": "ptr", "i": "i32", "q": "i64",
             "f": "i64" if native_abi == 3 else "i32", "h": "ptr" if native_abi == 3 else "i32"}
    arguments = ", ".join(types[code] for code in signature[1:])
    return abi_hash(f"{types[signature[0]]} ({arguments}):0:{architecture}:{native_abi}")


def runtime_contract_effect(contract_id: int) -> int:
    contract = runtime_contracts().get(contract_id)
    require(contract is not None, "unknown SDK runtime contract")
    return contract[1]


def verify(path: Path, objdump: Path, *, seal: bool = False) -> dict:
    image = read_image(path)
    require(not image.relocatable, "final verification requires a linked image")
    require(len(image.build_id) == 16 and any(image.build_id), "linked image has no supported loader build ID")
    records = parse_records(image, allow_unsealed=seal)
    nm = objdump.with_name("llvm-nm")
    command = [str(nm), "--defined-only", "--extern-only", "--format=posix", str(path)]
    symbols = dict(image.anchor_symbols)
    if image.kind == "macho":
        result = subprocess.run(command, check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for line in result.stdout.splitlines():
            fields = line.split()
            if len(fields) >= 3 and re.fullmatch(r"_?__llam_managed_anchor_[0-9a-f]{32}", fields[0]):
                symbols[fields[0].lstrip("_")] = int(fields[2], 16)
    metadata = image.metadata()
    contributions = {}
    for record in records:
        name = "llam_managed_anchor_" + record.anchor_id.hex()
        nbytes, = unpack(image.data, record.file_offset + 8, "Q")
        contributions.setdefault(name, []).append((record.file_offset, nbytes))
    for name, spans in contributions.items():
        begin = spans[0][0]
        end = begin
        for offset, nbytes in spans:
            require(offset == end, "image anchor contribution is not contiguous")
            end += nbytes
        require(symbols.get(name) == metadata.address + begin - metadata.offset,
                "image anchor is missing or does not identify its metadata contribution")
        require(image.kind != "elf" or image.anchor_sizes[name] == end - begin,
                "ELF image anchor size does not cover its metadata contribution")
    entries = sorted((e for record in records for e in record.entries), key=lambda e: e.begin)
    require(all(a.end <= b.begin for a, b in zip(entries, entries[1:])), "cross-record code overlap/ICF")
    extents = [(e.begin, e.end) for e in entries] + [(e.native, e.native_end) for e in entries if e.native_end]
    extents.sort()
    require(all(a[1] <= b[0] for a, b in zip(extents, extents[1:])), "native thunk overlaps another code extent")
    native = {e.native: e for e in entries}
    require(len(native) == len(entries), "duplicate native entry/ICF")
    decoded = decode(path, objdump, image.architecture)
    noreturn_targets = {e.native for e in entries if e.flags & 4}
    safe_count = 0
    for entry in entries:
        managed_body(entry, decoded, image.architecture, noreturn_targets)
        for r in entry.ranges:
            require(not any(r.kind == 1 and r.begin <= n < r.end for n, e in native.items() if not e.flags & 1),
                    "native entry in SAFE")
            pc = r.begin
            while pc < r.end:
                require(pc in decoded, f"range is not on an instruction boundary: 0x{pc:x}")
                size, mnemonic, operands = decoded[pc]
                require(pc + size <= r.end, "range ends inside an instruction")
                if r.kind == 1:
                    qualified_instruction(mnemonic, operands, image.architecture, entry.required_isa)
                    safe_count += 1
                pc += size
        for call in entry.calls:
            require(call.site in decoded and decoded[call.site][0] == call.continuation - call.site,
                    "call boundary changed after certification")
            mnemonic = decoded[call.site][1]
            if call.flags & (24 | 32 | 64):
                require(call.flags & (4 | 64) and mnemonic in ("bl", "blr", "call", "callq"),
                        "native boundary call is malformed")
                continue
            require(mnemonic in ("bl", "call", "callq"), "call record does not describe a direct call")
            target = native.get(call.target)
            require(target is not None and target.type_id == call.type_id,
                    "typed call target is absent from the immutable image")
            actual_target = re.match(r"\s*0x([0-9a-fA-F]+)\b", decoded[call.site][2])
            require(actual_target is not None and int(actual_target[1], 16) ==
                    ((target.body or target.begin) if call.effect == 0 else target.native),
                    "final call target changed after certification")
            if call.effect == 0:
                require(target.execution_class != 0 or (call.flags & 4 and target is entry),
                        "managed call closure/type is unqualified")
                require(entry.execution_class != 1 or target.execution_class == 1,
                        "portable caller reaches native-affine callee")
            else:
                require(target.execution_class == 0 and target.effects & (1 << call.effect),
                        "foreign/runtime bridge effect does not match the call")
                require(bool(target.flags & 24),
                        "foreign/runtime call target has no verified bridge protocol")
        native_thunk(entry, decoded, image.architecture, image.abi)
    identity = image.executable_identity()
    if seal:
        updated = bytearray(image.data)
        for record in records:
            struct.pack_into("<I", updated, record.file_offset + 48, record.flags | 4)
            struct.pack_into("<I", updated, record.file_offset + 52, 0)
            for offset, displacement in record.address_fixups:
                struct.pack_into("<q", updated, offset, displacement)
            updated[record.file_offset + 56:record.file_offset + 72] = image.build_id
            struct.pack_into("<Q", updated, record.file_offset + 104, identity)
        path.write_bytes(updated)
    else:
        require(all(r.flags & 4 and r.build_id == image.build_id and r.executable_identity == identity
                    for r in records), "linked code/build identity does not match its seal")
    return {"format": image.kind, "architecture": image.architecture, "build_id": image.build_id.hex(),
            "executable_identity": f"{identity:016x}", "records": len(records), "entries": len(entries),
            "managed_entries": sum(e.execution_class != 0 for e in entries), "safe_instructions": safe_count}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--objdump", type=Path, required=True)
    parser.add_argument("--seal", action="store_true")
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.image, args.objdump, seal=args.seal), sort_keys=True))
        return 0
    except (VerificationError, OSError, UnicodeError, struct.error, subprocess.SubprocessError) as error:
        parser.exit(1, f"llam-managed-verify: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
