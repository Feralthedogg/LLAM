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
# See LICENSES/Apache-2.0.txt.

"""Native exact-result compiler oracle and independent final-image mutations."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "preempt"))
from managed_verify import (Call, Entry, VerificationError, abi_hash, decode, native_thunk,
                            parse_records, read_image, verify)  # noqa: E402

OPTIONS: argparse.Namespace


class ManagedCompilerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory(prefix=".llam-managed-tests-", dir=ROOT)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.llvm = OPTIONS.llvm_bin.resolve()
        cls.plugin = OPTIONS.plugin.resolve()
        cls.env = os.environ.copy()
        cls.env.update(LLAM_LLVM_BIN=str(cls.llvm), LLAM_MANAGED_PLUGIN=str(cls.plugin),
                       PYTHONDONTWRITEBYTECODE="1")
        cls.native_flags = []
        if sys.platform == "darwin":
            sdk = subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()
            cls.native_flags = ["-isysroot", sdk]
        cls.consumer = cls.work / "consumer.o"
        subprocess.run([str(cls.llvm / "clang"), *cls.native_flags, "-O2", "-c",
                        str(ROOT / "tests/fixtures/managed/native_consumer.c"), "-o", str(cls.consumer)], check=True)
        cls.binary = cls.work / ("native.exe" if os.name == "nt" else "native")
        result = cls.driver("--llam-managed=required", str(ROOT / "tests/fixtures/managed/kernels.c"),
                            str(cls.consumer), "-o", str(cls.binary))
        if result.returncode:
            raise AssertionError(result.stderr[:6000])
        cls.image = read_image(cls.binary)
        cls.records = parse_records(cls.image)
        cls.entries = [e for record in cls.records for e in record.entries]

    @classmethod
    def driver(cls, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, str(ROOT / "tools/preempt/managed_driver.py"), *args],
                              env=cls.env, capture_output=True, text=True, timeout=30)

    def test_native_arrays_stack_recursion_atomics_tls_and_domain_transitions(self) -> None:
        result = subprocess.run([str(self.binary)], capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        report = verify(self.binary, self.llvm / "llvm-objdump")
        self.assertGreaterEqual(report["managed_entries"], 6)
        self.assertGreater(report["safe_instructions"], 40)
        self.assertTrue(any(e.execution_class == 2 for e in self.entries))
        self.assertTrue(any(e.flags & 1 for e in self.entries))
        self.assertTrue(any(c.effect == 0 for e in self.entries for c in e.calls))
        self.assertTrue(any(c.effect == 4 for e in self.entries for c in e.calls))

    def test_compiler_required_rejects_unresolved_effects(self) -> None:
        fixtures = {
            "indirect": "long entry(long (*f)(long),long n){return f(n);}",
            "assembly": 'long entry(void){__asm__ volatile("nop");return 1;}',
            "dynamic_stack": "long entry(unsigned n){volatile long a[n];a[n-1]=3;return a[n-1];}",
            "unknown_native": "extern long foreign(long);long entry(long n){return foreign(n);}",
            "no_preempt": '__attribute__((annotate("llam-no-preempt"))) long entry(long n){return n+1;}',
        }
        for name, source in fixtures.items():
            with self.subTest(name=name):
                path = self.work / (name + ".c")
                path.write_text(source + "\n")
                output = path.with_suffix(".o")
                result = self.driver("--llam-managed=required", "-c", str(path), "-o", str(output))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("unqualified", result.stderr)
                self.assertFalse(output.exists())

    def test_noreturn_native_entry_has_no_leave_witness(self) -> None:
        entries = [entry for entry in self.entries if entry.flags & 4 and entry.native_end]
        self.assertTrue(entries)
        for entry in entries:
            calls = [call for call in entry.calls if call.flags & 4]
            self.assertEqual(len(calls), 2)
            self.assertTrue(calls[0].flags & 8)
            self.assertFalse(any(call.flags & 16 for call in calls))

    def test_auto_keeps_unknown_code_legacy_without_safe_ranges(self) -> None:
        path = self.work / "legacy.c"
        path.write_text("extern long foreign(long);long entry(long n){return foreign(n);}\n")
        output = path.with_suffix(".o")
        result = self.driver("--llam-managed=auto", "-c", str(path), "-o", str(output))
        self.assertEqual(result.returncode, 0, result.stderr[:2500])
        image = read_image(output)
        section = image.metadata()
        data = image.data[section.offset:section.offset + section.size]
        self.assertEqual(struct.unpack_from("<I", data, 128 + 32)[0], 0)
        ranges_offset = struct.unpack_from("<Q", data, 88)[0]
        count = struct.unpack_from("<I", data, 20)[0]
        self.assertTrue(all(struct.unpack_from("<H", data, ranges_offset + i * 32 + 4)[0] != 1
                            for i in range(count)))

    def test_link_transforms_and_foreign_plugins_are_refused(self) -> None:
        for flag in ("-flto", "-ffunction-sections", "-fpass-plugin=other.so", "-Wl,--icf=all",
                     "-Wl,--gc-sections", "-Wl,-dead_strip", "-fsanitize=thread", "@options.rsp"):
            with self.subTest(flag=flag):
                output = self.work / "refused.o"
                result = self.driver("-c", str(ROOT / "tests/fixtures/managed/kernels.c"), flag, "-o", str(output))
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(output.exists())

    def mutation(self, name: str, data: bytes) -> Path:
        path = self.work / name
        path.write_bytes(data)
        return path

    def test_final_native_entry_cannot_alias_safe_pc_even_when_resealing(self) -> None:
        record = next(r for r in self.records if any(e.execution_class == 1 and not e.flags & 1 for e in r.entries))
        entry = record.entries[0]
        pc = next(r.begin for r in entry.ranges if r.kind == 1 and r.begin != entry.body)
        data = bytearray(self.image.data)
        section = self.image.metadata()
        field_offset = record.file_offset + 128
        field_address = section.address + field_offset - section.offset
        struct.pack_into("<q", data, field_offset, pc - field_address)
        with self.assertRaises(VerificationError):
            verify(self.mutation("native-in-safe", data), self.llvm / "llvm-objdump", seal=True)

    def test_unknown_final_instruction_cannot_be_resealed(self) -> None:
        entry = next(e for e in self.entries if e.execution_class == 1)
        pc = next(r.begin for r in entry.ranges if r.kind == 1)
        section = self.image.executable_section(pc)
        offset = section.offset + pc - section.address
        data = bytearray(self.image.data)
        opcode = b"\x01\x00\x00\xd4" if self.image.architecture == 2 else b"\x0f\x05"
        data[offset:offset + len(opcode)] = opcode
        with self.assertRaises(VerificationError):
            verify(self.mutation("unknown-opcode", data), self.llvm / "llvm-objdump", seal=True)

    def test_unsafe_frame_instruction_cannot_hide_a_call_trap_or_escape(self) -> None:
        entry, frame = next((e, r) for e in self.entries if e.execution_class for r in e.ranges
                            if r.kind == 4 and e.end - r.begin >= 5)
        section = self.image.executable_section(frame.begin)
        offset = section.offset + frame.begin - section.address
        if self.image.architecture == 2:
            displacement = ((entry.native - frame.begin) >> 2) & 0x03FFFFFF
            mutations = (struct.pack("<I", 0xD4000001), struct.pack("<I", 0x94000000 | displacement),
                         struct.pack("<I", 0x14000000 | displacement))
        else:
            displacement = struct.pack("<i", entry.native - frame.begin - 5)
            mutations = (b"\x0f\x05", b"\xe8" + displacement, b"\xe9" + displacement)
        for index, opcode in enumerate(mutations):
            with self.subTest(index=index):
                data = bytearray(self.image.data)
                data[offset:offset + len(opcode)] = opcode
                with self.assertRaises(VerificationError):
                    verify(self.mutation(f"unsafe-frame-{index}", data), self.llvm / "llvm-objdump", seal=True)

    def test_managed_direct_branch_cannot_escape_to_native_entry(self) -> None:
        decoded = decode(self.binary, self.llvm / "llvm-objdump", self.image.architecture)
        candidates = ((e, pc, size, e.native if self.image.architecture == 2 else target)
                      for e in self.entries if e.execution_class and e.native_end
                      for pc, (size, op, _) in decoded.items() if e.begin <= pc < e.end and
                      (op == "b" if self.image.architecture == 2 else op.startswith("j"))
                      for target in (e.begin - 1, e.end)
                      if self.image.architecture == 2 or size > 2 or -128 <= target - pc - size <= 127)
        entry, pc, size, target = next(candidates)
        section = self.image.executable_section(pc)
        offset = section.offset + pc - section.address
        data = bytearray(self.image.data)
        if self.image.architecture == 2:
            struct.pack_into("<I", data, offset, 0x14000000 | (((target - pc) >> 2) & 0x03FFFFFF))
        else:
            displacement_bytes = 1 if size == 2 else 4
            struct.pack_into("<b" if size == 2 else "<i", data, offset + size - displacement_bytes, target - pc - size)
        with self.assertRaisesRegex(VerificationError, "branch escapes"):
            verify(self.mutation("body-branch-escape", data), self.llvm / "llvm-objdump", seal=True)

    def test_managed_return_cannot_be_changed_to_fallthrough(self) -> None:
        decoded = decode(self.binary, self.llvm / "llvm-objdump", self.image.architecture)
        entry, pc, size = next((e, pc, size) for e in self.entries if e.execution_class
                              for pc, (size, op, _) in decoded.items()
                              if op in ("ret", "retq") and pc + size == e.end)
        section = self.image.executable_section(pc)
        offset = section.offset + pc - section.address
        data = bytearray(self.image.data)
        data[offset:offset + size] = b"\x1f\x20\x03\xd5" if self.image.architecture == 2 else b"\x90" * size
        with self.assertRaisesRegex(VerificationError, "falls through"):
            verify(self.mutation("body-fallthrough", data), self.llvm / "llvm-objdump", seal=True)

    def test_final_call_target_is_checked_independently_of_metadata(self) -> None:
        entry = next(e for e in self.entries if any(c.effect == 0 for c in e.calls))
        call = next(c for c in entry.calls if c.effect == 0)
        other = next(e for e in self.entries if e.execution_class and e.native != call.target and e.body != call.site)
        section = self.image.executable_section(call.site)
        offset = section.offset + call.site - section.address
        data = bytearray(self.image.data)
        if self.image.architecture == 2:
            encoded, = struct.unpack_from("<I", data, offset)
            encoded = (encoded & 0xFC000000) | (((other.body - call.site) >> 2) & 0x03FFFFFF)
            struct.pack_into("<I", data, offset, encoded)
        else:
            self.assertEqual(data[offset], 0xE8)
            struct.pack_into("<i", data, offset + 1, other.body - call.site - 5)
        with self.assertRaisesRegex(VerificationError, "call target changed"):
            verify(self.mutation("changed-call-target", data), self.llvm / "llvm-objdump", seal=True)

    def test_text_and_loader_id_must_match_the_seal(self) -> None:
        data = bytearray(self.image.data)
        data[self.records[0].file_offset + 56] ^= 1
        with self.assertRaisesRegex(VerificationError, "identity does not match"):
            verify(self.mutation("changed-build-id", data), self.llvm / "llvm-objdump")
        data = bytearray(self.image.data)
        struct.pack_into("<Q", data, self.records[0].file_offset + 104, 0)
        with self.assertRaisesRegex(VerificationError, "identity does not match"):
            verify(self.mutation("changed-text-seal", data), self.llvm / "llvm-objdump")

    def test_unsealed_and_wrong_machine_contracts_are_refused(self) -> None:
        for offset, value in ((48, 2), (34, 99), (72, 0), (128 + 40, 0x80)):
            with self.subTest(offset=offset):
                data = bytearray(self.image.data)
                data[self.records[0].file_offset + offset] = value
                with self.assertRaises(VerificationError):
                    verify(self.mutation(f"bad-contract-{offset}", data), self.llvm / "llvm-objdump")

    def bypass_guard(self, entry, target: int, name: str) -> None:
        pc = entry.begin if entry.flags & 8 else entry.native
        section = self.image.executable_section(pc)
        offset = section.offset + pc - section.address
        data = bytearray(self.image.data)
        if self.image.architecture == 2:
            struct.pack_into("<I", data, offset, 0x14000000 | (((target - pc) >> 2) & 0x03FFFFFF))
        else:
            data[offset] = 0xE9
            struct.pack_into("<i", data, offset + 1, target - pc - 5)
        with self.assertRaises(VerificationError):
            verify(self.mutation(name, data), self.llvm / "llvm-objdump", seal=True)

    def test_native_entry_cannot_branch_around_enter(self) -> None:
        entry = next(e for e in self.entries if e.native_end and not e.flags & 4)
        body = next(c for c in entry.calls if c.flags & 4 and not c.flags & 24)
        self.bypass_guard(entry, body.site, "native-entry-bypasses-guard")

    def test_foreign_boundary_cannot_branch_around_enter(self) -> None:
        entry = next(e for e in self.entries if e.flags & 8 and not e.flags & 4)
        body = next(c for c in entry.calls if c.flags & 32)
        self.bypass_guard(entry, body.site, "foreign-boundary-bypasses-guard")

    def test_guard_pointer_load_is_verified_after_final_link(self) -> None:
        entry = next(e for e in self.entries if e.native_end and not e.flags & 4)
        leave = next(c for c in entry.calls if c.flags & 16)
        decoded = decode(self.binary, self.llvm / "llvm-objdump", self.image.architecture)
        data = bytearray(self.image.data)
        if self.image.architecture == 2:
            pc = max(pc for pc, (_, op, operands) in decoded.items() if entry.native <= pc < leave.site and
                     op == "ldr" and "#0x8" in operands)
            section = self.image.executable_section(pc)
            offset = section.offset + pc - section.address
            instruction, = struct.unpack_from("<I", data, offset)
            struct.pack_into("<I", data, offset, instruction & ~(0xFFF << 10))
        else:
            size, _, operands = decoded[leave.site]
            if "(%rip)" in operands:
                section = self.image.executable_section(leave.site)
                offset = section.offset + leave.site - section.address
                struct.pack_into("<i", data, offset + size - 4, entry.guard_bindings - leave.site - size)
            else:
                pc = max(pc for pc, (_, op, operands) in decoded.items() if entry.native <= pc < leave.site and
                         op.startswith("mov") and "(%rip)" in operands)
                size = decoded[pc][0]
                section = self.image.executable_section(pc)
                offset = section.offset + pc - section.address
                struct.pack_into("<i", data, offset + size - 4, entry.guard_bindings - pc - size)
        with self.assertRaisesRegex(VerificationError, "binding"):
            verify(self.mutation("changed-guard-pointer-load", data), self.llvm / "llvm-objdump", seal=True)

    def test_noreturn_native_entry_has_exact_two_call_protocol(self) -> None:
        entry = next(e for e in self.entries if e.native_end and e.flags & 4)
        self.assertEqual([c.flags & 24 for c in entry.calls if c.flags & 4], [8, 0])

    def test_sdk_runtime_bridge_has_exact_abi_slot_and_checked_control_flow(self) -> None:
        source = self.work / "runtime-bridge.c"
        source.write_text("extern void llam_yield(void);\n"
                          "long runtime_entry(long n){llam_yield();return n+1;}\n")
        consumer = self.work / "runtime-consumer.c"
        consumer.write_text("extern long runtime_entry(long); static int yields;\n"
                            "void llam_yield(void){++yields;}\n"
                            "void llam_managed_native_enter(void){}\n"
                            "void llam_managed_native_leave(void){}\n"
                            "int main(void){return runtime_entry(41)!=42 || yields!=1;}\n")
        obj = consumer.with_suffix(".o")
        subprocess.run([str(self.llvm / "clang"), *self.native_flags, "-O2", "-c", str(consumer), "-o", str(obj)],
                       check=True, capture_output=True, text=True)
        binary = self.work / "runtime-bridge"
        result = self.driver("--llam-managed=required", str(source), str(obj), "-o", str(binary))
        self.assertEqual(result.returncode, 0, result.stderr[:4000])
        self.assertEqual(subprocess.run([str(binary)], timeout=10).returncode, 0)
        image = read_image(binary)
        records = parse_records(image)
        record = next(record for record in records if any(e.flags & 16 for e in record.entries))
        entry = next(e for e in record.entries if e.flags & 16)
        self.assertEqual(len(entry.calls), 1)
        call = entry.calls[0]
        self.assertEqual((call.flags, call.contract_id), (65, 1))
        section = image.executable_section(call.site)
        offset = section.offset + call.site - section.address
        data = bytearray(image.data)
        if image.architecture == 2:
            instruction, = struct.unpack_from("<I", data, offset)
            struct.pack_into("<I", data, offset, instruction & ~(1 << 21))  # BLR becomes BR.
        else:
            self.assertEqual(data[offset], 0xFF)
            data[offset + 1] = (data[offset + 1] & ~0x38) | 0x20  # CALL r/m64 becomes JMP r/m64.
        with self.assertRaises(VerificationError):
            verify(self.mutation("runtime-operation-jump", data), self.llvm / "llvm-objdump", seal=True)
        calls_offset, = struct.unpack_from("<Q", image.data, record.file_offset + 96)
        data = bytearray(image.data)
        struct.pack_into("<Q", data, record.file_offset + calls_offset + 40, 0xFFFFFFFF)
        with self.assertRaisesRegex(VerificationError, "runtime contract"):
            verify(self.mutation("runtime-unknown-contract", data), self.llvm / "llvm-objdump", seal=True)

    def test_unsupported_isa_and_call_role_flags_cannot_be_resealed(self) -> None:
        for bit in (4, 8):
            data = bytearray(self.image.data)
            field = self.records[0].file_offset + 128 + 40
            isa, = struct.unpack_from("<Q", data, field)
            struct.pack_into("<Q", data, field, isa | bit)
            with self.subTest(isa=bit), self.assertRaises(VerificationError):
                verify(self.mutation(f"unsupported-isa-{bit}", data), self.llvm / "llvm-objdump", seal=True)
        record = next(r for r in self.records if any(e.calls for e in r.entries))
        calls_offset, = struct.unpack_from("<Q", self.image.data, record.file_offset + 96)
        field = record.file_offset + calls_offset + 6
        for bit in (2, 32):
            data = bytearray(self.image.data)
            flags, = struct.unpack_from("<H", data, field)
            struct.pack_into("<H", data, field, flags | bit)
            with self.subTest(call_flag=bit), self.assertRaises(VerificationError):
                verify(self.mutation(f"invalid-call-role-{bit}", data), self.llvm / "llvm-objdump", seal=True)


class CrossNativeThunkTests(unittest.TestCase):
    """Real x86-64 cross-linked decoder tests; these images are never executed."""

    def test_elf_tls_zero_fill_cannot_authorize_image_storage(self) -> None:
        linker = shutil.which("ld.lld")
        if linker is None:
            self.skipTest("cross-linker ld.lld is unavailable")
        source, obj, binary = (self.work / ("tls-template" + suffix)
                               for suffix in (".c", ".o", ".so"))
        # Container parsing only: this byte is deliberately not a certificate.
        source.write_text('__attribute__((section(".llam.managed"),used)) '
                          'const unsigned char metadata[1] = {0};\n'
                          '__thread long value; long entry(void) { return value; }\n')
        subprocess.run([str(self.llvm / "clang"), "--target=x86_64-unknown-linux-gnu",
                        "-fPIC", "-c", str(source), "-o", str(obj)],
                       check=True, capture_output=True, text=True)
        subprocess.run([linker, "-shared", "--build-id=uuid", "-z", "separate-code",
                        "-o", str(binary), str(obj)],
                       check=True, capture_output=True, text=True)
        image = read_image(binary)
        self.assertFalse(any(section.name == ".tbss" for section in image.sections))
        data = bytearray(binary.read_bytes())
        phoff, shoff = struct.unpack_from("<QQ", data, 32)
        phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", data, 54)
        tls = next(phoff + index * phentsize for index in range(phnum)
                   if struct.unpack_from("<I", data, phoff + index * phentsize)[0] == 7)
        zero_fill = next(shoff + index * shentsize for index in range(shnum)
                         if struct.unpack_from("<I", data, shoff + index * shentsize + 4)[0] == 8 and
                         struct.unpack_from("<Q", data, shoff + index * shentsize + 8)[0] & 0x400)
        for mutation in ("executable", "missing-template", "outside-template"):
            changed = bytearray(data)
            if mutation == "executable":
                flags, = struct.unpack_from("<Q", changed, zero_fill + 8)
                struct.pack_into("<Q", changed, zero_fill + 8, flags | 4)
            elif mutation == "missing-template":
                struct.pack_into("<I", changed, tls, 0)
            else:
                address, = struct.unpack_from("<Q", changed, zero_fill + 16)
                struct.pack_into("<Q", changed, zero_fill + 16, address + 4096)
            invalid = self.work / ("tls-" + mutation + ".so")
            invalid.write_bytes(changed)
            with self.subTest(mutation=mutation), self.assertRaises(VerificationError):
                read_image(invalid)

    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory(prefix=".llam-managed-cross-tests-", dir=ROOT)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.llvm = OPTIONS.llvm_bin.resolve()

    def link(self, kind: str, objects: list[Path], image: Path) -> None:
        linker_name = {"elf": "ld.lld", "macho": "ld64.lld", "pe": "lld-link"}[kind]
        linker = shutil.which(linker_name)
        if linker is None:
            self.skipTest(f"cross-linker {linker_name} is unavailable")
        if kind == "elf":
            command = [linker, "-pie", "--build-id=uuid", "-z", "separate-code", "-z", "relro", "-z", "now",
                       "--export-dynamic-symbol=__llam_managed_anchor_*",
                       "-e", "thunk", "-o", str(image)]
        elif kind == "macho":
            command = [linker, "-arch", "x86_64", "-platform_version", "macos", "13.0", "13.0",
                       "-e", "thunk", "-o", str(image)]
        else:
            command = [linker, "/entry:thunk", "/subsystem:console", "/nodefaultlib", "/debug",
                       "/debug:dwarf", "/pdb:" + str(image.with_suffix(".pdb")), "/out:" + str(image)]
        result = subprocess.run([*command, *(str(obj) for obj in objects)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr[:4000])

    def fixture(self, kind: str, probe: str = "nop", *, folded: bool = False):
        targets = {"elf": ("x86_64-unknown-linux-gnu", 1),
                   "macho": ("x86_64-apple-macos13", 2),
                   "pe": ("x86_64-pc-windows-msvc", 3)}
        triple, abi = targets[kind]
        source, obj, image = (self.work / (kind + suffix) for suffix in (".s", ".o", ".image"))
        enter = "callq *guards(%rip)" if folded else "callq *%rax"
        leave = "callq *guards+8(%rip)" if folded else "callq *%rax"
        source.write_text(f""".text
