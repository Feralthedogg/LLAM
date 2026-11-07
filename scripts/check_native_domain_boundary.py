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

"""Check public native-domain gates before TLS, pins, allocation and locks."""

from __future__ import annotations

import argparse
import functools
import hashlib
from pathlib import Path
import re

try:
    import audit_shared_exports as exports
    import check_legacy_lease_boundary as lease
except ImportError:
    from scripts import audit_shared_exports as exports
    from scripts import check_legacy_lease_boundary as lease

ROOT = Path(__file__).resolve().parent.parent

# Full token fingerprints for genuinely context-free APIs and the signal-safe
# guard primitives and M/G-owned public critical transitions. The latter use
# authoritative native bindings, never inherited scheduler TLS as permission.
# No filename-wide or function-name-only exemption is granted.
REVIEWED = {('src/io/api/wait_options.c', 'llam_wait_opts_init'): '137a70a2ca1bd22f047bd317cce548e3220d0bd5c791d0c54a9e9d406c16af46',
 ('src/core/base/abi.c', 'llam_abi_version'): '96a408ba14c62a2195d474d56f38a201dd26323f6d7ed9c0fe29316002197d7d',
 ('src/core/base/abi.c', 'llam_version_string'): '59fbd42a9de07c6c72fbf101a27209b87d61e338d29ba6582236a57ed1b6db0b',
 ('src/core/base/abi.c', 'llam_abi_get_info'): '647b0d41ef0a54f2fe8007f626c848217bc217058c3fd923dd6648ba665f3c99',
 ('src/core/base/abi.c', 'llam_runtime_opts_init'): 'b104b930eb9a9413fa6ceb546a80042812fd9d670f835703a2c5c3941f9c4896',
 ('src/core/base/abi.c', 'llam_spawn_opts_init'): '399025c8dbc12dccfb306d89adf0b21b254d1c7ddd2b35d6663bd19b7c4d60f8',
 ('src/core/time/time.c', 'llam_now_ns'): 'acf08755e53d41c6119691005193d5c5fd27ca2b51f5ec3d2a48d01115c3506d',
 ('src/io/api/owned.c', 'llam_io_buffer_opts_init'): 'bb6e6dd0a912558c37d1d68f0ef236761f4ccb3fe6696ca9ef5c4890202f3ac4',
 # BSD-only syscall branches removed; supported native syscalls are unchanged.
 ('src/core/preempt/native_domain.c', 'llam_native_domain_id'): 'cf1c8dc89f8f746f47b4eff98fee14ae653f34defc04866a0a48e795a5a49693',
 ('src/core/preempt/native_domain.c', 'llam_native_domain_hash'): '04e2c581b3ea88a67b7473eff9cdfb20d2a43f3d81b79823fbfe2c3882338195',
 ('src/core/preempt/native_domain.c', 'llam_native_domain_lookup'): '6e195846dfe7e4c089553f032abad2632da4198cffb75007e8574d812e2142b3',
 ('src/core/preempt/native_domain.c', 'llam_native_domain_guarded_worker'): '9161bd21fcae13848831f162f3c18c28625053ae3e93d3543f59a697241725bf',
 ('src/core/preempt/native_domain.c', 'llam_managed_native_enter'): 'ffe979aeadb0ad56bc8cda070f3d2a91a49fd99d0edf5a05ce19c3304a627310',
 ('src/core/preempt/native_domain.c', 'llam_managed_native_leave'): '9c59defc3e56522cd4dc0a34f707f129cbf7fe817b28ef24a4cb0335a76e2c2c',
 ('src/internal/managed_domain.h', 'llam_native_domain_blocked'): '6533309355ad61a82c2cea5ac4c068712ca6bd6481bf99ce992c30e78f366e2d',
 ('src/core/preempt/native_domain.c', 'llam_native_domain_bind'): 'c3d0c78772230f36efb1019fadeae26a004e17a47314ec534b6b6874e224014c',
 ('src/core/preempt/native_domain.c', 'llam_native_domain_unbind'): 'a4b73bef2a8b89636779d4ca4df6557c302dfa946c11b9624ab6a00b02f1d279',
 ('src/core/preempt/native_domain.c', 'llam_native_domain_worker_init'): 'f61c596bd9041856acd63c3e15e08cb61188120689aa428c305955e3cb4ac52c',
 ('src/core/preempt/native_domain.c', 'llam_preempt_region_context'): '648a25cb5a4da3fb633ee66657801a9a798eeecde3b7cfad52196821cefdf991',
 ('src/core/preempt/native_domain.c', 'llam_preempt_region_enter'): 'f08c075a4c46e7137a2267effed1a3b6c51de34c9cd40ed2142ce54934768fec',
 ('src/core/preempt/native_domain.c', 'llam_preempt_region_leave'): '32393142c09796d4aef7435a9d5fdfb796da99e78fb97bdbf72ec259cefdc681',
 ('src/core/preempt/native_domain.c', 'llam_native_domain_enter_worker'): '3943eeb51e9b99cd1e4f588b8b551c9e5a80a31b207c070d4d2cc3933f767195',
 ('src/core/preempt/native_domain.c', 'llam_native_domain_leave_worker'): '04be6c3f116c1d785cd5ad8d02aa44df1d303b4f8092645a198b298264930598'}

GUARD_MACRO = """#define LLAM_NATIVE_API_GUARD(failure) do {
if (LLAM_UNLIKELY(llam_native_domain_blocked())) { errno = ENOTSUP; return failure; }
} while (0)"""


