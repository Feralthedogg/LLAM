#!/bin/sh
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

set -eu

prefix="/usr/local"
version="${LLAM_INSTALL_VERSION:-2.0.0}"
target=""
base_url=""
dry_run=0
force=0

usage() {
    cat <<'EOF'
usage: install.sh [--prefix <dir>] [--version <version>] [--target <target>] [--base-url <url>] [--dry-run] [--force]

Installs LLAM from a release archive. When run inside an extracted archive it
copies local contents. When run standalone it downloads the matching archive,
verifies its checksum, extracts it, and installs the extracted tree without
executing archive-supplied installer code.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)
            [ "$#" -ge 2 ] || { echo "--prefix requires a value" >&2; exit 2; }
            prefix="$2"
            shift 2
            ;;
        --prefix=*)
            prefix="${1#--prefix=}"
            shift
            ;;
        --version)
            [ "$#" -ge 2 ] || { echo "--version requires a value" >&2; exit 2; }
            version="$2"
            shift 2
            ;;
        --version=*)
            version="${1#--version=}"
            shift
            ;;
        --target)
            [ "$#" -ge 2 ] || { echo "--target requires a value" >&2; exit 2; }
            target="$2"
            shift 2
            ;;
        --target=*)
            target="${1#--target=}"
            shift
            ;;
        --base-url)
            [ "$#" -ge 2 ] || { echo "--base-url requires a value" >&2; exit 2; }
            base_url="$2"
            shift 2
            ;;
        --base-url=*)
            base_url="${1#--base-url=}"
            shift
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        --force)
            force=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

if [ -z "$prefix" ]; then
    echo "invalid prefix: empty" >&2
    exit 2
fi
while [ "$prefix" != "/" ] && [ "${prefix%/}" != "$prefix" ]; do
    prefix="${prefix%/}"
done

script_dir="$(dirname -- "$0")"
script_name="$(basename -- "$0")"
src_dir="$(CDPATH='' cd -- "$script_dir" 2>/dev/null && pwd)"
archive_mode=0
if [ "$script_name" = "install.sh" ] &&
    [ -f "$src_dir/install.sh" ] &&
    [ -f "$src_dir/include/llam/runtime.h" ] &&
    [ -d "$src_dir/lib" ]; then
    archive_mode=1
fi

