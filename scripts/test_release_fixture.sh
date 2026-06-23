#!/bin/sh
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

set -eu

sha256_file() {
    path="$1"

    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$path" | awk '{ print $1 }'
    elif command -v sha256 >/dev/null 2>&1; then
        sha256 -q "$path"
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$path" | awk '{ print $1 }'
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 -r "$path" | awk '{ print $1 }'
    elif command -v cksum >/dev/null 2>&1; then
        cksum -a sha256 "$path" | awk '{ print $1 }'
    else
        echo "sha256sum, sha256, shasum, openssl, or cksum is required for checksum fixtures" >&2
        exit 1
    fi
}

archive_xz() {
    base_dir="$1"
    package="$2"
    archive="$3"

    if tar -C "$base_dir" -cJf "$archive" "$package" 2>/dev/null; then
        return 0
    fi
    if ! command -v xz >/dev/null 2>&1; then
        echo "tar does not support -J and xz is not available" >&2
        exit 1
    fi
    tar -C "$base_dir" -cf - "$package" | xz -zc > "$archive"
}

write_sha256_sidecar() {
    archive="$1"
    sidecar="$2"
    digest="$(sha256_file "$archive")"

    printf '%s  %s\n' "$digest" "$(basename "$archive")" > "$sidecar"
}

command="${1:-}"
case "$command" in
    archive-xz)
        if [ "$#" -ne 4 ]; then
            echo "usage: $0 archive-xz BASE_DIR PACKAGE ARCHIVE" >&2
            exit 2
        fi
        archive_xz "$2" "$3" "$4"
        ;;
    sha256)
        if [ "$#" -ne 2 ]; then
            echo "usage: $0 sha256 FILE" >&2
            exit 2
        fi
        sha256_file "$2"
        ;;
    sha256-sidecar)
        if [ "$#" -ne 3 ]; then
            echo "usage: $0 sha256-sidecar ARCHIVE SIDECAR" >&2
            exit 2
        fi
        write_sha256_sidecar "$2" "$3"
        ;;
    *)
        echo "usage: $0 {archive-xz|sha256|sha256-sidecar} ..." >&2
        exit 2
        ;;
esac
