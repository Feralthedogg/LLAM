#!/usr/bin/env python3
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

"""Argparse numeric validators shared by LLAM CI/stress helpers."""

from __future__ import annotations

import argparse
import math
import os
import re
from collections.abc import Callable


_UNSIGNED_INT_RE = re.compile(r"[0-9]+")
_SIGNED_INT_RE = re.compile(r"-?[0-9]+")
_UNSIGNED_FLOAT_RE = re.compile(r"(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][0-9]+)?")


def _require_unsigned_decimal(raw: str, label: str) -> str:
    if not _UNSIGNED_INT_RE.fullmatch(raw):
        raise argparse.ArgumentTypeError(f"must be a {label}")
    return raw


def _require_signed_decimal(raw: str) -> str:
    if not _SIGNED_INT_RE.fullmatch(raw):
        raise argparse.ArgumentTypeError("must be an integer")
    return raw


def _require_unsigned_float(raw: str, label: str) -> str:
    # Python's float() accepts whitespace, signs, and underscores; CLI/env
    # parsers reject those forms so diagnostics cannot be bypassed by coercion.
    if not _UNSIGNED_FLOAT_RE.fullmatch(raw):
        raise argparse.ArgumentTypeError(f"must be a {label}")
    return raw


def boolean_flag(raw: str) -> bool:
    value = raw.strip().lower()

    if value in {"1", "true", "yes", "on"}:
        return True
    if value in {"0", "false", "no", "off"}:
        return False
    raise argparse.ArgumentTypeError("must be one of: 0, 1, true, false, yes, no, on, off")


def finite_positive_float(raw: str) -> float:
    _require_unsigned_float(raw, "finite positive number")
    try:
        value = float(raw)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a finite positive number") from exc
    if not math.isfinite(value) or value <= 0.0:
        raise argparse.ArgumentTypeError("must be a finite positive number")
    return value


def finite_positive_float_at_most(maximum: float) -> Callable[[str], float]:
    def convert(raw: str) -> float:
        value = finite_positive_float(raw)
        if value > maximum:
            raise argparse.ArgumentTypeError(f"must be a finite positive number <= {maximum:g}")
        return value

    return convert


def finite_nonnegative_float(raw: str) -> float:
    _require_unsigned_float(raw, "finite non-negative number")
    try:
        value = float(raw)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a finite non-negative number") from exc
    if not math.isfinite(value) or value < 0.0:
        raise argparse.ArgumentTypeError("must be a finite non-negative number")
    return value


def finite_nonnegative_float_at_most(maximum: float) -> Callable[[str], float]:
    def convert(raw: str) -> float:
        value = finite_nonnegative_float(raw)
        if value > maximum:
            raise argparse.ArgumentTypeError(f"must be a finite non-negative number <= {maximum:g}")
        return value

    return convert


def positive_int(raw: str) -> int:
    _require_unsigned_decimal(raw, "positive integer")
    try:
        value = int(raw, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a positive integer") from exc
    if value <= 0:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return value


def positive_int_at_most(maximum: int) -> Callable[[str], int]:
    def convert(raw: str) -> int:
        value = positive_int(raw)
        if value > maximum:
            raise argparse.ArgumentTypeError(f"must be a positive integer <= {maximum}")
        return value

    return convert


def nonnegative_int(raw: str) -> int:
    _require_unsigned_decimal(raw, "non-negative integer")
    try:
        value = int(raw, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be a non-negative integer") from exc
    if value < 0:
        raise argparse.ArgumentTypeError("must be a non-negative integer")
    return value


def nonnegative_int_at_most(maximum: int) -> Callable[[str], int]:
    def convert(raw: str) -> int:
        value = nonnegative_int(raw)
        if value > maximum:
            raise argparse.ArgumentTypeError(f"must be a non-negative integer <= {maximum}")
        return value

    return convert


def integer(raw: str) -> int:
    _require_signed_decimal(raw)
    try:
        return int(raw, 10)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("must be an integer") from exc


def integer_at_least(minimum: int) -> Callable[[str], int]:
    def convert(raw: str) -> int:
        value = integer(raw)
        if value < minimum:
            raise argparse.ArgumentTypeError(f"must be an integer >= {minimum}")
        return value

    return convert


def env_default(
    parser: argparse.ArgumentParser,
    name: str,
    default: str,
    converter: Callable[[str], object],
) -> object:
    raw = os.getenv(name)
    value = default if raw is None or raw == "" else raw
    try:
        return converter(value)
    except (argparse.ArgumentTypeError, ValueError) as exc:
        parser.error(f"{name} {exc}")
