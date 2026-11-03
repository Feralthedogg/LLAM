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

"""Audit SDK ABI declarations and reject name-only effects in managed compilation."""

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
sys.path.insert(0, str(ROOT / "tools/preempt"))
from managed_verify import VerificationError, parse_records, read_image, verify  # noqa: E402
CONTRACTS = ROOT / "tools/preempt/managed_runtime_contracts.def"
OPTIONS: argparse.Namespace


def contracts() -> list[tuple[int, str, str]]:
    return [(int(identity), name, signature) for identity, name, signature in re.findall(
        r'^LLAM_RUNTIME_CONTRACT\((\d+), (\w+), "([vpiqfh]+)", [A-Z_]+\)$', CONTRACTS.read_text(), re.MULTILINE)]


class RuntimeContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.temp = tempfile.TemporaryDirectory(prefix=".llam-runtime-contract-tests-", dir=ROOT)
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name)
        cls.llvm = OPTIONS.llvm_bin.resolve()
        cls.plugin = OPTIONS.plugin.resolve()
        cls.env = os.environ.copy()
        cls.env.update(LLAM_LLVM_BIN=str(cls.llvm), LLAM_MANAGED_PLUGIN=str(cls.plugin),
                       PYTHONDONTWRITEBYTECODE="1", LLAM_MANAGED_REQUIRED="1")
        cls.native_flags = []
        if sys.platform == "darwin":
            cls.native_flags = ["-isysroot", subprocess.check_output(["xcrun", "--show-sdk-path"], text=True).strip()]
        cls.triple = subprocess.check_output([str(cls.llvm / "clang"), "-dumpmachine"], text=True).strip()

    def command(self, command: list[str]) -> subprocess.CompletedProcess[str]:
        return subprocess.run(command, env=self.env, text=True, capture_output=True, timeout=30)

    def driver(self, *arguments: str) -> subprocess.CompletedProcess[str]:
        return self.command([sys.executable, str(ROOT / "tools/preempt/managed_driver.py"),
                             "--llam-managed=required", *arguments])

    def test_contract_table_matches_public_header_abi(self) -> None:
        rows = contracts()
        self.assertEqual(len(rows), len({row[0] for row in rows}))
        self.assertEqual(len(rows), len({row[1] for row in rows}))
        path = self.work / "public-contracts.c"
        path.write_text('#include "llam/managed.h"\n#include "llam/io.h"\n' + "".join(
            f"__typeof__(&{name}) const contract_{identity} = &{name};\n" for identity, name, _ in rows))
        ir = path.with_suffix(".ll")
        result = self.command([str(self.llvm / "clang"), *self.native_flags, "-I", str(ROOT / "include"),
                               "-S", "-emit-llvm", str(path), "-o", str(ir)])
        self.assertEqual(result.returncode, 0, result.stderr)
        declarations = {name: (result_type, arguments) for result_type, name, arguments in re.findall(
            r'^declare\s+(.+?)\s+@(llam_\w+)\((.*?)\)', ir.read_text(), re.MULTILINE)}
        windows = "windows" in self.triple or "mingw" in self.triple
        types = {"v": "void", "p": "ptr", "i": "i32", "q": "i64",
                 "f": "i64" if windows else "i32", "h": "ptr" if windows else "i32"}
        for _, name, signature in rows:
            with self.subTest(name=name):
                result_type, arguments = declarations[name]
                actual = [re.findall(r'\b(void|ptr|i32|i64)\b', result_type)[-1]]
                actual.extend(re.findall(r'\b(ptr|i32|i64)\b', arguments))
                self.assertEqual(actual, [types[code] for code in signature])
                self.assertNotRegex(result_type + " " + arguments,
                    r'\b(byval|byref|sret|inalloca|preallocated|inreg|signext|zeroext|noext|'
                    r'swiftself|swifterror|swiftasync|nest|alignstack)\b')

    def test_exact_runtime_prototypes_compile(self) -> None:
        path = self.work / "runtime-calls.c"
        path.write_text('#include "llam/runtime.h"\n#include "llam/io.h"\n'
                        'int entry(llam_channel_t *channel, void **out) {\n'
                        '  llam_task_safepoint();\n'
                        '  int result = llam_sleep_ns(1);\n'
                        '  return result + llam_channel_recv_result(channel, out);\n}\n')
        result = self.driver("-I", str(ROOT / "include"), "-c", str(path), "-o", str(path.with_suffix(".o")))
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_wrong_runtime_and_libc_prototypes_are_not_effect_contracts(self) -> None:
        cases = {
            "runtime_result": "extern long llam_yield(void); long entry(void){return llam_yield();}",
            "runtime_arguments": "extern int llam_join(long); int entry(long n){return llam_join(n);}",
            "runtime_vararg": "extern int llam_sleep_ns(unsigned long long,...); int entry(void){return llam_sleep_ns(1,2);}",
            "affine_result": "extern long llam_call_native_affine_result(void*(*)(void*),void*,void**); long entry(void*(*f)(void*),void *p){void *r;return llam_call_native_affine_result(f,p,&r);}",
            "stale_safepoint": "extern void llam_safepoint(void); void entry(void){llam_safepoint();}",
            "stale_sleep": "extern int llam_sleep_for(unsigned long long); int entry(void){return llam_sleep_for(1);}",
            "stale_blocking": "extern int llam_blocking_call(void); int entry(void){return llam_blocking_call();}",
            "libc_result": "extern int malloc(unsigned long); int entry(unsigned long n){return malloc(n);}",
            "libc_arguments": "extern void *memcpy(void*,void*); void *entry(void *p,void *q){return memcpy(p,q);}",
            "errno_arguments": "extern int *__error(int); int *entry(void){return __error(7);}",
        }
        for name, source in cases.items():
            with self.subTest(name=name):
                path = self.work / (name + ".c")
                path.write_text(source + "\n")
                output = path.with_suffix(".o")
                result = self.driver("-fno-builtin", "-c", str(path), "-o", str(output))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("unqualified", result.stderr)
                self.assertFalse(output.exists())

    def test_calling_convention_and_abi_attribute_mismatches_are_rejected(self) -> None:
        cases = {
            "runtime_cc": 'declare fastcc void @llam_yield()\ndefine void @entry(){call fastcc void @llam_yield()\nret void}',
            "runtime_abi": 'declare i32 @llam_join(ptr inreg)\ndefine i32 @entry(ptr %p){%v=call i32 @llam_join(ptr inreg %p)\nret i32 %v}',
            "runtime_result_attr": 'declare zeroext i32 @llam_join(ptr)\ndefine i32 @entry(ptr %p){%v=call zeroext i32 @llam_join(ptr %p)\nret i32 %v}',
            "callsite_type": 'declare void @llam_yield()\ndefine i64 @entry(){%v=call i64 @llam_yield()\nret i64 %v}',
            "callsite_attr": 'declare i32 @llam_join(ptr)\ndefine i32 @entry(ptr %p){%v=call i32 @llam_join(ptr inreg %p)\nret i32 %v}',
            "guard_type": 'declare i32 @llam_managed_native_enter(i32)\ndefine i32 @entry(){ret i32 1}',
            "libc_cc": 'declare fastcc ptr @malloc(i64)\ndefine ptr @entry(i64 %n){%v=call fastcc ptr @malloc(i64 %n)\nret ptr %v}',
            "libc_abi": 'declare ptr @malloc(i64 inreg)\ndefine ptr @entry(i64 %n){%v=call ptr @malloc(i64 inreg %n)\nret ptr %v}',
        }
        for name, body in cases.items():
            with self.subTest(name=name):
                source = self.work / (name + ".ll")
                source.write_text(f'target triple = "{self.triple}"\n' + body + "\n")
                result = self.command([str(self.llvm / "opt"), "-load-pass-plugin=" + str(self.plugin),
                                       "-passes=llam-managed", "-S", str(source), "-o", str(source.with_suffix(".out.ll"))])
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("unqualified", result.stderr)

    def test_memory_intrinsic_boundaries_preserve_bytes_and_balanced_domain(self) -> None:
        source = self.work / "memory.c"
        source.write_text('void copy(void *d,const void *s,unsigned long n){__builtin_memcpy(d,s,n);}\n'
                          'void move(void *d,const void *s,unsigned long n){__builtin_memmove(d,s,n);}\n'
                          'void set(void *d,int value,unsigned long n){__builtin_memset(d,value,n);}\n')
        consumer = self.work / "memory-consumer.c"
        consumer.write_text('#include <stdlib.h>\n#include <string.h>\n'
                            'static unsigned depth, enters;\n'
                            'void llam_managed_native_enter(void){++depth;++enters;}\n'
                            'void llam_managed_native_leave(void){if(!depth)abort();--depth;}\n'
                            'void copy(void*,const void*,unsigned long);\n'
                            'void move(void*,const void*,unsigned long);\n'
                            'void set(void*,int,unsigned long);\n'
                            'int main(void){unsigned char a[4096],b[4096];\n'
                            'for(unsigned i=0;i<4096;++i)a[i]=(unsigned char)(i*17);\n'
                            'copy(b,a,4096);if(memcmp(a,b,4096))return 1;\n'
                            'move(b+13,b,4000);if(memcmp(b+13,a,4000))return 2;\n'
                            'set(b,0xa5,4096);for(unsigned i=0;i<4096;++i)if(b[i]!=0xa5)return 3;\n'
                            'copy(0,0,0);move(0,0,0);set(0,1,0);\n'
                            'return depth||enters!=9;}\n')
        obj = consumer.with_suffix(".o")
        result = self.command([str(self.llvm / "clang"), *self.native_flags, "-O2", "-c", str(consumer), "-o", str(obj)])
        self.assertEqual(result.returncode, 0, result.stderr)
        image = self.work / "memory-native"
        result = self.driver(str(source), str(obj), "-o", str(image))
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.command([str(image)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def native_consumer(self, name: str, body: str) -> Path:
        source = self.work / (name + ".c")
        source.write_text('#include <stdlib.h>\nstatic unsigned depth, entries;\n'
                          'void llam_managed_native_enter(void){++depth;++entries;}\n'
                          'void llam_managed_native_leave(void){if(!depth)abort();--depth;}\n' + body)
        output = source.with_suffix(".o")
        result = self.command([str(self.llvm / "clang"), *self.native_flags, "-O2", "-c", str(source), "-o", str(output)])
        self.assertEqual(result.returncode, 0, result.stderr)
        return output

    def test_finite_local_function_pointer_set_has_exact_results(self) -> None:
        source = self.work / "finite.c"
        source.write_text('__attribute__((noinline)) static long left(long n){volatile long a[2];a[0]=n;a[1]=7;return a[0]+a[1];}\n'
                          '__attribute__((noinline)) static long right(long n){return n*3;}\n'
                          'long choose(int which,long n){long (*f)(long)=which?left:right;return f(n);}\n')
        consumer = self.native_consumer("finite-consumer",
            'long choose(int,long);int main(void){for(long i=0;i<10;++i){\n'
            'if(choose(0,i)!=i*3||choose(1,i)!=i+7)return 1;}return depth||entries!=20;}\n')
        image = self.work / "finite-native"
        result = self.driver(str(source), str(consumer), "-o", str(image))
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.command([str(image)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cross_translation_unit_requires_exact_certified_definition(self) -> None:
        caller = self.work / "cross-caller.c"
        caller.write_text('#include "llam/managed.h"\nint cross(int);\n'
                          'int entry(int n){return LLAM_MANAGED_TARGET(cross)(n)+1;}\n')
        callee = self.work / "cross-callee.c"
        callee.write_text('int cross(int n){return n+9;}\n')
        objects = []
        for source in (caller, callee):
            output = source.with_suffix(".o")
            result = self.driver("-I", str(ROOT / "include"), "-c", str(source), "-o", str(output))
            self.assertEqual(result.returncode, 0, result.stderr)
            objects.append(output)
        consumer = self.native_consumer("cross-consumer",
            'int entry(int);int cross(int);int main(void){\n'
            'if(entry(5)!=15||cross(5)!=14)return 1;return depth||entries!=2;}\n')
        image = self.work / "cross-native"
        result = self.driver(*(str(path) for path in (*objects, consumer)), "-o", str(image))
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.command([str(image)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

        body_symbol = ("_" if sys.platform == "darwin" else "") + "cross.llam.body"
        replacements = {
            "wrong-type": ('long cross(long n){return n+9;}\n', True),
            "native-only": ('int cross(int n){return n+9;}\n', False),
            "unverified-body": (f'int fake(int) __asm__("{body_symbol}");\n'
                                'int fake(int n){return n+9;}int cross(int n){return fake(n);}\n', False),
        }
        for name, (text, managed) in replacements.items():
            with self.subTest(name=name):
                source = self.work / (name + ".c")
                source.write_text(text)
                output = source.with_suffix(".o")
                if managed:
                    result = self.driver("-c", str(source), "-o", str(output))
                else:
                    result = self.command([str(self.llvm / "clang"), *self.native_flags, "-c", str(source), "-o", str(output)])
                self.assertEqual(result.returncode, 0, result.stderr)
                failed = self.work / (name + "-linked")
                result = self.driver(str(objects[0]), str(output), str(consumer), "-o", str(failed))
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(failed.exists())
                if name != "native-only":
                    self.assertIn("typed call target", result.stderr)

    def test_managed_marker_escape_and_unknown_target_sets_are_rejected(self) -> None:
        cases = {
            "escape": ('int cross(int);int (*entry(void))(int){return LLAM_MANAGED_TARGET(cross);}', "escaped"),
            "vararg": ('int cross(int,...);int entry(void){return LLAM_MANAGED_TARGET(cross)(1,2);}', "fixed C function"),
            "abi-cast": ('int cross(int);long entry(long n){return ((long(*)(long))LLAM_MANAGED_TARGET(cross))(n);}', "unqualified call ABI"),
            "external-set": ('long external(long);static long local(long n){return n+1;}\n'
                             'long entry(int b,long n){long(*f)(long)=b?external:local;return f(n);}', "unqualified"),
        }
        for name, (text, diagnostic) in cases.items():
            with self.subTest(name=name):
                source = self.work / (name + ".c")
                source.write_text('#include "llam/managed.h"\n' + text + "\n")
                result = self.driver("-I", str(ROOT / "include"), "-c", str(source), "-o", str(source.with_suffix(".o")))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(diagnostic, result.stderr)

    def test_shared_elf_metadata_does_not_bind_native_exports_locally(self) -> None:
        linker = shutil.which("ld.lld")
        if linker is None:
            self.skipTest("ELF cross-linker ld.lld is unavailable")
        source = self.work / "elf-caller.c"
        source.write_text('extern void *__llam_managed_target(void *);extern int helper(int);\n'
                          'int entry(int n){return ((int(*)(int))__llam_managed_target((void*)helper))(n);}\n')
        helper = self.work / "elf-helper.c"
        helper.write_text('int helper(int n){return n*7+3;}\n')
        for architecture in ("x86_64", "aarch64"):
            with self.subTest(architecture=architecture):
                objects = []
                for path in (source, helper):
                    output = self.work / (architecture + "-" + path.stem + ".o")
                    result = self.driver("--target=" + architecture + "-unknown-linux-gnu", "-c", str(path), "-o", str(output))
                    self.assertEqual(result.returncode, 0, result.stderr)
                    objects.append(str(output))
                image = self.work / (architecture + "-shared.so")
                result = self.command([linker, "-shared", "--build-id=uuid", "-z", "separate-code", "-z", "relro",
                                       "-z", "now", *objects, "-o", str(image)])
                self.assertEqual(result.returncode, 0, result.stderr)
                verify(image, self.llvm / "llvm-objdump", seal=True)
                verify(image, self.llvm / "llvm-objdump")
                symbols = self.command([str(self.llvm / "llvm-nm"), "--dynamic", "--defined-only", str(image)])
                self.assertEqual(symbols.returncode, 0, symbols.stderr)
                self.assertRegex(symbols.stdout, r"\bT helper\b")
                self.assertRegex(symbols.stdout, r"\bT entry\b")
                self.assertNotIn(".llam.body", symbols.stdout)
                self.assertNotIn("__llam_native_identity", symbols.stdout)

    def test_signal_subscription_contracts_preserve_native_thread_affinity(self) -> None:
        source = self.work / "signal-contract.c"
        source.write_text('#include "llam/managed.h"\n'
                          'int create_entry(void *arg){llam_signal_set_t *set=arg;'
                          'return llam_signal_set_create_ex(0,0,0,0,&set);}\n'
                          'int destroy_entry(void *arg){return llam_signal_set_destroy(arg);}\n')
        consumer = self.native_consumer("signal-contract-consumer",
            'int llam_signal_set_create_ex(const int *s,unsigned long n,const void *o,unsigned long z,void **p)'
            '{(void)s;(void)n;(void)o;(void)z;(void)p;return 0;}\n'
            'int llam_signal_set_destroy(void *p){(void)p;return 0;}\n'
            'int create_entry(void*);int destroy_entry(void*);'
            'int main(void){return create_entry(0)||destroy_entry(0)||depth;}\n')
        image = self.work / "signal-contract-native"
        result = self.driver("-I", str(ROOT / "include"), str(source), str(consumer), "-o", str(image))
        self.assertEqual(result.returncode, 0, result.stderr)
        entries = [entry for record in parse_records(read_image(image)) for entry in record.entries]
        self.assertFalse(any(entry.execution_class == 1 for entry in entries))
        self.assertEqual({call.contract_id for entry in entries for call in entry.calls
                          if call.effect == 4 and call.flags == 65}, {56, 59})
        self.assertEqual(self.command([str(image)]).returncode, 0)

    def test_native_affine_runtime_contract_propagates_class_and_bound_slot(self) -> None:
        source = self.work / "affine-contract.c"
        source.write_text('#include "llam/managed.h"\nextern void *native_callback(void *);\n'
                          'int entry(void *arg){void *result=0;int rc=llam_call_native_affine_result(native_callback,arg,&result);\n'
                          'return rc?rc:result!=arg;}\n')
        consumer = self.native_consumer("affine-contract-consumer",
            'void *native_callback(void *arg){return arg;}\n'
            'int llam_call_native_affine_result(void *(*fn)(void*),void *arg,void **out){*out=fn(arg);return 0;}\n'
            'int entry(void*);int main(void){int value=7;return entry(&value)||depth;}\n')
        image = self.work / "affine-contract-native"
        result = self.driver("-I", str(ROOT / "include"), str(source), str(consumer), "-o", str(image))
        self.assertEqual(result.returncode, 0, result.stderr)
        parsed = read_image(image)
        records = parse_records(parsed)
        entries = [entry for record in records for entry in record.entries]
        self.assertTrue(any(entry.execution_class == 2 and entry.effects & 16 for entry in entries))
        self.assertFalse(any(entry.execution_class == 1 for entry in entries))
        self.assertTrue(any(entry.flags == 17 and entry.effects == 16 and
                            any(call.contract_id == 109 and call.effect == 4 and call.flags == 65
                                for call in entry.calls) for entry in entries))
        result = self.command([str(image)])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        affine_record = next(record for record in records if record.entries[0].execution_class == 2)
        data = bytearray(parsed.data)
        struct.pack_into("<I", data, affine_record.file_offset + 128 + 32, 1)
        mutation = self.work / "affine-marked-portable"
        mutation.write_bytes(data)
        with self.assertRaisesRegex(VerificationError, "native-affine"):
            verify(mutation, self.llvm / "llvm-objdump", seal=True)
        runtime_record = next(record for record in records if record.entries[0].flags == 17)
        data = bytearray(parsed.data)
        struct.pack_into("<I", data, runtime_record.file_offset + 128 + 36, 2)
        calls_offset, = struct.unpack_from("<Q", data, runtime_record.file_offset + 96)
        struct.pack_into("<H", data, runtime_record.file_offset + calls_offset + 4, 1)
        mutation = self.work / "affine-marked-runtime-critical"
        mutation.write_bytes(data)
        with self.assertRaisesRegex(VerificationError, "runtime.*contract"):
            verify(mutation, self.llvm / "llvm-objdump", seal=True)


def main() -> None:
    global OPTIONS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-bin", required=True, type=Path)
    parser.add_argument("--plugin", required=True, type=Path)
    OPTIONS, remaining = parser.parse_known_args()
    unittest.main(argv=[sys.argv[0], *remaining])


if __name__ == "__main__":
    main()