copy_dir() {
    src="$1"
    dst="$2"

    [ -d "$src" ] || return 0
    validate_destination_path "$dst"
    if [ "$force" -ne 1 ] && [ -d "$dst" ]; then
        # Existing system include/lib/bin directories are normal. Only refuse
        # paths that this archive would actually overwrite.
        (cd "$src" && find . \( -type f -o -type l \) -exec sh -c '
            dst="$1"
            shift
            for rel do
                rel="${rel#./}"
                if [ -e "$dst/$rel" ] || [ -L "$dst/$rel" ]; then
                    echo "refusing to overwrite $dst/$rel; pass --force" >&2
                    exit 1
                fi
            done
        ' sh "$dst" {} +)
    fi
    (cd "$src" && find . \( -type f -o -type l \) -print) | while IFS= read -r rel; do
        rel="${rel#./}"
        validate_overwrite_target "$dst/$rel"
    done
    if [ "$dry_run" -eq 1 ]; then
        echo "copy $src -> $dst"
        return 0
    fi
    mkdir -p "$dst"
    validate_destination_path "$dst"
    cp -R "$src/." "$dst/"
}

copy_file() {
    src="$1"
    dst="$2"

    [ -f "$src" ] || return 0
    validate_destination_path "$dst"
    if [ -e "$dst" ] && [ "$force" -ne 1 ]; then
        echo "refusing to overwrite $dst; pass --force" >&2
        exit 1
    fi
    validate_overwrite_target "$dst"
    if [ "$dry_run" -eq 1 ]; then
        echo "copy $src -> $dst"
        return 0
    fi
    dst_parent="$(dirname -- "$dst")"
    mkdir -p "$dst_parent"
    validate_destination_path "$dst_parent"
    validate_destination_path "$dst"
    cp "$src" "$dst"
}

download_file() {
    url="$1"
    out="$2"

    if command -v curl >/dev/null 2>&1; then
        curl -fL "$url" -o "$out"
    elif command -v wget >/dev/null 2>&1; then
        wget -O "$out" "$url"
    else
        echo "curl or wget is required for standalone install" >&2
        exit 1
    fi
}

sha256_file() {
    path="$1"

    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$path" | awk '{ print $1 }'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$path" | awk '{ print $1 }'
    elif command -v sha256 >/dev/null 2>&1; then
        sha256 -q "$path"
    elif command -v openssl >/dev/null 2>&1; then
        openssl dgst -sha256 -r "$path" | awk '{ print $1 }'
    elif command -v cksum >/dev/null 2>&1; then
        cksum -a sha256 "$path" | awk '{ print $1 }'
    else
        echo "sha256sum, shasum, sha256, openssl, or cksum is required for checksum verification" >&2
        exit 1
    fi
}

tar_list_archive() {
    archive_path="$1"

    if tar -tf "$archive_path" 2>/dev/null; then
        return 0
    fi
    if command -v xz >/dev/null 2>&1; then
        xz -dc "$archive_path" | tar -tf -
        return $?
    fi
    tar -tf "$archive_path"
}

tar_verbose_archive() {
    archive_path="$1"

    if tar -tvf "$archive_path" 2>/dev/null; then
        return 0
    fi
    if command -v xz >/dev/null 2>&1; then
        xz -dc "$archive_path" | tar -tvf -
        return $?
    fi
    tar -tvf "$archive_path"
}

tar_extract_archive() {
    archive_path="$1"

    if tar -xf "$archive_path" 2>/dev/null; then
        return 0
    fi
    if command -v xz >/dev/null 2>&1; then
        xz -dc "$archive_path" | tar -xf -
        return $?
    fi
    tar -xf "$archive_path"
}

verify_archive_checksum() {
    archive_path="$1"
    checksum_path="$2"
    archive_name="$3"
    checksum_nonempty_count="$(awk 'NF { count++ } END { print count + 0 }' "$checksum_path")"
    checksum_field_count="$(awk 'NF { print NF; exit }' "$checksum_path")"
    expected="$(awk 'NF { print $1; exit }' "$checksum_path")"
    checksum_target="$(awk 'NF { print $2; exit }' "$checksum_path")"
    actual="$(sha256_file "$archive_path")"

    if [ "$checksum_nonempty_count" -ne 1 ]; then
        echo "invalid checksum file for $archive_name" >&2
        exit 1
    fi
    # Release .sha256 sidecars are generated as exactly two fields:
    # "<hex digest> <archive name>".  Reject trailing fields or extra lines so
    # standalone installs do not accept ambiguous checksum manifests.
    # Use awk field extraction instead of shell splitting: shell glob expansion
    # would otherwise turn a malicious "*.tar.xz" target into the real archive.
    if [ "$checksum_field_count" -ne 2 ]; then
        echo "invalid checksum file for $archive_name" >&2
        exit 1
    fi
    expected_lower="$(printf '%s' "$expected" | LC_ALL=C tr '[:upper:]' '[:lower:]')"
    actual_lower="$(printf '%s' "$actual" | LC_ALL=C tr '[:upper:]' '[:lower:]')"

    case "$expected" in
        *[!0123456789abcdefABCDEF]*|"")
            echo "invalid checksum digest for $archive_name" >&2
            exit 1
            ;;
    esac
    if [ "${#expected}" -ne 64 ]; then
        echo "invalid checksum length for $archive_name" >&2
        exit 1
    fi
    case "$checksum_target" in
        ""|"$archive_name") ;;
        \*"$archive_name") ;;
        *)
            echo "checksum target $checksum_target does not match $archive_name" >&2
            exit 1
            ;;
    esac
    if [ "$actual_lower" != "$expected_lower" ]; then
        echo "checksum mismatch for $archive_name" >&2
        exit 1
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

