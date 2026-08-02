#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
# Copyright 2026 Feralthedogg
"""Emit the deterministic phase-0 CONNECT-to-WRITE AOT C fixture."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import sys


SEMANTIC_CONTRACT = (
    "leir.phase0:v1;"
    "CONNECT(0,1,2,3)->WRITE(0,4,5,6)->RETURN(6);"
    "ERROR->FAIL"
).encode("ascii")

HEADER = """// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

#ifndef LLAM_EXPERIMENTS_LEIR_GENERATED_CONNECT_WRITE_H
#define LLAM_EXPERIMENTS_LEIR_GENERATED_CONNECT_WRITE_H

#include "leir_aot_module.h"

#define LEIR_AOT_CONNECT_WRITE_CONNECT_ERROR 1U
#define LEIR_AOT_CONNECT_WRITE_WRITE_RESULT 2U

#ifdef __cplusplus
extern "C" {
#endif

extern const leir_aot_module_v1_t
    leir_aot_connect_write_module_v1;

#ifdef __cplusplus
}
#endif

#endif
"""


def semantic_digest() -> str:
    """Return the first 64 bits of the canonical contract SHA-256."""
    return hashlib.sha256(SEMANTIC_CONTRACT).hexdigest()[:16]


def source_text() -> str:
    """Return the generated specialization wrapper."""
    return f"""// SPDX-License-Identifier: LicenseRef-LLAM-Commercial-Reciprocity-1.0
// Copyright 2026 Feralthedogg

/**
 * @file experiments/leir/generated/leir_aot_connect_write.c
 * @brief Generated direct AOT module for LEIR CONNECT followed by WRITE.
 */

#define LEIR_AOT_CONNECT_WRITE_DIGEST \\
    UINT64_C(0x{semantic_digest()})

#include "leir_aot_connect_write_template.inc"
"""


def outputs() -> dict[str, str]:
    return {
        "leir_aot_connect_write.c": source_text(),
        "leir_aot_connect_write.h": HEADER,
    }


def check_outputs(output_dir: Path) -> int:
    stale = False

    for name, expected in outputs().items():
        path = output_dir / name
        try:
            actual = path.read_text(encoding="utf-8")
        except OSError:
            actual = ""
        if actual != expected:
            print(f"stale generated file: {path}", file=sys.stderr)
            stale = True
    return 1 if stale else 0


def write_outputs(output_dir: Path) -> int:
    output_dir.mkdir(parents=True, exist_ok=True)
    for name, content in outputs().items():
        (output_dir / name).write_text(content, encoding="utf-8")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)

    if args.check:
        return check_outputs(args.output_dir)
    return write_outputs(args.output_dir)


if __name__ == "__main__":
    raise SystemExit(main())
