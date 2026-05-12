#!/usr/bin/env python3

import argparse
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent
PUBLIC_INCLUDE_DIR = ROOT / "include" / "llam"
INTERNAL_INCLUDE_DIR = ROOT / "src" / "internal"
SOURCE_DIRS = [ROOT / "src", ROOT / "examples"]
FORBIDDEN_STEMS = {"common", "misc", "helper", "helpers", "utils", "manager", "handler", "processor", "data", "logic", "module", "function"}
SOURCE_EXTS = {".c", ".h", ".inc", ".S"}
WARN_LINES = 500
SPLIT_LINES = 800
SIZE_BUDGETS = {
    # Public ABI and intentionally dense runtime internals. These files are
    # still audited: growth beyond the declared budget is reported as warning.
    "examples/bench_support.c": 1100,
    "examples/demo_entry.c": 700,
    "examples/demo_tasks.c": 700,
    "examples/server.c": 1300,
    "examples/server_flood.c": 1100,
    "examples/stress_core_cases.c": 700,
    "examples/stress_dynamic_cases.c": 850,
    "examples/stress_suite.c": 800,
    "examples/stress_support.c": 850,
    "examples/stress_tasks.c": 700,
    "examples/stress_timeout_cases.c": 750,
    "include/llam/runtime.h": 1400,
    "src/core/runtime_alloc.c": 800,
    "src/core/runtime_blocking_api.c": 700,
    "src/core/runtime_channel.c": 1200,
    "src/core/runtime_channel_select.c": 700,
    "src/core/runtime_debug.c": 650,
    "src/core/runtime_init.c": 950,
    "src/core/runtime_platform.c": 650,
    "src/core/runtime_queue.c": 800,
    "src/core/runtime_scheduler.c": 650,
    "src/core/runtime_task_stack.c": 950,
    "src/core/runtime_timer.c": 750,
    "src/core/runtime_wait_tracking.c": 750,
    "src/core/runtime_wake.c": 1100,
    "src/core/runtime_yield_join_sleep.c": 1000,
    "src/engine/runtime_engine.c": 650,
    "src/engine/runtime_watchdog_rehome.c": 900,
    "src/internal/runtime_types.h": 1200,
    "src/internal/runtime_windows_compat.h": 900,
    "src/io/darwin/runtime_io_watch_darwin_migration_live.c": 750,
    "src/io/darwin/runtime_io_watch_darwin_migration_rehome.c": 900,
    "src/io/darwin/runtime_io_watch_darwin_state.c": 800,
    "src/io/linux/runtime_io_watch_linux_cqe.c": 650,
    "src/io/linux/runtime_io_watch_linux_migration_live.c": 750,
    "src/io/linux/runtime_io_watch_linux_migration_rehome.c": 900,
    "src/io/runtime_io_api_blocking_ops.c": 800,
    "src/io/runtime_io_api_direct.c": 850,
    "src/io/runtime_io_api_direct_tuning.c": 850,
    "src/io/runtime_io_api_issue.c": 850,
    "src/io/runtime_io_api_public.c": 1200,
    "src/io/runtime_io_engine.c": 1000,
}

def rel(path: pathlib.Path) -> str:
    return str(path.relative_to(ROOT))


def iter_source_files() -> list[pathlib.Path]:
    files: list[pathlib.Path] = []
    for directory in [ROOT / "include", ROOT / "src", ROOT / "examples"]:
        if not directory.exists():
            continue
        files.extend(path for path in directory.rglob("*") if path.suffix in SOURCE_EXTS)
    return sorted(files)


def line_count(path: pathlib.Path) -> int:
    return len(path.read_text(encoding="utf-8", errors="replace").splitlines())


def public_header_boundary_errors() -> list[str]:
    errors: list[str] = []
    if (ROOT / "include" / "internal").exists():
        errors.append("include/internal must not exist; internal headers live under src/internal")
    for path in (ROOT / "include").rglob("*.h"):
        if PUBLIC_INCLUDE_DIR not in path.parents and path != PUBLIC_INCLUDE_DIR:
            errors.append(f"public header outside include/llam: {rel(path)}")
    if not PUBLIC_INCLUDE_DIR.exists():
        errors.append("missing include/llam public API directory")
    if not INTERNAL_INCLUDE_DIR.exists():
        errors.append("missing src/internal private header directory")
    return errors


def include_boundary_errors(files: list[pathlib.Path]) -> list[str]:
    errors: list[str] = []
    bad_patterns = [
        re.compile(r'#\s*include\s+"(?:\.\./)*include/internal/'),
        re.compile(r'#\s*include\s+"(?:\.\./)*include/nm_'),
    ]
    for path in files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for index, line in enumerate(text.splitlines(), start=1):
            for pattern in bad_patterns:
                if pattern.search(line):
                    errors.append(f"{rel(path)}:{index}: forbidden include path: {line.strip()}")
    return errors


def naming_errors(files: list[pathlib.Path]) -> list[str]:
    errors: list[str] = []
    for path in files:
        if path.suffix != ".c":
            continue
        if path.stem in FORBIDDEN_STEMS:
            errors.append(f"{rel(path)}: forbidden broad filename stem")
    return errors


def line_warnings(files: list[pathlib.Path]) -> list[str]:
    warnings: list[str] = []
    for path in files:
        if path.suffix not in {".c", ".h", ".inc"}:
            continue
        count = line_count(path)
        path_rel = rel(path)
        budget = SIZE_BUDGETS.get(path_rel)
        if budget is not None:
            if count > budget:
                warnings.append(f"{path_rel}: {count} lines, exceeds budget {budget}")
            continue
        if count >= SPLIT_LINES:
            warnings.append(f"{path_rel}: {count} lines, split candidate")
        elif count >= WARN_LINES:
            warnings.append(f"{path_rel}: {count} lines, watch")
    return warnings


def main() -> int:
    parser = argparse.ArgumentParser(description="Audit C project structure and module boundary rules.")
    parser.add_argument("--strict-size", action="store_true", help="Treat 800+ line files as errors.")
    args = parser.parse_args()

    files = iter_source_files()
    errors = []
    errors.extend(public_header_boundary_errors())
    errors.extend(include_boundary_errors(files))
    errors.extend(naming_errors(files))

    warnings = line_warnings(files)
    if args.strict_size:
        errors.extend(warning for warning in warnings if "split candidate" in warning)

    for warning in warnings:
        print(f"warning: {warning}")
    for error in errors:
        print(f"error: {error}", file=sys.stderr)

    if errors:
        print(f"audit failed: {len(errors)} error(s), {len(warnings)} warning(s)", file=sys.stderr)
        return 1
    print(f"audit ok: {len(files)} source/header file(s), {len(warnings)} size warning(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