install_tree_version() {
    tree_root="$1"

    if [ -f "$tree_root/VERSION" ] && [ ! -L "$tree_root/VERSION" ]; then
        sed -n '1p' "$tree_root/VERSION"
    else
        printf '%s\n' "$version"
    fi
}

install_tree_value() {
    tree_root="$1"
    file_name="$2"
    fallback="$3"

    if [ -f "$tree_root/$file_name" ] && [ ! -L "$tree_root/$file_name" ]; then
        sed -n '1p' "$tree_root/$file_name"
    else
        printf '%s\n' "$fallback"
    fi
}

release_major() {
    name="$1"
    value="$2"

    validate_release_component "$name" "$value"
    major="${value%%.*}"
    case "$major" in
        ""|*[!0123456789]*)
            echo "invalid $name major: $value" >&2
            exit 2
            ;;
    esac
    printf '%s\n' "$major"
}

numeric_metadata_value() {
    name="$1"
    value="$2"

    validate_release_component "$name" "$value"
    case "$value" in
        ""|*[!0123456789]*)
            echo "invalid $name: $value" >&2
            exit 2
            ;;
    esac
    printf '%s\n' "$value"
}

has_llam_runtime_lib_artifact() {
    tree_root="$1"

    [ -d "$tree_root/lib" ] || return 1
    first_artifact="$(find "$tree_root/lib" \( -type f -o -type l \) -name 'libllam_runtime*' -print 2>/dev/null | sed -n '1p')"
    [ -n "$first_artifact" ]
}

validate_llam_runtime_lib_link_shape() {
    tree_root="$1"
    install_abi_major="$2"

    for link_path in "$tree_root/lib/libllam_runtime.dylib" "$tree_root/lib/libllam_runtime.so"; do
        if { [ -e "$link_path" ] || [ -L "$link_path" ]; } && [ ! -L "$link_path" ]; then
            echo "refusing non-symlink LLAM library link in install tree: $link_path" >&2
            exit 1
        fi
    done

    if [ -n "$install_abi_major" ]; then
        link_path="$tree_root/lib/libllam_runtime.so.$install_abi_major"
        if { [ -e "$link_path" ] || [ -L "$link_path" ]; } && [ ! -L "$link_path" ]; then
            echo "refusing non-symlink LLAM library link in install tree: $link_path" >&2
            exit 1
        fi
    fi
}

is_allowed_system_symlink_path() {
    path="$1"

    case "$(uname -s):$path" in
        Darwin:/tmp|Darwin:/var|Darwin:/etc)
            resolved="$(CDPATH='' cd -- "$path" 2>/dev/null && pwd -P)" || return 1
            [ "$resolved" = "/private${path}" ]
            ;;
        *)
            return 1
            ;;
    esac
}

validate_no_symlink_components() {
    label="$1"
    path="$2"

    if [ -z "$path" ]; then
        echo "invalid $label: empty path" >&2
        exit 2
    fi

    case "$path" in
        /*)
            probe="/"
            rel="${path#/}"
            ;;
        *)
            probe="."
            rel="$path"
            ;;
    esac

    while [ -n "$rel" ]; do
        part="${rel%%/*}"
        if [ "$part" = "$rel" ]; then
            rel=""
        else
            rel="${rel#*/}"
        fi
        case "$part" in
            ""|.) continue ;;
            ..)
                echo "refusing parent traversal in $label: $path" >&2
                exit 1
                ;;
        esac

        if [ "$probe" = "/" ]; then
            probe="/$part"
        else
            probe="$probe/$part"
        fi

        if [ -L "$probe" ]; then
            if is_allowed_system_symlink_path "$probe"; then
                continue
            fi
            echo "refusing symlink component in $label: $probe" >&2
            exit 1
        fi
        if [ -e "$probe" ] && [ ! -d "$probe" ] && [ -n "$rel" ]; then
            echo "refusing non-directory component in $label: $probe" >&2
            exit 1
        fi
        if [ ! -e "$probe" ]; then
            break
        fi
    done
}

