#!/bin/sh
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

set -eu

target="${1:-}"
version="${LLAM_RELEASE_VERSION:-${GITHUB_REF_NAME:-v2.0.0}}"
version="${version#v}"
abi_major="${LLAM_ABI_MAJOR:-2}"
library_version="${LLAM_VERSION:-2.0.0}"
script_dir="$(dirname -- "$0")"
root_dir="$(CDPATH='' cd -- "$script_dir/.." && pwd)"
out_dir="$root_dir/target/dist"
host_os="$(uname -s)"

if [ -z "$target" ]; then
    os="$host_os"
    arch="$(uname -m)"
    case "$os" in
        Darwin) os_name="macos" ;;
        Linux) os_name="linux" ;;
        FreeBSD) os_name="freebsd" ;;
        OpenBSD) os_name="openbsd" ;;
        NetBSD) os_name="netbsd" ;;
        DragonFly) os_name="dragonflybsd" ;;
        *) os_name="$(printf '%s' "$os" | LC_ALL=C tr '[:upper:]' '[:lower:]')" ;;
    esac
    case "$arch" in
        x86_64|amd64) arch_name="x86_64" ;;
        arm64|aarch64) arch_name="aarch64" ;;
        *) arch_name="$arch" ;;
    esac
    target="$os_name-$arch_name"
fi

case "$host_os" in
    Darwin) host_target_os="macos" ;;
    Linux) host_target_os="linux" ;;
    FreeBSD) host_target_os="freebsd" ;;
    OpenBSD) host_target_os="openbsd" ;;
    NetBSD) host_target_os="netbsd" ;;
    DragonFly) host_target_os="dragonflybsd" ;;
    *)
        echo "unsupported release packaging host: $host_os" >&2
        exit 1
        ;;
esac

host_arch="$(uname -m)"
case "$host_arch" in
    x86_64|amd64) host_target_arch="x86_64" ;;
    arm64|aarch64) host_target_arch="aarch64" ;;
    *) host_target_arch="$host_arch" ;;
esac

host_target="$host_target_os-$host_target_arch"
if [ "$target" != "$host_target" ]; then
    echo "target $target must be packaged on $host_target, not $host_os/$host_arch" >&2
    exit 1
fi

missing_inputs=0
require_input() {
    path="$1"
    if [ ! -e "$path" ]; then
        echo "missing release input: $path" >&2
        missing_inputs=1
    fi
}

validate_release_component() {
    name="$1"
    value="$2"

    case "$value" in
        ""|*[!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-]*)
            echo "invalid $name: $value" >&2
            echo "$name may contain only ASCII letters, digits, '.', '_', '+', and '-'" >&2
            exit 2
            ;;
    esac
}

