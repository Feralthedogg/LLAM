#!/bin/sh
set -eu

prefix="/usr/local"
dry_run=0
force=0

usage() {
    cat <<'EOF'
usage: ./install.sh [--prefix <dir>] [--dry-run] [--force]

Installs the LLAM release archive contents into the selected prefix.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --prefix)
            if [ "$#" -lt 2 ]; then
                echo "--prefix requires a value" >&2
                exit 2
            fi
            prefix="$2"
            shift 2
            ;;
        --prefix=*)
            prefix="${1#--prefix=}"
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

src_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

copy_dir() {
    src="$1"
    dst="$2"

    if [ ! -d "$src" ]; then
        return 0
    fi
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

    if [ ! -f "$src" ]; then
        return 0
    fi
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

copy_dir "$src_dir/include" "$prefix/include"
copy_dir "$src_dir/lib" "$prefix/lib"
copy_dir "$src_dir/bin" "$prefix/bin"
copy_dir "$src_dir/share" "$prefix/share"
copy_dir "$src_dir/docs" "$prefix/share/llam/docs"
copy_dir "$src_dir/examples" "$prefix/share/llam/examples"

copy_file "$src_dir/README.md" "$prefix/share/llam/README.md"
copy_file "$src_dir/LICENSE" "$prefix/share/llam/LICENSE"
copy_file "$src_dir/VERSION" "$prefix/share/llam/VERSION"

if [ "$dry_run" -ne 1 ]; then
    echo "installed LLAM into $prefix"
fi