validate_archive_members() {
    archive_path="$1"
    package_dir="$2"
    members_file="$tmp_dir/archive.members"
    canonical_file="$tmp_dir/archive.members.canonical"
    canonical_fold_file="$tmp_dir/archive.members.canonical.folded"
    verbose_file="$tmp_dir/archive.verbose"
    duplicate_file="$tmp_dir/archive.duplicates"

    tar_list_archive "$archive_path" > "$members_file"
    : > "$canonical_file"
    while IFS= read -r member; do
        case "$member" in
            *[!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+/-]*)
                echo "refusing archive member with unsafe name characters: $member" >&2
                return 1
                ;;
        esac
        case "$member" in
            ""|/*|..|../*|*/..|*/../*)
                echo "refusing unsafe archive member: $member" >&2
                return 1
                ;;
        esac
        case "$member" in
            .|./*|*/.|*/./*|*//*)
                echo "refusing non-canonical archive member: $member" >&2
                return 1
                ;;
        esac
        case "$member" in
            "$package_dir"|"$package_dir/"|"$package_dir"/*) ;;
            *)
                echo "refusing archive member outside $package_dir: $member" >&2
                return 1
            ;;
        esac
        canonical_member="$member"
        while [ "$canonical_member" != "/" ] &&
            [ "${canonical_member%/}" != "$canonical_member" ]; do
            canonical_member="${canonical_member%/}"
        done
        printf '%s\n' "$canonical_member" >> "$canonical_file"
        printf '%s\n' "$canonical_member" | LC_ALL=C tr '[:upper:]' '[:lower:]' >> "$canonical_fold_file"
    done < "$members_file"
    LC_ALL=C sort "$canonical_file" | uniq -d > "$duplicate_file"
    if [ -s "$duplicate_file" ]; then
        duplicate_member="$(sed -n '1p' "$duplicate_file")"
        echo "refusing duplicate archive member: $duplicate_member" >&2
        return 1
    fi
    LC_ALL=C sort "$canonical_fold_file" | uniq -d > "$duplicate_file"
    if [ -s "$duplicate_file" ]; then
        duplicate_member="$(sed -n '1p' "$duplicate_file")"
        echo "refusing case-insensitive duplicate archive member: $duplicate_member" >&2
        return 1
    fi

    tar_verbose_archive "$archive_path" > "$verbose_file"
    while IFS= read -r line; do
        mode_field="${line%% *}"
        type_char="${line%"${line#?}"}"
        case "$type_char" in
            -|d)
                case "$mode_field" in
                    ?*[sStT]*|?????w*|????????w*)
                        echo "refusing unsafe archive member mode: $line" >&2
                        return 1
                        ;;
                esac
                ;;
            l)
                case "$line" in
                    *" -> "*) link_target="${line##* -> }" ;;
                    *" link to "*) link_target="${line##* link to }" ;;
                    *)
                        echo "refusing archive link with unknown target: $line" >&2
                        return 1
                        ;;
                esac
                case "$link_target" in
                    ""|/*|..|../*|*/..|*/../*|*[!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+/-]*)
                        echo "refusing unsafe archive link target: $link_target" >&2
                        return 1
                        ;;
                esac
                ;;
            h)
                echo "refusing hard-link archive member: $line" >&2
                return 1
                ;;
            *)
                echo "refusing special archive member: $line" >&2
                return 1
                ;;
        esac
    done < "$verbose_file"
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

validate_safe_install_tree() {
    root="$1"
    tree_version=""
    tree_library_version=""
    tree_abi_major=""

    if has_llam_runtime_lib_artifact "$root"; then
        tree_version="$(install_tree_version "$root")"
        tree_library_version="$(install_tree_value "$root" "LIBRARY_VERSION" "$tree_version")"
        tree_abi_major="$(install_tree_value "$root" "ABI_MAJOR" "$(release_major "install tree library version" "$tree_library_version")")"
        tree_abi_major="$(numeric_metadata_value "install tree ABI major" "$tree_abi_major")"
        validate_release_component "install tree library version" "$tree_library_version"
    fi
    validate_llam_runtime_lib_link_shape "$root" "$tree_abi_major"

    find "$root" ! -type d ! -type f ! -type l -exec sh -c '
        for path do
            echo "refusing special file in LLAM install tree: $path" >&2
            exit 1
        done
    ' sh {} +
    validate_no_hardlinked_regular_files "$root" "LLAM install tree"

    find "$root" -type l -exec sh -c '
        root="$1"
        install_abi_major="$2"
        install_library_version="$3"
        shift 3
        for link_path do
            link_target="$(readlink "$link_path")" || exit 1
            case "$link_target" in
                ""|/*|..|../*|*/..|*/../*|*[!abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+/-]*)
                    echo "refusing unsafe symlink $link_path -> $link_target" >&2
                    exit 1
                    ;;
            esac
            rel="${link_path#"$root"/}"
            if [ -z "$install_abi_major" ] || [ -z "$install_library_version" ]; then
                case "$rel" in
                    lib/libllam_runtime*)
                        echo "cannot validate LLAM library symlink without a numeric install tree version: $link_path" >&2
                        exit 1
                        ;;
                esac
            fi
            case "$rel:$link_target" in
                lib/libllam_runtime.dylib:libllam_runtime."$install_abi_major".dylib) ;;
                lib/libllam_runtime.so:libllam_runtime.so."$install_abi_major") ;;
                lib/libllam_runtime.so."$install_abi_major":libllam_runtime.so."$install_library_version") ;;
                *)
                    echo "refusing unexpected symlink in LLAM install tree: $link_path -> $link_target" >&2
                    exit 1
                    ;;
            esac
            case "$link_target" in
                */*)
                    echo "refusing non-local library symlink in LLAM install tree: $link_path -> $link_target" >&2
                    exit 1
                    ;;
            esac
            link_dir="${link_path%/*}"
            target_path="$link_dir/$link_target"
            if [ ! -e "$target_path" ] && [ ! -L "$target_path" ]; then
                echo "refusing dangling symlink in LLAM install tree: $link_path -> $link_target" >&2
                exit 1
            fi
        done
    ' sh "$root" "$tree_abi_major" "$tree_library_version" {} +
}