validate_safe_stage_tree() {
    root="$1"

    find "$root" ! -type d ! -type f ! -type l -exec sh -c '
        for path do
            echo "refusing special file in release stage: $path" >&2
            exit 1
        done
    ' sh {} +

    find "$root" -type l -exec sh -c '
        for link_path do
            link_target="$(readlink "$link_path")" || exit 1
            case "$link_target" in
                ""|/*|..|../*|*/..|*/../*|*[!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+/-]*)
                    echo "refusing unsafe staged symlink $link_path -> $link_target" >&2
                    exit 1
                    ;;
            esac
        done
    ' sh {} +
    find "$root" ! -type l \( -perm -0002 -o -perm -4000 -o -perm -2000 \) -exec sh -c '
        for path do
            echo "refusing unsafe release stage mode: $path" >&2
            exit 1
        done
    ' sh {} +
}

validate_no_hardlinked_regular_files() {
    root="$1"
    label="$2"

    find "$root" -type f -exec sh -c '
        label="$1"
        shift
        for path do
            if stat -c %h "$path" >/dev/null 2>&1; then
                links="$(stat -c %h "$path")"
            else
                links="$(stat -f %l "$path")"
            fi
            case "$links" in
                ""|*[!0123456789]*)
                    echo "cannot determine link count for $label: $path" >&2
                    exit 1
                    ;;
                0|1) ;;
                *)
                    echo "refusing hard-linked file in $label: $path" >&2
                    exit 1
                    ;;
            esac
        done
    ' sh "$label" {} +
}

validate_safe_symlink_target() {
    path="$1"
    link_target="$(readlink "$path")" || exit 1

    case "$link_target" in
        ""|/*|..|../*|*/..|*/../*|*[!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+/-]*)
            echo "refusing unsafe release input symlink $path -> $link_target" >&2
            exit 1
            ;;
    esac
}

validate_expected_symlink_target() {
    path="$1"
    expected="$2"

    if [ ! -L "$path" ]; then
        echo "refusing non-symlink release library link: $path" >&2
        exit 1
    fi

    link_target="$(readlink "$path")" || exit 1
    if [ "$link_target" != "$expected" ]; then
        echo "refusing unexpected release symlink target $path -> $link_target, expected $expected" >&2
        exit 1
    fi
}

extract_release_archive() {
    archive_path="$1"
    destination="$2"

    if ! tar -xf "$archive_path" -C "$destination" 2>/dev/null; then
        if ! command -v xz >/dev/null 2>&1; then
            echo "tar cannot extract $archive_path directly and xz is not available" >&2
            exit 1
        fi
        xz -dc "$archive_path" | tar -xf - -C "$destination"
    fi
}

validate_packaged_archive_links() (
    archive_path="$1"
    package_root="$2"
    tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/llam-release-archive.XXXXXX")"

    trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM
    extract_release_archive "$archive_path" "$tmp_dir"

    case "$host_os" in
        Darwin)
            validate_expected_symlink_target \
                "$tmp_dir/$package_root/lib/libllam_runtime.dylib" \
                "libllam_runtime.$abi_major.dylib"
            ;;
        Linux|FreeBSD|OpenBSD|NetBSD|DragonFly)
            validate_expected_symlink_target \
                "$tmp_dir/$package_root/lib/libllam_runtime.so.$abi_major" \
                "libllam_runtime.so.$library_version"
            validate_expected_symlink_target \
                "$tmp_dir/$package_root/lib/libllam_runtime.so" \
                "libllam_runtime.so.$abi_major"
            ;;
        *)
            echo "unsupported release archive host: $host_os" >&2
            exit 1
            ;;
    esac
)

validate_release_input_file() {
    path="$1"

    if [ -L "$path" ]; then
        echo "refusing symlink release input path: $path" >&2
        exit 1
    fi
    if [ ! -f "$path" ]; then
        echo "refusing non-file release input path: $path" >&2
        exit 1
    fi
    validate_no_hardlinked_regular_files "$path" "release input path"
}

validate_release_input_tree() {
    root="$1"

    if [ -L "$root" ]; then
        echo "refusing symlink release input path: $root" >&2
        exit 1
    fi
    if [ ! -d "$root" ]; then
        echo "refusing non-directory release input path: $root" >&2
        exit 1
    fi

    find "$root" ! -type d ! -type f -exec sh -c '
        for path do
            echo "refusing special release input path: $path" >&2
            exit 1
        done
    ' sh {} +
    validate_no_hardlinked_regular_files "$root" "release input tree"
}

validate_release_input_link_or_file() {
    path="$1"

    if [ -L "$path" ]; then
        validate_safe_symlink_target "$path"
        return
    fi
    if [ ! -f "$path" ]; then
        echo "refusing non-file release input path: $path" >&2
        exit 1
    fi
    validate_no_hardlinked_regular_files "$path" "release input path"
}

validate_safe_output_path() {
    path="$1"
    allow_leaf_file="${2:-1}"

    case "$path" in
        "$root_dir"/*)
            rel="${path#"$root_dir"/}"
            probe="$root_dir"
            while [ -n "$rel" ]; do
                part="${rel%%/*}"
                probe="$probe/$part"
                if [ -L "$probe" ]; then
                    echo "refusing symlink release output path: $probe" >&2
                    exit 1
                fi
                if [ -e "$probe" ] && [ ! -d "$probe" ]; then
                    if [ "$allow_leaf_file" -eq 1 ] && [ "$part" = "$rel" ]; then
                        :
                    else
                        echo "refusing non-directory release output path component: $probe" >&2
                        exit 1
                    fi
                fi
                [ "$part" = "$rel" ] && break
                rel="${rel#*/}"
            done
            ;;
        *)
            echo "release output path escaped repository root: $path" >&2
            exit 1
            ;;
    esac
}

