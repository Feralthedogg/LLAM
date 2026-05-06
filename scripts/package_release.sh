#!/bin/sh
set -eu

target="${1:-}"
version="${LLAM_RELEASE_VERSION:-${GITHUB_REF_NAME:-v0.1.0}}"
version="${version#v}"
version="$(printf '%s' "$version" | tr '/' '-')"
abi_major="${LLAM_ABI_MAJOR:-1}"
library_version="${LLAM_VERSION:-1.0.0}"
root_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out_dir="$root_dir/target/dist"

if [ -z "$target" ]; then
    os="$(uname -s)"
    arch="$(uname -m)"
    case "$os" in
        Darwin) os_name="macos" ;;
        Linux) os_name="linux" ;;
        *) os_name="$(printf '%s' "$os" | tr '[:upper:]' '[:lower:]')" ;;
    esac
    case "$arch" in
        x86_64|amd64) arch_name="x86_64" ;;
        arm64|aarch64) arch_name="aarch64" ;;
        *) arch_name="$arch" ;;
    esac
    target="$os_name-$arch_name"
fi

package_name="llam-$version-$target"
stage="$out_dir/$package_name"
archive="$out_dir/$package_name.tar.gz"

rm -rf "$stage" "$archive" "$archive.sha256"
mkdir -p "$stage/bin" "$stage/docs" "$stage/examples" "$stage/include" "$stage/lib"

printf '%s\n' "$version" > "$stage/VERSION"
cp "$root_dir/LICENSE" "$root_dir/README.md" "$stage/"
cp -R "$root_dir/docs/." "$stage/docs/"
cp -R "$root_dir/include/llam" "$stage/include/"
cp "$root_dir/include/nm_runtime.h" "$root_dir/include/nm_platform.h" "$stage/include/"
cp "$root_dir/examples/demo.c" "$root_dir/examples/bench.c" "$root_dir/examples/stress.c" "$stage/examples/"
cp "$root_dir/demo" "$root_dir/stress" "$root_dir/bench" "$stage/bin/"
cp "$root_dir/libllam_runtime.a" "$stage/lib/"

case "$(uname -s)" in
    Darwin)
        cp "$root_dir/libllam_runtime.$abi_major.dylib" "$stage/lib/"
        cp -P "$root_dir/libllam_runtime.dylib" "$stage/lib/"
        ;;
    Linux)
        cp "$root_dir/libllam_runtime.so.$library_version" "$stage/lib/"
        cp -P "$root_dir/libllam_runtime.so.$abi_major" "$stage/lib/"
        cp -P "$root_dir/libllam_runtime.so" "$stage/lib/"
        ;;
    *)
        echo "unsupported release packaging host: $(uname -s)" >&2
        exit 1
        ;;
esac

tar -C "$out_dir" -czf "$archive" "$package_name"
if command -v sha256sum >/dev/null 2>&1; then
    (cd "$out_dir" && sha256sum "$(basename "$archive")" > "$(basename "$archive").sha256")
else
    (cd "$out_dir" && shasum -a 256 "$(basename "$archive")" > "$(basename "$archive").sha256")
fi

printf '%s\n' "$archive"