validate_destination_path() {
    dst="$1"

    validate_no_symlink_components "install prefix" "$prefix"
    validate_no_symlink_components "install destination" "$dst"

    # Refuse symlinked install roots before checking children. Otherwise a
    # safe-looking prefix/include path can still escape through the prefix.
    if [ -L "$prefix" ]; then
        echo "refusing symlink install prefix: $prefix" >&2
        exit 1
    fi

    case "$dst" in
        "$prefix"/*)
            rel="${dst#"$prefix"/}"
            probe="$prefix"
            while [ -n "$rel" ]; do
                part="${rel%%/*}"
                probe="$probe/$part"
                if [ -L "$probe" ]; then
                    echo "refusing symlink destination inside install prefix: $probe" >&2
                    exit 1
                fi
                [ "$part" = "$rel" ] && break
                rel="${rel#*/}"
            done
            ;;
        *)
            if [ -L "$dst" ]; then
                echo "refusing symlink destination: $dst" >&2
                exit 1
            fi
            ;;
    esac
}

file_link_count() {
    path="$1"

    if stat -c %h "$path" >/dev/null 2>&1; then
        stat -c %h "$path"
    else
        stat -f %l "$path"
    fi
}

validate_overwrite_target() {
    dst="$1"

    if [ ! -e "$dst" ] && [ ! -L "$dst" ]; then
        return 0
    fi
    validate_destination_path "$dst"
    if [ -f "$dst" ] && [ ! -L "$dst" ]; then
        links="$(file_link_count "$dst")"
        case "$links" in
            ""|*[!0123456789]*)
                echo "cannot determine link count for destination: $dst" >&2
                exit 1
                ;;
            0|1) ;;
            *)
                echo "refusing hard-linked destination inside install prefix: $dst" >&2
                exit 1
                ;;
        esac
    fi
}