validate_release_component "release version" "$version"
validate_release_component "release target" "$target"
validate_release_component "ABI major" "$abi_major"
validate_release_component "library version" "$library_version"
validate_safe_output_path "$out_dir" 0

require_input "$root_dir/LICENSE"
require_input "$root_dir/README.md"
require_input "$root_dir/CHANGELOG.md"
require_input "$root_dir/scripts/install.sh"
require_input "$root_dir/scripts/install.ps1"
require_input "$root_dir/scripts/stress_server.py"
require_input "$root_dir/scripts/stress_server_composite.py"
require_input "$root_dir/docs"
require_input "$root_dir/include/llam"
require_input "$root_dir/examples"
require_input "$root_dir/demo"
require_input "$root_dir/stress"
require_input "$root_dir/bench"
require_input "$root_dir/server"
require_input "$root_dir/server_lossless"
require_input "$root_dir/server_flood"
require_input "$root_dir/libllam_runtime.a"

case "$host_os" in
    Darwin)
        require_input "$root_dir/libllam_runtime.$abi_major.dylib"
        require_input "$root_dir/libllam_runtime.dylib"
        ;;
    Linux|FreeBSD|OpenBSD|NetBSD|DragonFly)
        require_input "$root_dir/libllam_runtime.so.$library_version"
        require_input "$root_dir/libllam_runtime.so.$abi_major"
        require_input "$root_dir/libllam_runtime.so"
        ;;
esac

if [ "$missing_inputs" -ne 0 ]; then
    echo "run 'make clean all test' before packaging, or use 'make package'" >&2
    exit 1
fi

validate_release_input_file "$root_dir/LICENSE"
validate_release_input_file "$root_dir/README.md"
validate_release_input_file "$root_dir/CHANGELOG.md"
validate_release_input_file "$root_dir/scripts/install.sh"
validate_release_input_file "$root_dir/scripts/install.ps1"
validate_release_input_file "$root_dir/scripts/stress_server.py"
validate_release_input_file "$root_dir/scripts/stress_server_composite.py"
validate_release_input_tree "$root_dir/docs"
validate_release_input_tree "$root_dir/include/llam"
validate_release_input_tree "$root_dir/examples"
validate_release_input_file "$root_dir/demo"
validate_release_input_file "$root_dir/stress"
validate_release_input_file "$root_dir/bench"
validate_release_input_file "$root_dir/server"
validate_release_input_file "$root_dir/server_lossless"
validate_release_input_file "$root_dir/server_flood"
validate_release_input_file "$root_dir/libllam_runtime.a"

case "$host_os" in
    Darwin)
        validate_release_input_file "$root_dir/libllam_runtime.$abi_major.dylib"
        validate_release_input_link_or_file "$root_dir/libllam_runtime.dylib"
        validate_expected_symlink_target "$root_dir/libllam_runtime.dylib" "libllam_runtime.$abi_major.dylib"
        ;;
    Linux|FreeBSD|OpenBSD|NetBSD|DragonFly)
        validate_release_input_file "$root_dir/libllam_runtime.so.$library_version"
        validate_release_input_link_or_file "$root_dir/libllam_runtime.so.$abi_major"
        validate_release_input_link_or_file "$root_dir/libllam_runtime.so"
        validate_expected_symlink_target "$root_dir/libllam_runtime.so.$abi_major" "libllam_runtime.so.$library_version"
        validate_expected_symlink_target "$root_dir/libllam_runtime.so" "libllam_runtime.so.$abi_major"
        ;;
esac

package_name="llam-$version-$target"
stage="$out_dir/$package_name"
archive="$out_dir/$package_name.tar.xz"

validate_safe_output_path "$out_dir" 0
mkdir -p "$out_dir"
validate_safe_output_path "$out_dir" 0
validate_safe_output_path "$stage" 0
validate_safe_output_path "$archive"
validate_safe_output_path "$archive.sha256"
validate_safe_output_path "$out_dir/$package_name.tar.gz"
validate_safe_output_path "$out_dir/$package_name.tar.gz.sha256"
rm -rf "$stage" "$archive" "$archive.sha256" "$out_dir/$package_name.tar.gz" "$out_dir/$package_name.tar.gz.sha256"
mkdir -p "$stage/bin" "$stage/docs" "$stage/examples" "$stage/include" "$stage/lib" "$stage/scripts"
validate_safe_output_path "$stage" 0
validate_safe_stage_tree "$stage"

