#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Safe helpers for CI/test output files.

The scripts in this directory write benchmark results and timeout logs based on
command-line or workflow-provided paths.  Refuse symlink parent components,
symlink leaf paths, hard-linked leaf paths, and non-regular files so a
compromised workspace cannot redirect or alias artifact writes.
"""

from __future__ import annotations

import os
import errno
import stat
import argparse
import sys
from pathlib import Path
from typing import BinaryIO, TextIO


_DARWIN_ROOT_SYMLINKS = {
    Path("/var"): Path("/private/var"),
    Path("/tmp"): Path("/private/tmp"),
    Path("/etc"): Path("/private/etc"),
}
_CAN_USE_DIR_FD = (
    os.name != "nt"
    and os.open in os.supports_dir_fd
    and os.mkdir in os.supports_dir_fd
    and os.stat in os.supports_dir_fd
    and os.stat in os.supports_follow_symlinks
)


def _is_reparse_point(st: os.stat_result) -> bool:
    return bool(
        hasattr(st, "st_file_attributes")
        and hasattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT")
        and (st.st_file_attributes & stat.FILE_ATTRIBUTE_REPARSE_POINT) != 0
    )


def _is_allowed_system_symlink(path: Path) -> Path | None:
    target = _DARWIN_ROOT_SYMLINKS.get(path)

    if target is None:
        return None
    try:
        if path.resolve(strict=True) == target:
            return target
    except OSError:
        return None
    return None


def _directory_flags() -> int:
    flags = os.O_RDONLY

    if hasattr(os, "O_DIRECTORY"):
        flags |= os.O_DIRECTORY
    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    return flags


def _write_flags() -> int:
    flags = os.O_WRONLY | os.O_CREAT

    if hasattr(os, "O_CLOEXEC"):
        flags |= os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        flags |= os.O_NOFOLLOW
    return flags


def _rewrite_allowed_system_root(path: Path) -> Path:
    if not path.is_absolute():
        return path
    parts = path.parts
    if len(parts) < 2:
        return path

    alias = Path(parts[0]) / parts[1]
    resolved = _is_allowed_system_symlink(alias)
    if resolved is None:
        return path
    return resolved.joinpath(*parts[2:])


def _ensure_safe_parent_path(path: Path) -> None:
    parent = path.parent

    if str(parent) in ("", "."):
        return
    if path.is_absolute():
        current = Path(parent.anchor)
        parts = parent.parts[1:]
    else:
        current = Path.cwd()
        parts = parent.parts

    if any(part == ".." for part in parts):
        raise RuntimeError(f"refusing parent traversal in output path: {path}")

    for part in parts:
        if part in ("", "."):
            continue
        current = current / part
        try:
            st = current.lstat()
        except FileNotFoundError:
            current.mkdir()
            st = current.lstat()
        if stat.S_ISLNK(st.st_mode) or _is_reparse_point(st):
            resolved = _is_allowed_system_symlink(current)
            if resolved is None:
                raise RuntimeError(f"refusing symlink output directory or reparse point: {current}")
            current = resolved
            continue
        if not stat.S_ISDIR(st.st_mode):
            raise RuntimeError(f"refusing non-directory output parent: {current}")


def _validate_existing_leaf_path(path: Path) -> None:
    try:
        st = path.lstat()
    except FileNotFoundError:
        return
    except OSError as exc:
        raise RuntimeError(f"cannot inspect output path {path}: {exc}") from exc
    if stat.S_ISLNK(st.st_mode) or _is_reparse_point(st):
        raise RuntimeError(f"refusing symlink output path or reparse point: {path}")
    if not stat.S_ISREG(st.st_mode):
        raise RuntimeError(f"refusing non-regular output path: {path}")
    if getattr(st, "st_nlink", 1) > 1:
        raise RuntimeError(f"refusing hard-linked output path: {path}")


def _prepare_output_path_fallback(path: Path) -> None:
    if path.name in ("", ".", ".."):
        raise RuntimeError(f"refusing invalid output path: {path}")
    if ".." in path.parts:
        raise RuntimeError(f"refusing parent traversal in output path: {path}")
    _ensure_safe_parent_path(path)
    _validate_existing_leaf_path(path)


def _open_parent_dir_fd(path: Path) -> tuple[int, str]:
    normalized = _rewrite_allowed_system_root(path)
    leaf = normalized.name

    if leaf in ("", ".", ".."):
        raise RuntimeError(f"refusing invalid output path: {path}")

    parent = normalized.parent
    if normalized.is_absolute():
        dir_fd = os.open(parent.anchor, _directory_flags())
        parts = parent.parts[1:]
    else:
        dir_fd = os.open(".", _directory_flags())
        parts = parent.parts

    try:
        for part in parts:
            if part in ("", "."):
                continue
            if part == "..":
                raise RuntimeError(f"refusing parent traversal in output path: {path}")

            try:
                next_fd = os.open(part, _directory_flags(), dir_fd=dir_fd)
            except FileNotFoundError:
                os.mkdir(part, 0o755, dir_fd=dir_fd)
                next_fd = os.open(part, _directory_flags(), dir_fd=dir_fd)
            except OSError as exc:
                try:
                    st = os.stat(part, dir_fd=dir_fd, follow_symlinks=False)
                except OSError:
                    st = None
                if st is not None and stat.S_ISLNK(st.st_mode):
                    raise RuntimeError(f"refusing symlink output directory: {path}") from exc
                if st is not None and not stat.S_ISDIR(st.st_mode):
                    raise RuntimeError(f"refusing non-directory output parent: {path}") from exc
                raise RuntimeError(f"refusing unsafe output directory {part!r} in {path}: {exc}") from exc

            os.close(dir_fd)
            dir_fd = next_fd
        return dir_fd, leaf
    except BaseException:
        os.close(dir_fd)
        raise


def _validate_existing_leaf(parent_fd: int, leaf: str, path: Path) -> None:
    try:
        st = os.stat(leaf, dir_fd=parent_fd, follow_symlinks=False)
    except FileNotFoundError:
        return
    except OSError as exc:
        raise RuntimeError(f"cannot inspect output path {path}: {exc}") from exc
    if stat.S_ISLNK(st.st_mode):
        raise RuntimeError(f"refusing symlink output path: {path}")
    if not stat.S_ISREG(st.st_mode):
        raise RuntimeError(f"refusing non-regular output path: {path}")
    if getattr(st, "st_nlink", 1) > 1:
        raise RuntimeError(f"refusing hard-linked output path: {path}")


def _validate_and_truncate_output_fd(fd: int, path: Path) -> int:
    st = os.fstat(fd)
    if not stat.S_ISREG(st.st_mode):
        raise RuntimeError(f"refusing non-regular output path: {path}")
    if getattr(st, "st_nlink", 1) > 1:
        raise RuntimeError(f"refusing hard-linked output path: {path}")
    os.ftruncate(fd, 0)
    return fd


def _open_fd_for_write(path: Path) -> int:
    if not _CAN_USE_DIR_FD:
        _prepare_output_path_fallback(path)
        fd = os.open(path, _write_flags(), 0o644)
        try:
            return _validate_and_truncate_output_fd(fd, path)
        except BaseException:
            os.close(fd)
            raise

    parent_fd, leaf = _open_parent_dir_fd(path)

    try:
        _validate_existing_leaf(parent_fd, leaf, path)
        try:
            fd = os.open(leaf, _write_flags(), 0o644, dir_fd=parent_fd)
        except OSError as exc:
            if exc.errno == errno.ELOOP:
                raise RuntimeError(f"refusing symlink output path: {path}") from exc
            raise RuntimeError(f"cannot open safe output path {path}: {exc}") from exc
    finally:
        os.close(parent_fd)

    try:
        return _validate_and_truncate_output_fd(fd, path)
    except BaseException:
        os.close(fd)
        raise


def prepare_output_path(path: Path) -> Path:
    if not _CAN_USE_DIR_FD:
        _prepare_output_path_fallback(path)
        return path

    parent_fd, _leaf = _open_parent_dir_fd(path)
    try:
        _validate_existing_leaf(parent_fd, _leaf, path)
    finally:
        os.close(parent_fd)
    return path


def prepare_output_dir(path: Path) -> Path:
    """Create and validate an output directory without following symlinks.

    CI shell steps often write logs through `tee`, which cannot use this
    module's safe file descriptors directly.  Preparing the directory through a
    sentinel child still gives those shell writers the same parent-component
    protection as `open_text_for_write`.
    """

    sentinel = path / ".safe-output-probe"
    prepare_output_path(sentinel)
    return path


def open_text_for_write(
    path: Path,
    *,
    encoding: str = "utf-8",
    errors: str | None = None,
    newline: str | None = None,
) -> TextIO:
    fd = _open_fd_for_write(path)
    return open(fd, "w", encoding=encoding, errors=errors, newline=newline, closefd=True)


def open_binary_for_write(path: Path) -> BinaryIO:
    fd = _open_fd_for_write(path)
    return open(fd, "wb", closefd=True)


def write_text_safely(path: Path, text: str, *, encoding: str = "utf-8") -> None:
    with open_text_for_write(path, encoding=encoding) as handle:
        handle.write(text)


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare LLAM CI output paths safely.")
    parser.add_argument(
        "--prepare-dir",
        action="append",
        type=Path,
        default=[],
        help="create and validate an output directory without symlink traversal",
    )
    parser.add_argument(
        "--prepare-file",
        action="append",
        type=Path,
        default=[],
        help="create and validate an output file's parent directory",
    )
    args = parser.parse_args()

    try:
        for path in args.prepare_dir:
            prepare_output_dir(path)
        for path in args.prepare_file:
            prepare_output_path(path)
    except (OSError, RuntimeError) as exc:
        print(exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