detect_target() {
    os="$(uname -s)"
    arch="$(uname -m)"

    case "$os" in
        Linux) os_name="linux" ;;
        Darwin) os_name="macos" ;;
        FreeBSD) os_name="freebsd" ;;
        OpenBSD) os_name="openbsd" ;;
        NetBSD) os_name="netbsd" ;;
        DragonFly) os_name="dragonflybsd" ;;
        *)
            echo "unsupported OS for LLAM release installer: $os" >&2
            exit 1
            ;;
    esac

    case "$arch" in
        x86_64|amd64) arch_name="x86_64" ;;
        arm64|aarch64) arch_name="aarch64" ;;
        *)
            echo "unsupported architecture for LLAM release installer: $arch" >&2
            exit 1
            ;;
    esac

    printf '%s-%s\n' "$os_name" "$arch_name"
}

run_archive_install_from() {
    install_src_dir="$1"

    validate_safe_install_tree "$install_src_dir"

    copy_dir "$install_src_dir/include" "$prefix/include"
    copy_dir "$install_src_dir/lib" "$prefix/lib"
    copy_dir "$install_src_dir/bin" "$prefix/bin"
    copy_dir "$install_src_dir/share" "$prefix/share"
    copy_dir "$install_src_dir/docs" "$prefix/share/llam/docs"
    copy_dir "$install_src_dir/examples" "$prefix/share/llam/examples"
    copy_dir "$install_src_dir/scripts" "$prefix/share/llam/scripts"

    copy_file "$install_src_dir/README.md" "$prefix/share/llam/README.md"
    copy_file "$install_src_dir/LICENSE" "$prefix/share/llam/LICENSE"
    copy_file "$install_src_dir/CHANGELOG.md" "$prefix/share/llam/CHANGELOG.md"
    copy_file "$install_src_dir/VERSION" "$prefix/share/llam/VERSION"
    copy_file "$install_src_dir/ABI_MAJOR" "$prefix/share/llam/ABI_MAJOR"
    copy_file "$install_src_dir/LIBRARY_VERSION" "$prefix/share/llam/LIBRARY_VERSION"

    [ "$dry_run" -eq 1 ] || echo "installed LLAM into $prefix"
}

run_archive_install() {
    run_archive_install_from "$src_dir"
}

run_standalone_install() {
    release_tag="$version"
    release_version="${version#v}"
    target="${target:-$(detect_target)}"
    validate_release_component "version" "$release_version"
    validate_release_component "target" "$target"
    case "$release_tag" in
        v*) ;;
        *) release_tag="v$release_tag" ;;
    esac
    base_url="${base_url:-https://github.com/Feralthedogg/LLAM/releases/download/$release_tag}"
    package="llam-$release_version-$target"
    archive="$package.tar.xz"

    if [ "$dry_run" -eq 1 ]; then
        echo "download $base_url/$archive"
        echo "download $base_url/$archive.sha256"
        echo "verify $archive.sha256"
        echo "extract $archive"
        echo "install extracted $package -> $prefix"
        return 0
    fi

    tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/llam-install.XXXXXX")"
    trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

    download_file "$base_url/$archive" "$tmp_dir/$archive"
    download_file "$base_url/$archive.sha256" "$tmp_dir/$archive.sha256"

    (
        cd "$tmp_dir"
        verify_archive_checksum "$archive" "$archive.sha256" "$archive"
        validate_archive_members "$archive" "$package"
        tar_extract_archive "$archive"
    )

    if [ ! -f "$tmp_dir/$package/install.sh" ] || [ -L "$tmp_dir/$package/install.sh" ]; then
        echo "archive does not contain a regular $package/install.sh" >&2
        exit 1
    fi
    validate_safe_install_tree "$tmp_dir/$package"
    # Do not execute installer code from the downloaded archive.  The current
    # verified installer performs archive-local installation directly so a
    # compromised payload cannot turn a valid-looking archive into arbitrary
    # script execution.
    run_archive_install_from "$tmp_dir/$package"
}

if [ "$archive_mode" -eq 1 ]; then
    run_archive_install
else
    run_standalone_install
fi