printf '%s\n' "$version" > "$stage/VERSION"
printf '%s\n' "$abi_major" > "$stage/ABI_MAJOR"
printf '%s\n' "$library_version" > "$stage/LIBRARY_VERSION"
cp "$root_dir/LICENSE" "$root_dir/README.md" "$root_dir/CHANGELOG.md" "$stage/"
cp "$root_dir/scripts/install.sh" "$root_dir/scripts/install.ps1" "$stage/"
cp "$root_dir/scripts/stress_server.py" "$root_dir/scripts/stress_server_composite.py" "$stage/scripts/"
cp -R "$root_dir/docs/." "$stage/docs/"
cp -R "$root_dir/include/llam" "$stage/include/"
cp "$root_dir"/examples/*.c "$root_dir"/examples/*.h "$stage/examples/"
cp "$root_dir/demo" "$root_dir/stress" "$root_dir/bench" "$root_dir/server" "$root_dir/server_lossless" "$root_dir/server_flood" "$stage/bin/"
cp "$root_dir/libllam_runtime.a" "$stage/lib/"

case "$host_os" in
    Darwin)
        cp "$root_dir/libllam_runtime.$abi_major.dylib" "$stage/lib/"
        ln -s "libllam_runtime.$abi_major.dylib" "$stage/lib/libllam_runtime.dylib"
        validate_expected_symlink_target "$stage/lib/libllam_runtime.dylib" "libllam_runtime.$abi_major.dylib"
        ;;
    Linux|FreeBSD|OpenBSD|NetBSD|DragonFly)
        cp "$root_dir/libllam_runtime.so.$library_version" "$stage/lib/"
        ln -s "libllam_runtime.so.$library_version" "$stage/lib/libllam_runtime.so.$abi_major"
        ln -s "libllam_runtime.so.$abi_major" "$stage/lib/libllam_runtime.so"
        validate_expected_symlink_target "$stage/lib/libllam_runtime.so.$abi_major" "libllam_runtime.so.$library_version"
        validate_expected_symlink_target "$stage/lib/libllam_runtime.so" "libllam_runtime.so.$abi_major"
        ;;
    *)
        echo "unsupported release packaging host: $host_os" >&2
        exit 1
        ;;
esac

LLAM_VERSION="$library_version" LLAM_ABI_MAJOR="$abi_major" \
    "$root_dir/scripts/generate_sdk_metadata.sh" "$stage" "$target"

validate_safe_stage_tree "$stage"

if ! tar -C "$out_dir" -cJf "$archive" "$package_name" 2>/dev/null; then
    rm -f "$archive"
    if ! command -v xz >/dev/null 2>&1; then
        echo "tar does not support -J and xz is not available" >&2
        exit 1
    fi
    tar -C "$out_dir" -cf - "$package_name" | xz -z -c > "$archive"
fi

validate_packaged_archive_links "$archive" "$package_name"

if command -v sha256sum >/dev/null 2>&1; then
    (cd "$out_dir" && sha256sum "$(basename "$archive")" > "$(basename "$archive").sha256")
elif command -v shasum >/dev/null 2>&1; then
    (cd "$out_dir" && shasum -a 256 "$(basename "$archive")" > "$(basename "$archive").sha256")
elif command -v sha256 >/dev/null 2>&1; then
    archive_base="$(basename "$archive")"
    digest="$(cd "$out_dir" && sha256 -q "$archive_base")"
    printf '%s  %s\n' "$digest" "$archive_base" > "$archive.sha256"
elif command -v openssl >/dev/null 2>&1; then
    (cd "$out_dir" && openssl dgst -sha256 -r "$(basename "$archive")" > "$(basename "$archive").sha256")
elif command -v cksum >/dev/null 2>&1; then
    archive_base="$(basename "$archive")"
    digest="$(cd "$out_dir" && cksum -a sha256 "$archive_base" | awk '{ print $1 }')"
    printf '%s  %s\n' "$digest" "$archive_base" > "$archive.sha256"
else
    echo "sha256sum, shasum, sha256, openssl, or cksum is required to write release checksums" >&2
    exit 1
fi

printf '%s\n' "$archive"
