#!/bin/sh
set -eu

target="${1:-}"
version="${LLAM_RELEASE_VERSION:-${GITHUB_REF_NAME:-v1.0.0}}"
version="${version#v}"
version="$(printf '%s' "$version" | tr '/' '-')"
abi_major="${LLAM_ABI_MAJOR:-1}"
library_version="${LLAM_VERSION:-1.0.0}"
root_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
out_dir="$root_dir/target/dist"
host_os="$(uname -s)"

if [ -z "$target" ]; then
    os="$host_os"
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

case "$host_os" in
    Darwin) host_target_os="macos" ;;
    Linux) host_target_os="linux" ;;
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

require_input "$root_dir/LICENSE"
require_input "$root_dir/README.md"
require_input "$root_dir/scripts/install.sh"
require_input "$root_dir/scripts/install.ps1"
require_input "$root_dir/scripts/stress_server.py"
require_input "$root_dir/scripts/stress_server_composite.py"
require_input "$root_dir/docs"
require_input "$root_dir/include/llam"
require_input "$root_dir/examples/demo.c"
require_input "$root_dir/examples/bench.c"
require_input "$root_dir/examples/stress.c"
require_input "$root_dir/examples/server.c"
require_input "$root_dir/examples/server_flood.c"
require_input "$root_dir/demo"
require_input "$root_dir/stress"
require_input "$root_dir/bench"
require_input "$root_dir/server"
require_input "$root_dir/server_flood"
require_input "$root_dir/libllam_runtime.a"

case "$host_os" in
    Darwin)
        require_input "$root_dir/libllam_runtime.$abi_major.dylib"
        require_input "$root_dir/libllam_runtime.dylib"
        ;;
    Linux)
        require_input "$root_dir/libllam_runtime.so.$library_version"
        require_input "$root_dir/libllam_runtime.so.$abi_major"
        require_input "$root_dir/libllam_runtime.so"
        ;;
esac

if [ "$missing_inputs" -ne 0 ]; then
    echo "run 'make clean all test' before packaging, or use 'make package'" >&2
    exit 1
fi

package_name="llam-$version-$target"
stage="$out_dir/$package_name"
archive="$out_dir/$package_name.tar.xz"

rm -rf "$stage" "$archive" "$archive.sha256" "$out_dir/$package_name.tar.gz" "$out_dir/$package_name.tar.gz.sha256"
mkdir -p "$stage/bin" "$stage/docs" "$stage/examples" "$stage/include" "$stage/lib" "$stage/scripts"

printf '%s\n' "$version" > "$stage/VERSION"
cp "$root_dir/LICENSE" "$root_dir/README.md" "$stage/"
cp "$root_dir/scripts/install.sh" "$root_dir/scripts/install.ps1" "$stage/"
cp "$root_dir/scripts/stress_server.py" "$root_dir/scripts/stress_server_composite.py" "$stage/scripts/"
cp -R "$root_dir/docs/." "$stage/docs/"
cp -R "$root_dir/include/llam" "$stage/include/"
cp "$root_dir/examples/demo.c" "$root_dir/examples/bench.c" "$root_dir/examples/stress.c" "$root_dir/examples/server.c" "$root_dir/examples/server_flood.c" "$stage/examples/"
cp "$root_dir/demo" "$root_dir/stress" "$root_dir/bench" "$root_dir/server" "$root_dir/server_flood" "$stage/bin/"
cp "$root_dir/libllam_runtime.a" "$stage/lib/"

case "$host_os" in
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
        echo "unsupported release packaging host: $host_os" >&2
        exit 1
        ;;
esac

LLAM_VERSION="$library_version" LLAM_ABI_MAJOR="$abi_major" \
    "$root_dir/scripts/generate_sdk_metadata.sh" "$stage" "$target"

tar -C "$out_dir" -cJf "$archive" "$package_name"
if command -v sha256sum >/dev/null 2>&1; then
    (cd "$out_dir" && sha256sum "$(basename "$archive")" > "$(basename "$archive").sha256")
else
    (cd "$out_dir" && shasum -a 256 "$(basename "$archive")" > "$(basename "$archive").sha256")
fi

printf '%s\n' "$archive"