.globl thunk, thunk_end, enter_site, body_site, leave_site, body, guards
thunk:
    pushq %rbx
    leaq guards(%rip), %rbx
    movq (%rbx), %rax
    {probe}
enter_site:
    {enter}
body_site:
    callq body
    movq 8(%rbx), %rax
leave_site:
    {leave}
    popq %rbx
    retq
thunk_end:
body:
    retq
guard_enter:
    retq
guard_leave:
    retq
.data
.p2align 3
guards:
    .quad guard_enter, guard_leave
""")
        subprocess.run([str(self.llvm / "clang"), "--target=" + triple, "-c", str(source), "-o", str(obj)],
                       check=True, capture_output=True, text=True)
        self.link(kind, [obj], image)
        symbols_text = subprocess.check_output([str(self.llvm / "llvm-nm"), "--format=posix", str(image)], text=True)
        symbols = {parts[0]: int(parts[2], 16) for line in symbols_text.splitlines()
                   if len(parts := line.split()) >= 3 and re.fullmatch(r"[0-9a-fA-F]+", parts[2])}
        decoded = decode(image, self.llvm / "llvm-objdump", 1)
        native = symbols["thunk"]
        bindings = symbols["guards"]
        calls = []
        for name, target, effect, type_id, flags in (
                ("enter_site", bindings, 1, abi_hash(f"void ():0:1:{abi}"), 13),
                ("body_site", native, 0, 123, 4),
                ("leave_site", bindings + 8, 1, abi_hash(f"void ():0:1:{abi}"), 21)):
            pc = symbols[name]
            calls.append(Call(pc, pc + decoded[pc][0], target, effect, type_id, flags))
        entry = Entry(native, symbols["body"], symbols["body"], symbols["body"] + 1,
                      1, 3, 123, [], calls, 0, bindings, symbols["thunk_end"])
        return entry, decoded, abi, image

    def managed_fixture(self, kind: str) -> Path:
        triple = {"elf": "x86_64-unknown-linux-gnu", "macho": "x86_64-apple-macos13",
                  "pe": "x86_64-pc-windows-msvc"}[kind]
        source, obj, shim, shim_obj, image = (self.work / ("managed-" + kind + suffix)
                                            for suffix in (".c", ".o", "-shim.s", "-shim.o", ".image"))
        source.write_text("long entry(long *p){return __atomic_fetch_add(p,3,5)*7;}\n"
                          "double floating(double x){return x*3.0+1.0;}\n"
                          "extern void llam_yield(void);\n"
                          "long runtime_entry(long n){llam_yield();return n+1;}\n")
        env = os.environ.copy()
        env.update(LLAM_LLVM_BIN=str(self.llvm), LLAM_MANAGED_PLUGIN=str(OPTIONS.plugin.resolve()),
                   PYTHONDONTWRITEBYTECODE="1")
        result = subprocess.run([sys.executable, str(ROOT / "tools/preempt/managed_driver.py"),
                                 "--llam-managed=required", "--target=" + triple, "-c", str(source), "-o", str(obj)],
                                env=env, text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr[:4000])
        prefix = "_" if kind == "macho" else ""
        symbols = ["thunk", prefix + "llam_managed_native_enter", prefix + "llam_managed_native_leave",
                   prefix + "llam_yield", prefix + "__stack_chk_fail"]
        shim.write_text(".text\n.globl " + ", ".join(symbols) + "\n" +
                        "".join(name + ":\n retq\n" for name in symbols) +
                        (".data\n.globl _fltused\n_fltused:\n .long 0\n" if kind == "pe" else ""))
        subprocess.run([str(self.llvm / "clang"), "--target=" + triple, "-c", str(shim), "-o", str(shim_obj)],
                       check=True, capture_output=True, text=True)
        self.link(kind, [obj, shim_obj], image)
        return image

    def test_cross_linked_full_managed_images_with_atomics_and_simd(self) -> None:
        for kind in ("elf", "macho", "pe"):
            with self.subTest(format=kind):
                path = self.managed_fixture(kind)
                report = verify(path, self.llvm / "llvm-objdump", seal=True)
                self.assertEqual(report["format"], kind)
                self.assertGreaterEqual(report["managed_entries"], 3)
                verify(path, self.llvm / "llvm-objdump")

    def test_cross_linked_x86_64_guard_traces_in_elf_macho_and_pe(self) -> None:
        for kind in ("elf", "macho", "pe"):
            with self.subTest(format=kind):
                entry, decoded, abi, _ = self.fixture(kind)
                native_thunk(entry, decoded, 1, abi)

    def test_cross_linked_x86_64_folded_guard_loads(self) -> None:
        entry, decoded, abi, _ = self.fixture("elf", folded=True)
        native_thunk(entry, decoded, 1, abi)

    def test_x86_partial_implicit_and_multiple_definitions_kill_pointer_provenance(self) -> None:
        for probe in ("movl $0, %eax", "movw $0, %ax", "movb $0, %al", "movb $0, %ah",
                      "movl (%rbx), %eax", "xchgq %rax, %rcx", "mulq %rcx", "cltq",
                      "movq %rax, %r8; movb $0, %r8b; movq %r8, %rax",
                      "movl $0, %ebx; movq (%rbx), %rax", "leaq (%rbx), %rax"):
            with self.subTest(probe=probe):
                entry, decoded, abi, _ = self.fixture("elf", probe)
                with self.assertRaisesRegex(VerificationError, "binding"):
                    native_thunk(entry, decoded, 1, abi)

    def test_x86_conditional_guard_bypass_and_unrecorded_call_are_rejected(self) -> None:
        for probe in ("jmp body_site", "je body_site", "callq body"):
            with self.subTest(probe=probe):
                entry, decoded, abi, _ = self.fixture("elf", probe)
                with self.assertRaises(VerificationError):
                    native_thunk(entry, decoded, 1, abi)

    def test_arm_narrow_writes_and_load_writeback_kill_pointer_provenance(self) -> None:
        linker = shutil.which("ld64.lld")
        if linker is None:
            self.skipTest("cross-linker ld64.lld is unavailable")
        source, obj, image = (self.work / ("arm-trace" + suffix) for suffix in (".s", ".o", ".image"))
        for probe in ("nop", "mov w8, #0", "add w19, w19, #0\n ldr x8, [x19]",
                      "ldr x0, [x19], #8\n ldr x8, [x19]"):
            with self.subTest(probe=probe):
                source.write_text(f""".text