def values(source: str) -> tuple[str, ...]:
    # Assembly strings and literal return values are semantic in this protocol.
    # Preserve them while removing comments; the ownership audit's generic
    # literal masking would miss an altered raw syscall instruction.
    pattern = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    masked = re.sub(pattern, lambda m: " " * len(m[0]) if m[0].startswith("/") else m[0],
                    source, flags=re.S)
    return tuple(token.value for token in lease.phase0._tokens(masked))


@functools.lru_cache(maxsize=512)
def fingerprint(source: str) -> str:
    return hashlib.sha256("\0".join(values(source)).encode()).hexdigest()


def function_spans(source: str) -> list:
    masked = lease.phase0._mask_comments_and_literals(source)
    lines = []
    continued = False
    for line in masked.splitlines(keepends=True):
        directive = continued or line.lstrip().startswith("#")
        continued = directive and line.rstrip().endswith("\\")
        lines.append("".join("\n" if c == "\n" else " " for c in line) if directive else line)
    return lease._function_spans("".join(lines))


@functools.lru_cache(maxsize=512)
def _source_layout(source: str) -> tuple[str, tuple[lease._FunctionSpan, ...]]:
    # Adversarial audits revisit the same translation units. Cache immutable
    # lexical facts by full content; headers and review policy are checked anew.
    return lease.phase0._mask_comments_and_literals(source), tuple(function_spans(source))


def guard_definition(state: str) -> str:
    lines = state.splitlines(keepends=True)
    for index, line in enumerate(lines):
        if not line.startswith("#define LLAM_NATIVE_API_GUARD"):
            continue
        result = line
        while lines[index].rstrip().endswith("\\"):
            index += 1
            if index >= len(lines):
                return ""
            result += lines[index]
        return result.replace("\\\n", "\n")
    return ""


def guarded_prefix(body: str) -> bool:
    tokens = values(body)
    if tokens[:2] == ("LLAM_NATIVE_API_GUARD", "("):
        return ")" in tokens[2:8] and ";" in tokens[3:9]
    prefix = ("if", "(", "LLAM_UNLIKELY", "(", "llam_native_domain_blocked", "(", ")", ")", ")", "{")
    if tokens[:len(prefix)] != prefix:
        return False
    pos = len(prefix)
    while tokens[pos:pos + 2] == ("if", "("):
        row = tokens[pos:pos + 11]
        if len(row) != 11 or row != ("if", "(", row[2], "!=", "NULL", ")", "*", row[2], "=", "NULL", ";"):
            return False
        pos += 11
    return tokens[pos:pos + 9] == ("errno", "=", "ENOTSUP", ";", "return", "-", "1", ";", "}")


def audit(sources: dict[str, str], headers: dict[str, str]) -> list[str]:
    api = set()
    for source in headers.values():
        api.update(exports.PUBLIC_FN_RE.findall(exports.strip_comments(source)))
    errors = []
    found = set()
    reviewed_found = set()
    for path, source in sources.items():
        if path != "src/internal/runtime_state.h" and re.search(
                r"(?m)^\s*#\s*(define|undef)\s+LLAM_NATIVE_API_GUARD\b", source):
            errors.append(f"{path}: public native guard cannot be overridden locally")
        masked, spans = _source_layout(source)
        for fn in spans:
            text = source[fn.start_offset:fn.end_offset]
            key = (path, fn.name)
            expected = REVIEWED.get(key)
            if expected is not None:
                reviewed_found.add(key)
                if fingerprint(text) != expected:
                    errors.append(f"{path}:{fn.start_line}: reviewed native-domain contract changed: {fn.name}")
            if fn.name not in api:
                continue
            found.add(fn.name)
            if expected is not None:
                continue
            opening = masked.find("{", fn.start_offset, fn.end_offset)
            if opening < 0 or not guarded_prefix(source[opening + 1:fn.end_offset - 1]):
                errors.append(f"{path}:{fn.start_line}: {fn.name} must gate before TLS/pins/allocation/locks")
    errors.extend(f"missing public entry definition: {name}" for name in sorted(api - found))
    errors.extend(f"missing reviewed contract: {path}:{name}" for path, name in sorted(set(REVIEWED) - reviewed_found))
    state = sources.get("src/internal/runtime_state.h", "")
    if values(guard_definition(state)) != values(GUARD_MACRO):
        errors.append("native API guard macro changed or disappeared")
    domain = sources.get("src/core/preempt/native_domain.c", "")
    for name, number in (("SLOTS", "4096U"), ("PROBES", "64U"), ("DEPTH_MAX", "65535U")):
        if re.search(rf"(?m)^#define LLAM_NATIVE_DOMAIN_{name}\s+{number}\s*$", domain) is None:
            errors.append(f"native domain {name} bound changed")
    return errors


def load_sources(root: Path) -> tuple[dict[str, str], dict[str, str]]:
    sources = {
        path.relative_to(root).as_posix(): path.read_text(encoding="utf-8")
        for path in (root / "src").rglob("*.c")
    }
    state = root / "src/internal/runtime_state.h"
    sources[state.relative_to(root).as_posix()] = state.read_text(encoding="utf-8")
    domain = root / "src/internal/managed_domain.h"
    sources[domain.relative_to(root).as_posix()] = domain.read_text(encoding="utf-8")
    headers = {
        name: (root / "include/llam" / name).read_text(encoding="utf-8")
        for name in exports.PUBLIC_HEADERS
    }
    return sources, headers


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args()
    errors = audit(*load_sources(args.root))
    for error in errors:
        print(f"error: {error}")
    if not errors:
        print("native domain boundary: all public entries guarded or exact reviewed native contracts")
    return bool(errors)


if __name__ == "__main__":
    raise SystemExit(main())
