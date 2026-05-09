#!/bin/sh
set -eu

prefix="/usr/local"
version="${LLAM_INSTALL_VERSION:-1.0.0}"
target=""
base_url=""
dry_run=0
force=0

usage() {
    cat <<'EOF'
usage: install.sh [--prefix <dir>] [--version <version>] [--target <target>] [--base-url <url>] [--dry-run] [--force]

Installs LLAM from a release archive. When run inside an extracted archive it
copies local contents. When run standalone it downloads the matching archive,
verifies its checksum, extracts it, and runs the archive-local installer.
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

src_dir="$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd || pwd)"

copy_dir() {
    src="$1"
    dst="$2"

    [ -d "$src" ] || return 0
    if [ "$dry_run" -eq 1 ]; then
        echo "copy $src -> $dst"
        return 0
    fi
    mkdir -p "$dst"
    cp -R "$src/." "$dst/"
}

copy_file() {
    src="$1"
    dst="$2"

    [ -f "$src" ] || return 0
    if [ -e "$dst" ] && [ "$force" -ne 1 ]; then
        echo "refusing to overwrite $dst; pass --force" >&2
        exit 1
    fi
    if [ "$dry_run" -eq 1 ]; then
        echo "copy $src -> $dst"
        return 0
    fi
    mkdir -p "$(dirname -- "$dst")"
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

detect_target() {
    os="$(uname -s)"
    arch="$(uname -m)"

    case "$os" in
        Linux) os_name="linux" ;;
        Darwin) os_name="macos" ;;
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

run_archive_install() {
    copy_dir "$src_dir/include" "$prefix/include"
    copy_dir "$src_dir/lib" "$prefix/lib"
    copy_dir "$src_dir/bin" "$prefix/bin"
    copy_dir "$src_dir/share" "$prefix/share"
    copy_dir "$src_dir/docs" "$prefix/share/llam/docs"
    copy_dir "$src_dir/examples" "$prefix/share/llam/examples"
    copy_dir "$src_dir/scripts" "$prefix/share/llam/scripts"

    copy_file "$src_dir/README.md" "$prefix/share/llam/README.md"
    copy_file "$src_dir/LICENSE" "$prefix/share/llam/LICENSE"
    copy_file "$src_dir/VERSION" "$prefix/share/llam/VERSION"

    [ "$dry_run" -eq 1 ] || echo "installed LLAM into $prefix"
}

run_standalone_install() {
    release_version="${version#v}"
    target="${target:-$(detect_target)}"
    base_url="${base_url:-https://github.com/Feralthedogg/LLAM/releases/download/$version}"
    package="llam-$release_version-$target"
    archive="$package.tar.xz"

    if [ "$dry_run" -eq 1 ]; then
        echo "download $base_url/$archive"
        echo "download $base_url/$archive.sha256"
        echo "verify $archive.sha256"
        echo "extract $archive"
        echo "run $package/install.sh --prefix $prefix"
        return 0
    fi

    tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/llam-install.XXXXXX")"
    trap 'rm -rf "$tmp_dir"' EXIT HUP INT TERM

    download_file "$base_url/$archive" "$tmp_dir/$archive"
    download_file "$base_url/$archive.sha256" "$tmp_dir/$archive.sha256"

    (
        cd "$tmp_dir"
        if command -v sha256sum >/dev/null 2>&1; then
            sha256sum -c "$archive.sha256"
        else
            shasum -a 256 -c "$archive.sha256"
        fi
        tar -xf "$archive"
    )

    args="--prefix"
    if [ "$force" -eq 1 ]; then
        sh "$tmp_dir/$package/install.sh" "$args" "$prefix" --force
    else
        sh "$tmp_dir/$package/install.sh" "$args" "$prefix"
    fi
}

if [ -f "$src_dir/include/llam/runtime.h" ] && [ -d "$src_dir/lib" ]; then
    run_archive_install
else
    run_standalone_install
fi