.globl thunk, thunk_end, body, enter_site, body_site, leave_site, guards
thunk:
    stp x19, x30, [sp, #-16]!
    adrp x19, guards@PAGE
    add x19, x19, guards@PAGEOFF
    ldr x8, [x19]
    {probe}
enter_site:
    blr x8
body_site:
    bl body
    ldr x8, [x19, #8]
leave_site:
    blr x8
    ldp x19, x30, [sp], #16
    ret
thunk_end:
body:
    ret
guard_enter:
    ret
guard_leave:
    ret
.data
.p2align 3
guards:
    .quad guard_enter, guard_leave
""")
                subprocess.run([str(self.llvm / "clang"), "--target=arm64-apple-macos13", "-c", str(source), "-o", str(obj)],
                               check=True, capture_output=True, text=True)
                subprocess.run([linker, "-arch", "arm64", "-platform_version", "macos", "13.0", "13.0",
                                "-e", "thunk", str(obj), "-o", str(image)], check=True, capture_output=True, text=True)
                output = subprocess.check_output([str(self.llvm / "llvm-nm"), "--format=posix", str(image)], text=True)
                symbols = {parts[0]: int(parts[2], 16) for line in output.splitlines()
                           if len(parts := line.split()) >= 3}
                native, bindings, body = (symbols[name] for name in ("thunk", "guards", "body"))
                guard_type = abi_hash("void ():0:2:2")
                calls = [Call(symbols[name], symbols[name] + 4, target, effect, type_id, flags)
                         for name, target, effect, type_id, flags in (
                             ("enter_site", bindings, 1, guard_type, 13),
                             ("body_site", native, 0, 123, 4),
                             ("leave_site", bindings + 8, 1, guard_type, 21))]
                entry = Entry(native, body, body, body + 4, 1, 3, 123, [], calls, 0, bindings, symbols["thunk_end"])
                decoded = decode(image, self.llvm / "llvm-objdump", 2)
                if probe == "nop":
                    native_thunk(entry, decoded, 2, 2)
                else:
                    with self.assertRaisesRegex(VerificationError, "binding"):
                        native_thunk(entry, decoded, 2, 2)

    def test_cross_macho_section_bytes_must_match_loader_segment(self) -> None:
        _, _, _, path = self.fixture("macho")
        original = read_image(path)
        data = bytearray(original.data)
        cursor = 32
        ncmds, = struct.unpack_from("<I", data, 16)
        for _ in range(ncmds):
            command, size = struct.unpack_from("<II", data, cursor)
            if command == 0x19:
                count, = struct.unpack_from("<I", data, cursor + 64)
                for index in range(count):
                    section = cursor + 72 + index * 80
                    if data[section:section + 16].rstrip(b"\0") == b"__text":
                        offset, = struct.unpack_from("<I", data, section + 48)
                        struct.pack_into("<I", data, section + 48, offset + 1)
            cursor += size
        path.write_bytes(data)
        with self.assertRaisesRegex(VerificationError, "loader mapping"):
            read_image(path)

    def test_cross_elf_section_bytes_must_match_loader_segment(self) -> None:
        path = self.managed_fixture("elf")
        original = read_image(path)
        text = next(s for s in original.sections if s.name == ".text")
        data = bytearray(original.data)
        shoff, = struct.unpack_from("<Q", data, 40)
        count, = struct.unpack_from("<H", data, 60)
        for index in range(count):
            section = shoff + index * 64
            address, offset = struct.unpack_from("<QQ", data, section + 16)
            if address == text.address and offset == text.offset:
                struct.pack_into("<Q", data, section + 24, offset + 1)
        path.write_bytes(data)
        with self.assertRaisesRegex(VerificationError, "loader mapping"):
            read_image(path)

    def test_cross_elf_anchor_requires_exact_exported_contribution_size(self) -> None:
        path = self.managed_fixture("elf")
        image = read_image(path)
        data = bytearray(image.data)
        shoff, = struct.unpack_from("<Q", data, 40)
        count, = struct.unpack_from("<H", data, 60)
        mutated = False
        for index in range(count):
            section = shoff + index * 64
            stype, = struct.unpack_from("<I", data, section + 4)
            if stype != 11:
                continue
            offset, size, link = struct.unpack_from("<QQI", data, section + 24)
            strings_offset, strings_size = struct.unpack_from("<QQ", data, shoff + link * 64 + 24)
            strings = data[strings_offset:strings_offset + strings_size]
            for symbol in range(offset, offset + size, 24):
                name, = struct.unpack_from("<I", data, symbol)
                if strings[name:].startswith(b"__llam_managed_anchor_"):
                    nbytes, = struct.unpack_from("<Q", data, symbol + 16)
                    struct.pack_into("<Q", data, symbol + 16, nbytes - 1)
                    mutated = True
        self.assertTrue(mutated)
        path.write_bytes(data)
        with self.assertRaisesRegex(VerificationError, "anchor size"):
            verify(path, self.llvm / "llvm-objdump", seal=True)


def main() -> int:
    global OPTIONS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", type=Path, required=True)
    parser.add_argument("--plugin", type=Path, required=True)
    OPTIONS, remaining = parser.parse_known_args()
    return not unittest.main(argv=[sys.argv[0], *remaining], exit=False).result.wasSuccessful()


if __name__ == "__main__":
    raise SystemExit(main())
