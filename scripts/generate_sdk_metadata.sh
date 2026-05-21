#!/bin/sh
# Copyright 2026 Feralthedogg
# SPDX-License-Identifier: Apache-2.0

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: generate_sdk_metadata.sh <stage-dir> [target]" >&2
    exit 2
fi

stage="$1"
target="${2:-}"
version="${LLAM_VERSION:-1.2.0}"
abi_major="${LLAM_ABI_MAJOR:-1}"

if [ -z "$target" ]; then
    case "$(uname -s)" in
        Darwin) target="macos-$(uname -m)" ;;
        Linux) target="linux-$(uname -m)" ;;
        *) target="$(uname -s)-$(uname -m)" ;;
    esac
fi

case "$target" in
    linux-*)
        private_libs="-pthread -luring -lm"
        ;;
    macos-*|darwin-*)
        private_libs="-pthread"
        ;;
    *)
        private_libs="-pthread"
        ;;
esac

mkdir -p "$stage/lib/pkgconfig" "$stage/share/llam/cmake"

cat > "$stage/lib/pkgconfig/llam.pc" <<EOF
prefix=\${pcfiledir}/../..
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: LLAM
Description: Stackful user-thread runtime for C applications
Version: $version
Libs: -L\${libdir} -lllam_runtime
Libs.private: $private_libs
Cflags: -I\${includedir}
EOF

cat > "$stage/share/llam/cmake/llam-config.cmake" <<'EOF'
include(CMakeFindDependencyMacro)

find_dependency(Threads)

get_filename_component(_LLAM_PREFIX "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/llam-targets.cmake")

set(llam_FOUND TRUE)
EOF

cat > "$stage/share/llam/cmake/llam-targets.cmake" <<'EOF'
set(_LLAM_PLATFORM_LIBS Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _LLAM_PLATFORM_LIBS uring m)
endif()

if(NOT TARGET llam::runtime)
    add_library(llam::runtime STATIC IMPORTED)
    set_target_properties(llam::runtime PROPERTIES
        IMPORTED_LOCATION "${_LLAM_PREFIX}/lib/libllam_runtime.a"
        INTERFACE_INCLUDE_DIRECTORIES "${_LLAM_PREFIX}/include"
        INTERFACE_LINK_LIBRARIES "${_LLAM_PLATFORM_LIBS}"
    )
endif()

set(_LLAM_SHARED_LOCATION "")
foreach(_LLAM_CANDIDATE
    "${_LLAM_PREFIX}/lib/libllam_runtime.dylib"
    "${_LLAM_PREFIX}/lib/libllam_runtime.so"
)
    if(EXISTS "${_LLAM_CANDIDATE}")
        set(_LLAM_SHARED_LOCATION "${_LLAM_CANDIDATE}")
        break()
    endif()
endforeach()

if(_LLAM_SHARED_LOCATION AND NOT TARGET llam::runtime_shared)
    add_library(llam::runtime_shared SHARED IMPORTED)
    set_target_properties(llam::runtime_shared PROPERTIES
        IMPORTED_LOCATION "${_LLAM_SHARED_LOCATION}"
        INTERFACE_INCLUDE_DIRECTORIES "${_LLAM_PREFIX}/include"
        INTERFACE_LINK_LIBRARIES "${_LLAM_PLATFORM_LIBS}"
    )
endif()

unset(_LLAM_CANDIDATE)
unset(_LLAM_PLATFORM_LIBS)
unset(_LLAM_SHARED_LOCATION)
EOF

cat > "$stage/share/llam/cmake/llam-config-version.cmake" <<EOF
set(PACKAGE_VERSION "$version")

if(PACKAGE_FIND_VERSION_MAJOR STREQUAL "" OR PACKAGE_FIND_VERSION_MAJOR EQUAL $abi_major)
    if(PACKAGE_FIND_VERSION VERSION_LESS_EQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_COMPATIBLE TRUE)
    endif()
    if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
EOF
